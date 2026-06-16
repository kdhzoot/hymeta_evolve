# HyMeta Sim — Implementation

C++ simulator for RocksDB HyMeta metadata-scheme selection (Full / Partitioned /
Unify), used as the evaluator for OpenEvolve-driven per-SST policy search.

## Architecture

```
hymeta_evolve/
├── initial_program.cpp     # EVOLVE-BLOCK: select_scheme() only
├── harness/                # fixed infrastructure (not evolved)
│   ├── sim_engine.{hpp,cpp}    # LSMTree + HyperClockCache + IOModel + DiskQueue
│   ├── workload.{hpp,cpp}      # uniform + scrambled Zipfian
│   ├── layout_loader.{hpp,cpp} # sst_layout.json parser
│   └── sim_main.cpp            # CLI; emits JSON on stdout
├── evaluator.py            # OpenEvolve fitness (avg us/op over 6 scenarios)
├── sim_validate_28configs.py   # final accuracy validation harness
├── manual_smoke.py         # LLM-free policy ranking
├── bench_results/          # real RocksDB CSVs + calibration logs
├── docs/                   # this file + PRESENTATION_SUMMARY.md
└── scripts/                # real RocksDB measurement orchestrators
```

**Total LoC**: 1,416 fixed harness + **11 LoC evolve target** (`initial_program.cpp`).

## Key components

### `IOModel` ([sim_engine.cpp](../harness/sim_engine.cpp))
Per-block hit/miss cost table calibrated from real RocksDB measurements. Each
Get walks: filter top → filter leaf → (compute) → index top → index leaf →
(compute) → data. Costs come from `IOCostConfig` ([sim_engine.hpp](../harness/sim_engine.hpp)).

### `HyperClockCache` ([sim_engine.cpp](../harness/sim_engine.cpp))
Direct port of RocksDB `FixedHyperClockTable` ([clock_cache.cc](/home/godong/himeta/cache/clock_cache.cc)).
Critical CLOCK protocol matching the upstream:

```cpp
insert:     usage = kLowCountdown = 2     // LOW priority (default)
lookup hit: usage++                        // no cap
CLOCK sweep:
  if (usage == 0) evict
  else            usage = min(usage - 1, kMaxCountdown - 1 = 2)
```

This was the single most impactful fix in the whole project — see
"CLOCK fix" below.

### `DiskQueue` (DES, optional)
`des-mode=1` runs N virtual threads on a shared FIFO disk queue (service time
= block_size / bandwidth). Used to model multi-thread NVMe contention.
**`des-mode=0` (post-hoc bandwidth saturation) is the production setting** —
empirically more accurate at high cache regimes (DES over-promises parallelism
on hot cache; see validation results below).

## Calibration sources

| Quantity | Source | File |
|---|---|---|
| Per-block disk_io latency (μs) | RocksDB `block_latency_collector.h` instrumentation, cold4 (cache=1, 250GB DB) | `bench_results/cold4_*.log` |
| Per-block cache_lookup latency | warm4 phase ALL_HIT samples | `bench_results/warm4_*.log` |
| Per-scheme compute (μs) | warm4 phase B − sum(cache_lookup A) | `bench_results/warm4_*.log` |
| NVMe bandwidth curve | `fio` 4 KB → 1 MB random read sweep, RAID0 across 4× Samsung 990 Pro | `bench_results/fio_nvme_bw.txt` |
| Partitioned top-level sizes | Custom `IdxPartTop` slot in instrumentation; 5GB shortening on/off comparison | `bench_results/cold_partitioned_5gb_*.log` |

### Latest calibration (5GB no-shortening DB)

```cpp
// Partitioned scheme — top/leaf split in BlockType enum (today's refactor)
disk_io_part_filter_top_us  = 75.4    // FilterPartIdx, ~14.5 KB
disk_io_part_filter_leaf_us = 99.1    //  ~3.6 KB
disk_io_part_index_top_us   = 76.7    // ~14.7 KB  (newly separated)
disk_io_part_index_leaf_us  = 104.9   //  ~4.0 KB
```

Background: until today the simulator used a single `BT_PART_INDEX` bucket
mixing top + leaf reads, biasing the average size to ~2.8 KB and latency to
~85 μs. New per-bucket measurement (added `kSlotIndexPartTop` to
`block_latency_collector.h`, instrumented `partitioned_index_reader.cc` with
`ScopedBlockSlotOverride`) confirmed top is **8.6× larger** than leaf (14.7 KB
vs 1.7 KB at the default `index_shortening_mode=1`; the 14.7 KB matches
the no-shortening case used in the calibration).

## CLOCK fix — the load-bearing bug

Hybrid-policy validation initially showed sim +25–36 % over real. False trails:
priority hypothesis (refuted: `cache_high_pri_pool_ratio=0` in config),
pin-top hypothesis (refuted: default false), curve-fit at 3 M ops (refuted:
real runs ~92 M ops, accuracy collapsed when matched).

Root cause found by reading `clock_cache.cc` directly:

```cpp
// Old sim (wrong): cold entry can be evicted on first sweep
insert: usage = 0
evict:  if (usage == 0) evict; else usage--

// Fixed (matches RocksDB): cold entry survives 3 sweeps
insert: usage = kLowCountdown = 2
evict:  if (usage == 0) evict; else usage = min(usage - 1, 2)
```

**Effect**: 2 % 16T hybrid MAE **18.6 % → 3.5 %** at matched num_ops. Lesson:
read the source; don't curve-fit.

## Validation — 28 configs (final)

Single, definitive validation table. See
[`sim_validate_28configs.py`](../sim_validate_28configs.py) and
[`bench_results/exp_validation_28configs.csv`](../bench_results/exp_validation_28configs.csv).

**Scope**:
- Pure baseline: 12 = {Full, Partitioned, Unify} × {5%, 2%, 1%, 0.5%} cache × 16T
- Hybrid: 16 = {P1, P2, P3, P4} × {2%, 1%, 0.5%, 0.2%} cache × 16T

**Settings**: `num_ops = 10 M`, `--des-mode=0` (post-hoc bandwidth saturation),
`--found-rate=0.63`, uniform workload, seed=42.

| Category | N | MAE us/op | **MAE throughput** | Max |
|---|---|---|---|---|
| Pure | 12 | 3.89 % | **3.79 %** | 14.7 % (Full @ 1 %) |
| Hybrid | 16 | 2.80 % | **3.19 %** | 26.1 % (P2 @ 0.2 %) |
| **All 28** | **28** | **3.26 %** | **3.45 %** | 26.1 % |

### Speed comparison (real vs sim)

| | Real RocksDB (`db_bench --duration=600`) | Sim (10 M ops, parallel pool 8) |
|---|---|---|
| 28 configs | ~4 h 45 min | **41 s** |
| Speedup | — | **~417×** |

### Outliers (2 / 28)

- **Full @ 1 % (-14.7 %)** — Full filter (821 KB) saturates the disk;
  current saturation model under-penalises huge-block IOs.
- **P2 @ 0.2 % (+26.1 %)** — sim CLOCK under-models thrashing at extreme cache
  pressure when L3 is Full (the policy's hardest regime).

The remaining 26 configs are within ±11 %, with hybrid configs at 1 % / 0.5 %
cache effectively perfect (≤ 4 %).

## Policy findings

### `initial_program.cpp` seed

```cpp
static constexpr Scheme kLevelScheme[7] = {
  SCHEME_FULL, SCHEME_FULL, SCHEME_FULL,
  SCHEME_PARTITIONED,                       // L3 — the tunable
  SCHEME_UNIFY, SCHEME_UNIFY, SCHEME_UNIFY,
};
```

The L3 choice is the cache-pressure-sensitive lever. From the 28-config data:

| Cache | Best policy | L3 scheme |
|---|---|---|
| 5 % – 2 % | all_Full (lowest us/op) | Full |
| 2 % – 0.5 % | P2 (3,3) | Full |
| **0.2 %** (extreme) | **P5 (2,2)** | **Unify** |

Crossover at ~0.3–0.4 % cache: L3 is 429 SSTs / ~25 GB; full metadata for L3
≈ 5 GB. Below ~1.25 GB cache (= 0.5 % of 250 GB) the L3-Full layout starts
thrashing.

### Sub-finding — P5 ≈ all_Unify

L0–L2 contain only ~1.3 % of SSTs in this DB, so the "Full at top, Unify
elsewhere" hybrid (P5, also the HyMeta paper default) collapses to all_Unify
in throughput.

## Limitations

- **Static layout**: no compaction / write path simulation.
- **CLOCK at extreme cache pressure**: ±20 % under-estimate when L3 is Full
  and cache < 0.3 %.
- **5GB DB calibration vs 250GB DB**: tops are slightly larger in our 5GB
  no-shortening DB (~14.5 KB) than in the 250GB shortening=1 DB (~1.7 KB).
  This is the deliberate calibration choice — see "Latest calibration" above.
- **Single workload**: uniform only. Zipfian validated only on the policy
  ranking, not on absolute us/op.

## Next steps

- [ ] Run OpenEvolve 150 iterations against the new calibration (needs API key).
- [ ] Top-N evolved policies → cross-validate on real RocksDB (~10 min/policy).
- [ ] Zipfian absolute-accuracy validation (re-measure real, compare).
- [ ] Improve thrashing model at < 0.3 % cache (the P2 outlier).
