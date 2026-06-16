# HyMeta Simulator — Architecture & OpenEvolve Pipeline

Detailed reference for the C++ point-lookup simulator and how it plugs into
the OpenEvolve evolutionary search loop.

> File-and-line references in this doc point at the actual source. Open them
> in your editor while reading.

---

## 1. What the simulator simulates

A single `Get(key)` against a 250 GB RocksDB instance with the **HyMeta**
metadata layout (each SST stores Full / Partitioned / Unify metadata
side-by-side). For every operation we produce a per-op latency in
microseconds; aggregating over many operations we get throughput.

The simulator is **not** an instruction-accurate replay. It is a layered
analytical model whose constants are calibrated from real RocksDB
measurements (see [§3 Calibration](#3-calibration)). The model has three
moving parts that interact:

1. **LSM tree layout** — which SSTs hold which key range at which level.
2. **Per-block I/O cost model** — cost of reading or hitting each block type.
3. **HyperClock cache** — bit-for-bit port of RocksDB's `FixedHyperClockTable`
   that decides hit vs miss on every block access.

A **policy** (the thing OpenEvolve evolves) chooses which scheme each SST
uses; the model then walks the per-scheme block sequence for each Get.

---

## 2. Code map

```
hymeta_evolve/
├── initial_program.cpp     # EVOLVE-BLOCK: select_scheme()
├── harness/                # fixed infrastructure
│   ├── sim_engine.{hpp,cpp}    # LSMTree + IOModel + HyperClockCache + DiskQueue
│   ├── workload.{hpp,cpp}      # uniform / scrambled-Zipfian key generator
│   ├── layout_loader.{hpp,cpp} # sst_layout.json → vector<SSTFile>
│   └── sim_main.cpp            # CLI entry; emits JSON on stdout
├── CMakeLists.txt          # Release -O3 -march=native, C++17
├── config.yaml             # OpenEvolve config
├── evaluator.py            # OpenEvolve fitness function
├── sim_validate_28configs.py   # final accuracy validation
├── manual_smoke.py         # LLM-free policy ranking
└── bench_results/sst_layout.json   # frozen 250GB layout
```

LoC budget: **1,435** lines of fixed C++ (`harness/`) + **8 lines** that
OpenEvolve actually mutates (`initial_program.cpp`).

---

## 3. Calibration

Every numeric constant in `IOCostConfig` ([sim_engine.hpp](../harness/sim_engine.hpp))
is grounded in a real measurement, not a guess.

| Constant | Measurement source | Method |
|---|---|---|
| `disk_io_*_us` | `bench_results/cold4_*.log` | RocksDB instrumented with `block_latency_collector.h`; cache=1, 100 K reads on the 250 GB DB. |
| `disk_io_part_index_top_us` (separated today) | `bench_results/cold_partitioned_5gb_noshort.log` | Custom `kSlotIndexPartTop` slot in the collector + `ScopedBlockSlotOverride` around top-level loads in `partitioned_index_reader.cc`. |
| `cache_lookup_*_us` | `bench_results/warm4_*.log` | Same instrumentation, ALL_HIT phase samples. |
| `*_compute_us` | derived | `phase_B − Σ cache_lookup_A` over warm4 ALL_HIT samples. |
| `disk_bandwidth_table` | `bench_results/fio_nvme_bw.txt` | `fio` random read sweep, 4 KB → 1 MB, 16 threads, direct I/O, on the same NVMe RAID0. |
| HyperClockCache CLOCK protocol | `/home/godong/himeta/cache/clock_cache.cc` | Direct port of the upstream insert/lookup/sweep logic. |

The CLOCK port is the most load-bearing piece: an early bug
(insert `usage = 0` instead of `kLowCountdown = 2`) caused +25 % over-prediction
of latency on hybrid policies. Fixing it dropped MAE from 18.6 % → 3.5 %.

---

## 4. Per-component walkthrough

### 4.1 `SSTFile` and `LSMTree` ([sim_engine.cpp](../harness/sim_engine.cpp#L494-L539))

Each `SSTFile` holds level, key range, num_keys, and **derived** metadata
sizes. Sizes are computed once in `LSMTree::compute_metadata_sizes()`:

```
keys_per_block = block_size / (key_size + value_size)             ← 4096 / 91 ≈ 45
num_blocks     = ceil(num_keys / keys_per_block)
full_index_size  = num_blocks × (key_size + 8) × shortening(0.37)
full_filter_size = num_keys × bloom_bits / 8                       ← 10 bits/key
part_num_filter_parts = ceil(full_filter_size / metadata_block_size)
part_num_index_parts  = ceil(full_index_size  / metadata_block_size)
part_filter_top_size  = part_num_filter_parts × (key_size + 8)
part_index_top_size   = part_num_index_parts  × (key_size + 8)
unify_num_parts       = ceil((full_index_size + full_filter_size) / metadata_block_size)
unify_top_index_size  = unify_num_parts × (key_size + 8)
```

**Why pre-compute**: the policy may select any scheme per SST, so we precompute
all three layouts and let `IOModel` pick at access time.

`LSMTree::get_ssts_for_key(key, &out)` returns the SSTs whose `[min_key, max_key]`
range covers the given key, level by level. L0 is scanned linearly (overlapping
ranges), L1+ uses binary search on the `min_key`-sorted index.

### 4.2 `IOModel::point_lookup` — the core simulation

For each SST visited in a Get, we walk the access pattern of that SST's
`active_scheme`. For Partitioned (the most complex), the walk is:

```
filter_top → filter_leaf → (compute) → bloom test
   if rejects: return                                  ← negative path stops here
index_top  → index_leaf  → (compute) → data
```

`access_block(cache_key, size, BlockType, sst)`:
1. `cache.lookup(key)` — hit or miss
2. On hit: return `hit_cost_[bt]` (= `cache_lookup_<bt>_us`)
3. On miss: insert into cache, return `disk_io_<bt>_us + cache_insert_us`,
   record `total_miss_bytes += size`, `total_io_count += 1`

The bloom test uses `bloom_false_positive_rate = 0.0082` to randomise rejects
on negative lookups. Positive lookups (`key_in_sst == true`) never reject.

The Full and Unify branches are simpler subsets of this walk (`harness/sim_engine.cpp`
lines 250–330).

### 4.3 `HyperClockCache` ([sim_engine.cpp](../harness/sim_engine.cpp#L9-L130))

A bit-faithful reimplementation of RocksDB's `FixedHyperClockTable`:

```cpp
// Insert (after a miss)
ce.usage = (priority == HIGH) ? kHighCountdown : kLowCountdown;     // HIGH=3, LOW=2
clock_queue_.push_back(key);
current_size += entry.size;
while (current_size > capacity_) evict_one();

// Lookup hit
ce.usage += 1;       // no cap on the way up (RocksDB-faithful)

// Eviction sweep (CLOCK 2nd-chance)
while (true) {
  if (clock_queue_.front()'s usage == 0) {
    actually evict; current_size -= size; break;
  } else {
    usage = min(usage - 1, kMaxCountdown - 1 = 2);   ← cap on the way down
    rotate to back of queue;
  }
}
```

Sharded (default 64 shards): `key & shard_mask` selects the shard, exactly
like RocksDB.

This single component is the dominant accuracy lever. See
[IMPLEMENTATION.md §CLOCK fix](IMPLEMENTATION.md) for the bug-finding story.

### 4.4 Bandwidth saturation — post-hoc model (`des-mode=0`, default)

Single-threaded simulation produces `base_us_per_op`. To predict 16-thread
throughput we apply [a closed-form penalty](../harness/sim_engine.cpp#L477-L491):

```cpp
avg_io_size  = total_miss_bytes / total_io_count       // (bytes / IO)
bw           = bandwidth_for_size(avg_io_size)         // GB/s, fio-calibrated
demand_per_op = total_miss_bytes / op_count
aggregate    = num_threads × (1e6 / base_us) × demand_per_op  // bytes/s
if aggregate <= bw:
  effective_us = base_us                                // not saturated
else:
  effective_us = num_threads × demand_per_op / bw × 1e6 // bandwidth-bound
```

`bandwidth_for_size()` linearly interpolates the fio-measured curve at the
mean per-op miss size. The 25 % efficiency factor (`disk_bandwidth_efficiency`)
accounts for measured RocksDB overhead vs raw fio.

### 4.5 DES mode (`des-mode=1`, optional)

Each operation is dispatched round-robin to one of N virtual threads. Misses
are submitted to a shared `DiskQueue` with service time `block_size / bw`.
Each thread carries `t_now`; `effective_us_per_op` is the timeline length
divided by ops/thread.

DES is more expressive than the post-hoc model but less accurate at large
caches in our validation runs (overshoots when contention is low). **The
production setting is `des-mode=0`**.

### 4.6 Workload generator ([workload.cpp](../harness/workload.cpp))

- `uniform`: `mt19937_64` + `uniform_int_distribution`. Matches `db_bench
  --benchmarks=readrandom`.
- `zipfian`: scrambled-Zipfian (FNV hash of zipf-distributed rank, like YCSB-C).
- `next_key_exists()`: pre-rolls per-op whether the key is present (real DB
  is `fillrandom` so duplicates leave ~63 % unique coverage; default
  `found_rate = 0.63`).

### 4.7 `sim_main.cpp` — the CLI

[Source](../harness/sim_main.cpp). Top-level flow:

```
parse_args → load_sst_layout_json → LSMTree(cfg, ssts)
for each SST: stats = SSTStats(sst); sst.active_scheme = select_scheme(stats)
HyperClockCache cache(capacity)
IOModel io(cfg.io, &cache)
WorkloadGen wg(cfg.wl, max_key)

for i in 0..num_ops:
  key = wg.next_key()
  present = wg.next_key_exists()
  cand = lsm.get_ssts_for_key(key)
  owner = cand.deepest_level if present else null
  for sst in cand:                       # walk levels top-down
    cost += io.point_lookup(sst, key, sst==owner, rng)
    if sst == owner: break

base_us = total_cost / op_count
eff_us  = io.effective_us_per_op(base_us, op_count, num_threads)
print JSON { base_us, eff_us, throughput, hit_rate, ... }
```

The output JSON is the simulator's contract with everything else.
Important fields:

| field | meaning |
|---|---|
| `base_us_per_op` | single-thread per-op latency (no bandwidth saturation) |
| `effective_us_per_op` | with bandwidth saturation folded in (multi-thread) |
| `throughput_ops_sec_total` | `num_threads × 1e6 / effective_us_per_op` (db_bench convention) |
| `cache_hit_rate` | hits / (hits + misses) over the whole run |
| `metadata_hit_rate` | same but excluding data blocks |
| `phase_stats` | per-scheme/phase ALL_HIT/HAD_MISS counts and avg latency |

### 4.8 `select_scheme()` — the EVOLVE-BLOCK

The only piece OpenEvolve mutates, in [`initial_program.cpp`](../initial_program.cpp):

```cpp
#include "sim_engine.hpp"
namespace hymeta {
static constexpr Scheme m[7] = {
  SCHEME_FULL, SCHEME_FULL, SCHEME_FULL,           // L0-L2
  SCHEME_PARTITIONED,                               // L3
  SCHEME_UNIFY, SCHEME_UNIFY, SCHEME_UNIFY,         // L4-L6
};
Scheme select_scheme(const SSTStats& s) {
  int lv = s.level;
  if (lv < 0) return SCHEME_FULL;
  if (lv >= 7) return SCHEME_UNIFY;
  return m[lv];
}
}
```

`SSTStats` exposes `level`, `num_keys`, `full_metadata_size`,
`partitioned_num_partitions`, `unify_num_partitions`, plus stats counters
(currently zero at policy-apply time — dynamic policies would need an
epoch-based re-apply protocol, not implemented today).

Because policies are pure C++ functions of `SSTStats`, OpenEvolve can mutate
them as freely as it wants (introduce new branches, change the array, even
add helper functions). The harness recompiles whatever lands in the file.

---

## 5. Build and run (manual)

```bash
cd /home/godong/hymeta_evolve

# One-time CMake configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j --target sim_main

# Run (250 GB DB, 1 % cache, 16 threads, 3 M ops)
./build/sim_main \
  --layout=bench_results/sst_layout.json \
  --cache-bytes=$((250 * 1024**3 / 100)) \
  --num-threads=16 \
  --num-ops=3000000 \
  --found-rate=0.63 \
  --seed=42 \
  --dist=uniform \
  --des-mode=0
```

Output: a single JSON object on stdout (~250 bytes).

Helpful one-liners:

```bash
# Sanity: 28-config validation against real RocksDB
python3 sim_validate_28configs.py        # ~41s, MAE 3.45% on throughput

# LLM-free policy ranking (5 hand-coded policies)
python3 manual_smoke.py

# Single fitness evaluation (matches OpenEvolve's call)
python3 evaluator.py
```

---

## 6. OpenEvolve pipeline

OpenEvolve is an LLM-driven evolutionary code-search framework. It treats
your source file as the genome and your `evaluator.py` as the fitness
function. Iterations proceed:

```
                ┌──────────────────────────────────────────────────────┐
                │                  OpenEvolve loop                     │
                │                                                      │
                │  1. Sample parents from population (islands, elitism)│
                │  2. LLM rewrites EVOLVE-BLOCK in initial_program.cpp │
                │  3. evaluator.py(candidate)  →  fitness dict         │
                │  4. Insert into population, archive top performers   │
                │  5. Repeat for max_iterations                        │
                └─────────────────────────┬────────────────────────────┘
                                          │ calls
                                          ▼
   evaluator.py:evaluate(program_path)
     ├─ shutil.copyfile(program_path, ./initial_program.cpp)
     ├─ cmake --build build -j4 --target sim_main          ← incremental, ~0.3 s
     ├─ for (cache_pct, threads, weight) in SCENARIOS:
     │    ./build/sim_main --cache-bytes=... --num-threads=... --num-ops=3M ...
     │    parse JSON, accumulate effective_us_per_op
     └─ return {"fitness": -avg_us_per_op, "avg_us_per_op": ..., "per_scenario": [...]}
```

### 6.1 Why this works

Three properties make the C++ simulator a clean OpenEvolve target:

1. **Self-contained per-candidate**: the entire evolved code is in a single
   file (`initial_program.cpp`). OpenEvolve drops in a new file, the
   evaluator rebuilds, runs, returns a number. No cross-file mutations.
2. **Cheap evaluation**: 6 sim runs × ~2 s = ~12 s/iter at 3 M ops. 150
   iterations ≈ 30 minutes wall time (excluding LLM API time).
3. **Good signal**: the simulator has 3.45 % MAE vs real RocksDB on 28
   configs, so a policy that wins in sim is very likely to win on real
   hardware. The fitness landscape is informative.

### 6.2 The evaluator contract ([evaluator.py](../evaluator.py))

```python
# Module-level constants — what the evaluator pins
NUM_OPS    = 3_000_000          # per scenario
FOUND_RATE = 0.63
SEED       = 42
SCENARIOS  = [(2.0,1,1.0), (1.0,1,1.0), (0.5,1,1.0),
              (2.0,16,1.0), (1.0,16,1.0), (0.5,16,1.0)]   # 6 (cache%, threads, weight)
PENALTY    = 1e9                # returned on build/runtime failure

def evaluate(program_path: str) -> dict:
    # 1. Drop candidate into ./initial_program.cpp
    # 2. cmake --build (fail → return {fitness: -PENALTY, error: "build_failed"})
    # 3. For each scenario, run sim_main and parse JSON
    # 4. Aggregate: avg_us_per_op = weighted_mean(effective_us_per_op)
    # 5. Return {"fitness": -avg_us_per_op, "avg_us_per_op": ..., "per_scenario": [...]}
```

OpenEvolve maximises fitness, so we negate `avg_us_per_op` (lower latency = higher
fitness). Build/runtime failures get a huge negative penalty so they're
strongly selected against.

### 6.3 The OpenEvolve config ([config.yaml](../config.yaml))

```yaml
max_iterations: 150
language: cpp
file_suffix: .cpp

llm:
  api_base: "https://api.openai.com/v1"
  primary_model: "gpt-4o-mini"
  secondary_model: "gpt-4o"
  temperature: 0.8

prompt:
  num_top_programs: 3        # keep top-3 in prompt for the LLM to riff on
  num_diverse_programs: 2    # plus 2 diverse picks for exploration

database:
  population_size: 64
  archive_size: 20
  num_islands: 3             # MAP-Elites-style island model
  elite_selection_ratio: 0.1
  exploration_ratio: 0.2
  exploitation_ratio: 0.7
```

Three islands (separate sub-populations that occasionally migrate) help
avoid local optima (e.g., the seed L0–L2 = Full local maximum).

### 6.4 End-to-end run

```bash
cd /home/godong/hymeta_evolve

# One-time build (subsequent OpenEvolve iterations are incremental)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Sanity-check: evaluator on the seed candidate
python3 evaluator.py
# → {"fitness": -<avg_us>, "avg_us_per_op": <avg_us>, "per_scenario": [...]}

# Real run (needs API key)
OPENAI_API_KEY=<your-api-key> openevolve-run \
    initial_program.cpp evaluator.py \
    --config config.yaml \
    --iterations 150 \
    --output evolve_runs
```

OpenEvolve writes `evolve_runs/<timestamp>/` with checkpoints every 25 iters,
the best program, and the full lineage. Each iteration logs the LLM prompt,
the produced diff, and the evaluator's full return dict.

### 6.5 Validating evolved policies

Once OpenEvolve finishes, the top candidates should be cross-checked against
real RocksDB:

```bash
# Pick the best evolved program
cp evolve_runs/<TS>/best_program.cpp initial_program.cpp

# Re-run the 28-config sim validation
python3 sim_validate_28configs.py

# For the very top policies, also do a real-DB run (~10 min/config)
bash scripts/extended_real_16t.sh
```

A policy that wins in sim by ≥5 % is a candidate for upstream HyMeta
adoption; ≤5 % wins are within sim noise and need real-DB confirmation.

---

## 7. Limitations to be aware of

- **Static layout** — no compaction or write path; `sst_layout.json` is a
  point-in-time snapshot of the 250 GB DB.
- **Stats are zero at policy-apply time** — `access_count`, `cache_hit_rate`,
  etc. in `SSTStats` are zeros when `select_scheme()` is called. Workload-
  adaptive policies would need an epoch-based re-apply protocol (not
  implemented).
- **Uniform workload only validated** — Zipfian was implemented but not
  yet validated against real RocksDB at the 28-config level.
- **One DB shape** — calibrated against the specific 250 GB / 3,346 SSTs /
  L1=3 / L2=40 / L3=429 / L4=2,874 layout. A new DB (different size, key
  size, bloom bits) would need re-running `bench_250gb.sh` + `bench_fio_nvme.sh`
  to re-calibrate.

---

## 8. Cheatsheet — common debugging recipes

| symptom | likely cause | check |
|---|---|---|
| sim builds but `effective_us_per_op` looks wrong | calibration drift | re-run `bench_250gb.sh` and diff `cold4_*.log` against current values |
| 16T result == 1T result | DES off + only one thread | verify `--num-threads > 1` and `--des-mode=0` (post-hoc still applies bandwidth penalty) |
| huge error at extreme cache (< 0.3 %) | known CLOCK thrashing limit | accept ±20 %, or improve eviction model |
| OpenEvolve evaluator returns penalty | check `last_build_error.log` | usually a syntax error in the evolved `select_scheme()` |
| `total_miss_bytes / op_count` too low | num_ops too small for cache to warm | increase to ≥ 10 M ops (default 3 M is OpenEvolve-tuned) |

---

## 9. References

- [IMPLEMENTATION.md](IMPLEMENTATION.md) — high-level results and the CLOCK
  bug-finding story.
- [PRESENTATION_SUMMARY.md](PRESENTATION_SUMMARY.md) — 15-minute presentation
  flow including the per-block latency tables and policy crossover findings.
- [bench_results/exp_validation_28configs.csv](../bench_results/exp_validation_28configs.csv) — final 28-config sim vs real comparison.
- RocksDB upstream: `/home/godong/himeta/cache/clock_cache.cc` (the CLOCK
  algorithm we ported).
