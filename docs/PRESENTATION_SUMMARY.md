# HyMeta 정책 시뮬레이터 — 발표 요약 (15 min)

**작성일**: 2026-04-22 · **저자**: Donghyun Ko (Yonsei S3 Lab)
**관련 문서**: [IMPLEMENTATION.md](IMPLEMENTATION.md), [README.md](../README.md)

---

## 스토리라인

1. **문제 → 속도 동기 → 환경** (Part A, 3분): 왜 시뮬레이터가 필요한가
2. **실측 분석** (Part B, 5분): 실측에서 관찰된 latency·scaling·phase·정책 동역학
3. **시뮬레이터 구축 여정** (Part C, 5분): 단순 1T sim → 16T mismatch → fio 측정 → saturation → CLOCK 버그 수정 → 최종 검증
4. **OpenEvolve + 결론** (Part D, 2분): 진화 준비 상태, 교훈

---

# Part A — 동기

## 슬라이드 1 · 문제 제기 (1 min)

**RocksDB HyMeta** 는 한 SST 안에 Full / Partitioned / Unify 3가지 메타데이터를 모두 보관하고, **Runtime에 레벨별로 선택** 가능 (`--himeta_level_preference=full_last,part_last`).

- **Full**: 단일 큰 filter (≈ 840KB) + index (≈ 316KB) 블록
- **Partitioned**: 4KB filter/index 파티션 + top-level 인덱스
- **Unify**: filter+index 통합 4KB 파티션 + top-level

**핵심 질문**: 어느 레벨에 어떤 scheme을 할당해야 최적인가?

- 7 레벨 × 3 scheme = **3⁷ = 2,187 조합**
- 최적값은 cache 크기·workload·found_rate에 따라 달라짐 → 상황마다 재탐색 필요
- 한 정책 실측에 10분 → 2,187 조합 × 10분 = **364시간** → **실측만으로 탐색 불가**

---

## 슬라이드 2 · 왜 시뮬레이터가 필요한가 — 속도 (1.5 min) ⭐

### 실측은 시간 기반 (600초 고정), ops는 정책·cache에 따라 크게 다름

600초 동안 실제로 처리된 ops 개수:

| 시나리오 (16T) | us/op | ops_sec | 600초 총 ops |
|--------------|-------|---------|-------------|
| 2% all_Full (최고 throughput) | 74.5 | 215k | **129 M** |
| 2% P2 hybrid | 102.4 | 156k | 94 M |
| 0.5% P2 | 158.3 | 101k | 61 M |
| 0.2% P5 | 226.4 | 71k | 42 M |
| 0.5% all_Full | 1085 | 15k | 9 M |
| **0.2% all_Full** | **1765** | 9k | **5.4 M** |

→ **24× 편차** (5.4M ~ 129M ops). 1T는 1.5M ~ 8.4M.

### 실측 vs Sim 속도

| 방식 | 1 scenario | 용도 |
|------|-----------|------|
| Real db_bench | **600초 고정** | 최종 검증 (ground truth) |
| Sim 3M ops (OpenEvolve용) | **~2초** | 랭킹 평가 (~300× 가속) |
| Sim 60M ops (accuracy 매칭) | ~30-60초 | 절대값 검증 (~15× 가속) |

### OpenEvolve 탐색 비용

150 iteration × 6 시나리오 fitness 평가:

| 방식 | 1 iter | 150 iter 총 |
|------|--------|------------|
| Real db_bench (600s × 6) | 60분 | **150시간 = 6.25일** |
| **Sim (2s × 6)** | **12초** | **30분** |

**→ 6일 → 30분 = 300× 가속**. OpenEvolve 자동 탐색을 현실화한 핵심.

---

## 슬라이드 3 · 실험 환경 (1 min)

**하드웨어**: AMD Ryzen 9 7950X3D (16C/32T) + Samsung 990 PRO 2TB × 4 NVMe RAID0, Linux 6.17

**RocksDB**: `himeta` fork (10.10.1 + HyMeta 확장) at `/home/godong/himeta/db_bench`

**DB** (`/work/db/250gb_himeta`):
- 250 GB, **3,346 SSTs**, 2,949,840,175 unique keys
- Level 분포: **L1=3, L2=40, L3=429, L4=2,874** (L0/L5/L6 empty after compaction drain)
- Key=48B, Value=43B, Block=4KB, Bloom=10 bits/key (FPR 이론 0.82%, 실측 ~1.0%)

**Workload**: YCSB-C uniform point lookup, `duration=600s`, seed=42

**Cache budget**: **2%, 1%, 0.5%, 0.2%** (5GB → 500MB)

**db_bench 공통 flag**:
```
--benchmarks=readrandom  --num=2949840175  --duration=600  --threads=16
--key_size=48  --value_size=43  --bloom_bits=10  --block_size=4096
--use_himeta_scheme=true  --cache_type=hyper_clock_cache
--cache_numshardbits=-1  --cache_index_and_filter_blocks=true
--use_direct_reads=true  --use_direct_io_for_flush_and_compaction=true
--disable_wal=true  --statistics=true  --perf_level=3
--enable_index_compression=false  --index_shortening_mode=1
--cache_size=<DB_SIZE × pct>
--metadata_format_preference=<himeta|full|partitioned|unify>
--himeta_level_preference=<F,P>  (himeta 모드에서만)
```

각 실험 전 `echo 3 > /proc/sys/vm/drop_caches` 로 OS page cache 초기화.

---

# Part B — 실측 분석

## 슬라이드 4 · Pure scheme 실측 + scaling 붕괴 (2 min) ⭐

### 16T us/op × 4 cache (3 pure schemes)

| cache | all_Full | all_Part | all_Unify |
|-------|---------|----------|-----------|
| 2% | **74.5** | 107.7 | 104.7 |
| 1% | 397.1 | **170.2** | 143.9 |
| 0.5% | **1085.4** | 229.6 | **184.7** |
| 0.2% | **1765.2** | 290.2 | **234.5** |

→ **2% 에서 Full 우승 → 1% 부터 역전 → 0.2% 에서 Full 24배 느림**

### 1T vs 16T scaling efficiency

`scaling = (16T aggregate throughput) / (16 × 1T throughput)` — 1.0 = 완벽 확장.

| cache | scheme | 1T us | 16T us | **scaling** |
|-------|--------|-------|--------|-------------|
| 2% | Full | 71.2 | 74.5 | **95.5%** ✓ |
| 2% | Part | 111.1 | 107.7 | **103%** (16T 더 빠름) |
| 1% | Full | 188.9 | **397.1** | **47.6%** ⚠️ |
| 1% | Part | 166.4 | 170.2 | 97.8% |
| 0.5% | Full | 393.4 | **1085.4** | **36.2%** ⚠️ |
| 0.5% | Part | 220.4 | 229.6 | 96.0% |

### 핵심 인사이트

1. **Full scheme은 tight cache에서 스레드 확장 실패** (36-48% scaling)
2. **Part/Unify는 선형 확장** (96-103%)
3. 원인: 840KB Full filter 블록을 16 threads가 동시 요청 → **NVMe bandwidth 포화**
4. 극단 예: 0.2% 16T all_Full 1765us/op = 2% 16T all_Full(74us) 의 **24×**

**이 scaling 붕괴가 시뮬레이터 설계의 핵심 과제** (→ 슬라이드 9).

---

## 슬라이드 5 · Per-block latency (cold4 측정) (1.5 min) ⭐

실측 cold4 (empty cache로 시작한 read) 로그 — **각 블록 타입의 single-flight DISK_IO latency**:

| Scheme | 블록 | 크기 | DISK_IO (us) | CACHE_INSERT (us) |
|--------|------|------|-------------|-------------------|
| **Full** | Filter | ~840 KB | **289.8** | 0.36 |
| **Full** | Index | ~316 KB | 194.2 | 0.40 |
| **Part** | FilterPartIdx (top) | ~14 KB | 77.8 | 0.29 |
| **Part** | Filter partition | 4 KB | **97.1** | 0.26 |
| **Part** | Index partition | 4 KB | 85.3 | 0.26 |
| **Unify** | Top-level | ~19 KB | 85.4 | 0.29 |
| **Unify** | Partition (F+I) | 4 KB | **102.7** | 0.23 |
| (공통) | Data | 4 KB | 103.6 | 0.25 |

### 인사이트

1. **840 KB Full filter = 290us** (4KB Part partition의 ~3배)
2. **작은 블록도 80-100us** — NVMe random I/O latency는 **크기보다 I/O 횟수에 더 민감**
3. → Cache hit 잘 되면 Full 우세 (I/O 적음), miss 많으면 Part 우세 (개별 I/O 저렴)
4. **이 값들이 sim의 per-block miss cost calibration** (`IOCostConfig::disk_io_*_us`)

---

## 슬라이드 6 · Phase-level ALL_HIT/HAD_MISS 분석 (1.5 min) ⭐

`perf_level=3` 로 각 lookup의 filter/index phase를 분류:
- **ALL_HIT**: 모든 블록이 cache hit → 빠른 경로 (~1-2us)
- **HAD_MISS**: ≥ 1 블록이 miss → disk 접근 포함 (~100us)

**exp_cb_2pct vs 0p5pct (Partitioned, 16T 600s)**:

| cache | phase | ALL_HIT count | ALL_HIT us | HAD_MISS count | HAD_MISS us | **ALL_HIT 비율** |
|-------|-------|---------------|-----------|----------------|-------------|-----------------|
| 2% | filter | 13.1M | 1.43 | 1.5M | 102.4 | **89.7%** |
| 2% | index | 3.07M | 2.29 | 458k | 106.7 | **87.0%** |
| **0.5%** | filter | 4.68M | 1.27 | 2.70M | 101.7 | **63.4%** |
| **0.5%** | index | 549k | 2.26 | 1.23M | 105.7 | **30.9%** |

### 핵심 관찰

- Cache 4× 작아짐 (2% → 0.5%) → **index HAD_MISS 비율 5배 증가** (13% → 69%)
- **ALL_HIT / HAD_MISS 개별 latency 는 거의 불변** (~1.5us / ~100us)
- Latency 증가의 본질은 **miss 비율 자체**가 기하급수적으로 나빠지는 것

→ Sim 검증 시 이 phase 비율이 실측과 일치하는지가 품질 지표 (실제 MAE: filter **0.8%**, index 8%).

---

## 슬라이드 7 · Hybrid 정책 설계 + 실측 결과 (2 min) ⭐

### 5 hybrid + 3 baseline 정책

| ID | `himeta_level_preference` | L0-L2 (43 SSTs) | L3 (429) | L4 (2874) |
|----|--------------------------|-----------------|----------|-----------|
| P1 | 2,3 | Full | Part | Unify |
| **P2** | 3,3 | Full | **Full** | Unify |
| P3 | 1,3 | 일부 Full | Part | Unify |
| P4 | 2,4 | Full | Part | **Part** |
| **P5** | 2,2 (HyMeta 논문 default) | Full | **Unify** | Unify |
| baseline | — | all_Full / all_Part / all_Unify | | |

### 실측 결과 (us/op, 16T, 600s)

| Policy | 2% | 1% | 0.5% | 0.2% |
|--------|----|----|----|----|
| P1 (2,3) | 104.4 | 143.4 | 183.8 | 228.3 |
| **P2 (3,3)** | **102.4** | **135.2** | **158.3** | 227.5 |
| P3 (1,3) | 105.0 | 144.2 | 184.9 | 234.3 |
| P4 (2,4) | 107.0 | 169.2 | 227.9 | 282.3 |
| P5 (2,2) | 104.0 | 143.1 | 183.5 | **226.4** |
| all_Full | 74.5 | 397.1 | 1085.4 | 1765.2 |
| all_Part | 107.7 | 170.2 | 229.6 | 290.2 |
| all_Unify | 104.7 | 143.9 | 184.7 | 234.5 |

### 3가지 발견

**① L3 scheme crossover (0.3-0.4% cache 근처)**:
- 2% ~ 0.5%: **P2 (L3=Full)** 우승 (P5 대비 5-15% 빠름)
- **0.2%: P5 (L3=Unify)** 우승 (P2 역전) — L3 Full 메타 5GB 가 500MB cache 압박
- **한 레벨 scheme 선택이 8% 이상 성능 차이**

**② HyMeta 논문 default (P5) ≈ all_Unify**:
- 모든 cache 에서 두 정책이 1us 이내 차이
- L0-L2 는 1.3% SSTs 뿐 → Full 구간의 통계적 기여 미미
- **L3 scheme 선택이 진짜 leverage, L0-L2는 noise**

**③ P2 가 새 seed 후보**: P1(현재) 대비 모든 tight cache 구간에서 5-10% 우수

---

# Part C — 시뮬레이터 구축 여정

## 슬라이드 8 · 시뮬레이터 아키텍처 (0.75 min)

```
hymeta_evolve/
├── initial_program.cpp           # ⭐ EVOLVE-BLOCK: select_scheme() 11 LoC
└── harness/                      # 1,416 LoC 고정 infrastructure
    ├── sim_engine.{hpp,cpp}      # LSMTree + HyperClockCache + IOModel + DiskQueue
    ├── workload.{hpp,cpp}        # YCSB uniform/Zipfian
    ├── layout_loader.{hpp,cpp}   # sst_layout.json 파서
    └── sim_main.cpp              # CLI + JSON stdout
```

**OpenEvolve 가 수정하는 건 11 LoC** (`select_scheme`). 나머지는 고정 infra.

**구성 요소**:
- LSMTree + 실제 DB SST layout 임포트 (3,346 SSTs)
- HyperClockCache (RocksDB `clock_cache.cc` 정확 포팅)
- IOModel (per-block latency 기반 cost 계산)
- DiskQueue (multi-thread bandwidth saturation 모델)

---

## 슬라이드 9 · 확장 여정: Single-thread → Multi-thread (2 min) ⭐

### Step 1 — 1T sim을 cold4 per-block latency로 구축

```cpp
double access_block(cache_key, size, block_type, sst) {
  if (cache.lookup(cache_key))       return hit_cost_[bt];     // ~0.3us
  cache.insert(cache_key, size, bt); return miss_cost_[bt];    // 97~290us
}
```

- `miss_cost_[bt]` = cold4 logs 에서 그대로 (Full filter 290us, Part partition 97us 등)
- 1T 실측과 **MAE ~3%** — 훌륭한 출발점

### Step 2 — 16T로 naive 확장 → mismatch

"16 threads 면 그냥 throughput × 16" 가정 시:

| 시나리오 | 1T 실측 | 16T 예상 (1T와 동일) | 16T 실측 | 오차 |
|---------|---------|---------------------|----------|------|
| 0.5% Full | 393us | 393us | **1085us** | **+176%** ❌ |
| 1% Full | 189us | 189us | **397us** | +110% ❌ |
| 0.5% Part | 220us | 220us | 230us | +4% ✓ |

→ **Full 에서만 심한 mismatch** (Part는 정상). 원인: 840KB filter 블록을 16 threads 가 동시 요청 → NVMe bandwidth 포화. Cold4 latency 는 single-flight 기준이라 **동시 경합 반영 못함**.

### Step 3 — fio 로 NVMe bandwidth ceiling 측정

`bench_fio_nvme.sh` (random read, `direct=1`, 16 threads, iodepth=1):

| 블록 크기 | Bandwidth (GB/s) |
|---------|-----------------|
| 4 KB | 0.93 |
| 16 KB | 2.98 |
| 64 KB | 7.03 |
| 256 KB | 15.47 |
| 1 MB | 21.51 |

→ NVMe 처리량 한도. Cold4 per-block 과 **차원이 다른 양**:
- cold4 = 한 I/O의 비용 (μs)
- fio = 시스템 동시 처리 상한 (GB/s)

### Step 4 — Post-hoc saturation model

```cpp
double effective_us_per_op(base_us, op_count, N_threads):
  avg_io_size = total_miss_bytes / total_io_count
  bw = bandwidth_for_size(avg_io_size) × 0.75      // RocksDB overhead
  demand_per_op = total_miss_bytes / op_count
  aggregate_demand = N × (1e6/base_us) × demand_per_op
  if (aggregate_demand > bw):
    return N × demand_per_op / bw × 1e6             // saturated
  return base_us                                     // under capacity
```

**0.5% 16T Full 검증**:
- demand = 16 × (1/393μs) × 820KB ≈ **33 GB/s**
- bandwidth(840KB) × 0.75 ≈ **15 GB/s** (supply)
- Demand > Supply → saturated_us = 16 × 820KB / 15GB/s ≈ **~870us**
- 실측 1085us, sim 예측 870us → **2.76× slowdown 재현**

### 단계별 정확도

| 구성 | 1T 정확도 | 16T 정확도 |
|------|---------|----------|
| Cold4 per-block only | ±3% ✓ | **-70%** (mismatch) ❌ |
| + fio saturation | ±3% ✓ | **±10%** ✓ |

→ Per-block latency **와** bandwidth ceiling 이 둘 다 필요. 서로 다른 물리 현상.

---

## 슬라이드 10 · CLOCK 버그 수정 (1.5 min) ⭐

### 새로운 문제 발견 — hybrid 정책에서 과추정

Saturation model 까지 마친 sim을 hybrid 정책 (2% 16T) 에 돌려보니:

| Policy | Real | Sim | Error |
|--------|------|-----|-------|
| P1 (2,3) | 104.4 | 138.3 | **+32%** ❌ |
| P2 (3,3) | 102.4 | 128.5 | +25% |
| all_Full | 71.6 | 72.2 | -3% ✓ |
| **all_Part** | 107.7 | 147.9 | **+37%** ❌ |

**Pure Full은 정확한데 Part/hybrid만 오차 큼** → HyperClockCache 구현에 문제 의심.

### Root cause — RocksDB 소스 직접 읽기

`/home/godong/himeta/cache/clock_cache.cc` 의 알고리즘:

```cpp
// GetInitialCountdown(): insert 시 초기 CLOCK counter
HIGH   → 3
LOW    → 2   ← default (우리 실험: cache_high_pri_pool_ratio=0)
BOTTOM → 1

// FinishSlotInsert: acquire_count = release_count = initial_countdown 로 세팅
// ClockUpdate (sweep): new_count = min(count-1, kMaxCountdown-1=2)
//                      0에 도달한 entry만 evict
```

### 내 sim 의 버그

```
insert 시 usage 초기값     내 sim: 0         Real: 2  ← 3배 차이
Cold entry 생존 sweep 수   내 sim: 0~1      Real: 3
```

**Small-block 많은 Part/Unify 에서 miss rate 크게 벌어짐** (Full 은 entry 적어 영향 미미).

### 수정 후 재검증 (matched num_ops)

```cpp
insert: usage = kLowCountdown = 2
lookup: usage++  // cap 없음
sweep:  usage = min(usage - 1, kMaxCountdown - 1 = 2)
```

| 설정 | 이전 MAE | 수정 후 MAE |
|------|---------|-----------|
| 2% 16T hybrid (4 policies) | **18.6%** | **3.5%** |
| Rankings | P4 역전 ❌ | **4/4 perfect** ✓ |

→ CLOCK counter 의미만 RocksDB 와 일치시키자 sim 정확도 5× 개선. 비용 값(cold4) 은 그대로, **알고리즘 정합성이 핵심**.

---

## 슬라이드 11 · 최종 Sim 검증 결과 (1 min) ⭐

**38 real runs** (baseline 12 + hybrid 12 + 확장 14) vs **matched num_ops sim**:

| 카테고리 | N | MAE | 주요 outlier |
|---------|---|-----|--------------|
| Pure baseline | 12 | **5.7%** | 1% 16T Full +17% (saturation) |
| Hybrid P1-P4 | 12 | **3.1%** | P4 @ 0.5% -5.1% |
| 확장 (P5, 0.2%) | 14 | **4.1%** | **P2 @ 0.2% -20.7%** |
| **전체 38 scenarios** | | **~5%** | |

### Rankings 보존

- **Pure crossover 6/6 일치** (2%/1%/0.5% × 1T/16T 전부)
- Hybrid rankings: 1%/0.5% 에서 **4/4 perfect**, 2% 에선 top 2 일치 (하위 2개 sim 차이 0.02us → tie)

### 남은 한계 (투명 공개)

**P2 @ 0.2% sim 예측 180us, 실측 228us (-20.7%)**:
- L3 Full (~5GB) 을 500MB cache 에 밀어넣는 극한 상황
- Sim CLOCK 이 실제 hyper_clock_cache 의 thrash 효과를 아직 과소평가
- **2% 이상 일반 조건 에서는 문제 없음**

---

# Part D — OpenEvolve + 결론

## 슬라이드 12 · OpenEvolve 통합 (1 min)

### 진화 대상 (`initial_program.cpp`, 11 LoC)

```cpp
#include "sim_engine.hpp"
namespace hymeta {
// EVOLVE-BLOCK-START
static constexpr Scheme kLevelScheme[7] = {
    SCHEME_FULL, SCHEME_FULL, SCHEME_FULL,    // L0-L2
    SCHEME_PARTITIONED,                        // L3 ← 핵심 탐색 공간
    SCHEME_UNIFY, SCHEME_UNIFY, SCHEME_UNIFY,  // L4-L6
};
Scheme select_scheme(const SSTStats& s) {
  int lv = s.level; if (lv<0) return SCHEME_FULL; if (lv>=7) return SCHEME_UNIFY;
  return kLevelScheme[lv];
}
// EVOLVE-BLOCK-END
}
```

### Evaluator (`evaluator.py`)

- 6 시나리오 (2%/1%/0.5% × 1T/16T) × 3M ops sim = **~12초 per iteration**
- Fitness = `-avg_us_per_op` (낮을수록 좋음)
- 150 iteration × 12초 = **30분** (LLM API 응답 제외)

### 실행 준비 완료

```bash
OPENAI_API_KEY=<your-api-key> openevolve-run initial_program.cpp evaluator.py \
  --config config.yaml --iterations 150 --output evolve_runs
```

**후속 검증 사이클**: 상위 5개 후보 → real RocksDB 10분씩 cross-validate (50분).

---

## 슬라이드 13 · 교훈 & 남은 과제 (1 min)

### 메타 교훈

1. **실험 조건 정확히 매칭**: num_ops, thread, found_rate 까지. "대충 맞으면 OK"는 curve-fitting 함정.
2. **실제 소스 읽기 > 추측·가설**: RocksDB `clock_cache.cc` 400줄 읽기 30분, 그 전 시행착오 4시간 허비.
3. **Absolute accuracy vs ranking preservation**: OpenEvolve 에는 **상대 랭킹이 더 중요**. 20% outlier 있어도 evolve 가능.
4. **Baseline 먼저 탄탄하게**: pure scheme 정확도 확보 후 hybrid 로 확장. 디버그 쉬움.

### 도메인 인사이트

1. **Cache budget crossover 는 실존**: 같은 DB, 같은 workload 인데 cache 크기만으로 최적 scheme 역전.
2. **Thread scaling = cache 효율 함수**: Full + tight cache = saturation 붕괴, Part/Unify 는 선형.
3. **L3 scheme 이 진짜 leverage**: P2 vs P5 의 8% 차이가 L3 한 레벨 선택.
4. **HyMeta 논문 default ≈ all_Unify**: L0-L2 는 SSTs 의 1.3% 뿐.

### 남은 과제

- [ ] `initial_program.cpp` seed 를 P2 (3,3) 로 업데이트
- [ ] OpenEvolve 150 iter 실행 (API key 필요)
- [ ] Top-N 정책 real 재검증
- [ ] Zipfian workload 확장
- [ ] 0.2% 극소 cache sim 모델 개선 (thrash)

---

## 슬라이드 14 · 한 페이지 요약 (30s)

> **250GB RocksDB HyMeta + 38 실측 + 38 matched sim = 76 실험으로 검증한 C++ simulator 완성 (MAE ~5%, rankings 보존).**
>
> **실측 인사이트**
> - Cache crossover: Full 우세(2%) → Part/Unify 우세(1% 이하)
> - Thread scaling: Part/Unify 선형, Full tight cache 에서 36-48% 붕괴
> - L3 scheme 선택 = 진짜 leverage (P2 > HyMeta 논문 default by 8%)
>
> **Simulator 여정**
> - 1T: cold4 per-block latency 만으로 MAE 3%
> - 16T: naive 확장 mismatch → fio bandwidth 기반 saturation 모델로 MAE 10% 이내
> - Hybrid 검증 중 +30% 과추정 발견 → RocksDB clock_cache.cc 정확 포팅 → MAE 3.5%
>
> **OpenEvolve**: real 6일 → sim 30분 (300× 가속). 실행 준비 완료.

---

## 부록 A · Q&A 예상 질문

**Q1. 왜 sim을 쓰는가? Real DB로 grid search 하면?**
2,187 조합 × 10분 = 364시간. Workload·cache 조건 바뀔 때마다 반복 불가. Sim 은 1-2초에 평가.

**Q2. Sim에서 top-N 뽑고 real 로 재검증 — 왜 안 맞을 수도 있나?**
Sim 의 rankings 가 real 과 일치하므로 top-N 은 거의 항상 real 에서도 좋음. 하지만 1-2위 순서는 뒤집힐 수 있음 → 여러 후보 재검증 권장.

**Q3. Sim MAE ~5% 라는데 OpenEvolve 방해 안 되나?**
Ranking preservation 이 더 중요. 1%/0.5% hybrid 4/4 perfect. 20% outlier (P2@0.2%) 는 극소 cache 한정.

**Q4. Cold4 latency 와 fio bandwidth 를 둘 다 쓰는 이유?**
- Cold4: 한 I/O 가 RocksDB 내부 경로에서 걸리는 총 시간 (single-flight)
- fio: NVMe 동시 처리 상한 (contention ceiling)
→ Multi-thread 확장에서 두 값 모두 필수.

**Q5. CLOCK counter 3 vs 2 가 왜 3배 차이를 만드나?**
Cold entry 가 eviction 되기까지 sweep 횟수 차이 (0~1 vs 3). Small-block 많은 Part/Unify 에서 증폭.

**Q6. P2 가 P1 seed 보다 좋다면 왜 지금 업데이트 안 했나?**
0.2% 극소 cache 에선 P5 가 P2 를 이김 — seed 변경은 OpenEvolve 탐색 전에 신중히 결정. 다중 seed 실행도 고려 중.

---

## 부록 B · 전체 실측 결과 수치

### B.1 Pure baseline (exp_cache_budget_real*, 600s)

**1T**:
| cache | scheme | us/op | ops/sec | hit | miss |
|-------|--------|-------|---------|-----|------|
| 2.0 | Full | 71.168 | 14,051 | 28,412,884 | 5,456,553 |
| 2.0 | Part | 111.085 | 9,002 | 34,419,827 | 5,460,170 |
| 1.0 | Full | 188.910 | 5,293 | 9,249,324 | 3,511,782 |
| 1.0 | Part | 166.391 | 6,009 | 20,995,754 | 5,620,733 |
| 0.5 | Full | 393.357 | 2,542 | 3,334,991 | 2,794,126 |
| 0.5 | Part | 220.358 | 4,538 | 14,388,682 | 5,710,591 |

**16T**:
| cache | scheme | us/op | ops/sec | hit | miss |
|-------|--------|-------|---------|-----|------|
| 2.0 | Full | 74.533 | 214,657 | 434,199,784 | 83,224,095 |
| 2.0 | Part | 107.692 | 148,563 | 575,532,771 | 82,577,991 |
| 1.0 | Full | 397.050 | 40,286 | 70,425,071 | 26,718,204 |
| 1.0 | Part | 170.198 | 93,997 | 331,020,570 | 85,420,577 |
| 0.5 | Full | 1085.390 | 14,734 | 19,334,973 | 16,221,100 |
| 0.5 | Part | 229.620 | 69,666 | 221,871,717 | 86,793,652 |

### B.2 Hybrid P1-P4 (16T, 600s)

**2% cache**:
| Policy | `pref` | us/op | ops/sec |
|--------|--------|-------|---------|
| P1 | 2,3 | 104.382 | 153,270 |
| P2 | 3,3 | 102.426 | 156,195 |
| P3 | 1,3 | 105.004 | 152,362 |
| P4 | 2,4 | 106.957 | 149,578 |

**1% cache**:
| Policy | us/op | ops/sec |
|--------|-------|---------|
| P1 | 143.373 | 111,583 |
| P2 | 135.233 | 118,300 |
| P3 | 144.153 | 110,979 |
| P4 | 169.160 | 94,571 |

**0.5% cache**:
| Policy | us/op | ops/sec |
|--------|-------|---------|
| P1 | 183.765 | 87,057 |
| P2 | 158.314 | 101,050 |
| P3 | 184.864 | 86,539 |
| P4 | 227.885 | 70,198 |

### B.3 확장 실험 (16T, 600s)

**P5, all_Unify @ 2%/1%/0.5%**:
| cache | label | us/op |
|-------|-------|-------|
| 2.0 | P5 (2,2) | 104.014 |
| 2.0 | all_Unify | 104.650 |
| 1.0 | P5 (2,2) | 143.119 |
| 1.0 | all_Unify | 143.906 |
| 0.5 | P5 (2,2) | 183.507 |
| 0.5 | all_Unify | 184.659 |

**0.2% 전 정책**:
| label | mode | us/op | ops/sec |
|-------|------|-------|---------|
| P1 | himeta 2,3 | 228.295 | 70,073 |
| P2 | himeta 3,3 | 227.535 | 70,308 |
| P3 | himeta 1,3 | 234.325 | 68,270 |
| P4 | himeta 2,4 | 282.327 | 56,659 |
| P5 | himeta 2,2 | 226.424 | 70,651 |
| all_Full | full | **1765.225** | **9,059** |
| all_Part | partitioned | 290.209 | 55,121 |
| all_Unify | unify | 234.486 | 68,221 |

### B.4 Sim vs Real error (16T, matched num_ops)

| pct | scheme/policy | real | sim | error % |
|-----|---------------|------|-----|---------|
| 2.0 | Full | 74.533 | 69.020 | -7.4 |
| 2.0 | Part | 107.692 | 102.410 | -4.9 |
| 2.0 | P1 | 104.382 | 100.750 | -3.5 |
| 2.0 | P2 | 102.426 | 100.300 | -2.1 |
| 2.0 | P3 | 105.004 | 101.500 | -3.3 |
| 2.0 | P4 | 106.957 | 101.480 | -5.1 |
| 2.0 | P5 | 104.014 | 101.620 | -2.3 |
| 2.0 | all_Unify | 104.650 | 102.560 | -2.0 |
| 1.0 | Full | 397.050 | 465.850 | **+17.3** |
| 1.0 | Part | 170.198 | 162.690 | -4.4 |
| 1.0 | P1 | 143.373 | 138.020 | -3.7 |
| 1.0 | P2 | 135.233 | 133.990 | -0.9 |
| 1.0 | P3 | 144.153 | 138.840 | -3.7 |
| 1.0 | P4 | 169.160 | 161.960 | -4.3 |
| 1.0 | P5 | 143.119 | 144.210 | +0.8 |
| 1.0 | all_Unify | 143.906 | 145.160 | +0.9 |
| 0.5 | Full | 1085.390 | 1055.840 | -2.7 |
| 0.5 | Part | 229.620 | 221.190 | -3.7 |
| 0.5 | P1 | 183.765 | 177.310 | -3.5 |
| 0.5 | P2 | 158.314 | 157.330 | -0.6 |
| 0.5 | P3 | 184.864 | 178.100 | -3.7 |
| 0.5 | P4 | 227.885 | 220.320 | -3.3 |
| 0.5 | P5 | 183.507 | 187.290 | +2.1 |
| 0.5 | all_Unify | 184.659 | 188.500 | +2.1 |
| 0.2 | Full | 1765.225 | 1592.140 | -9.8 |
| 0.2 | Part | 290.209 | 281.160 | -3.1 |
| 0.2 | P1 | 228.295 | 219.700 | -3.8 |
| 0.2 | P2 | 227.535 | 180.460 | **-20.7** |
| 0.2 | P3 | 234.325 | 223.840 | -4.5 |
| 0.2 | P4 | 282.327 | 272.950 | -3.3 |
| 0.2 | P5 | 226.424 | 228.690 | +1.0 |
| 0.2 | all_Unify | 234.486 | 237.400 | +1.2 |

### B.5 Sim calibration 설정값 (cold4 logs 로 직접 calibration)

```python
disk_io_full_filter_us    = 289.8   # 840KB
disk_io_full_index_us     = 194.2   # 316KB
disk_io_part_top_us       = 77.8    # 13.9KB (FilterPartIdx)
disk_io_part_filter_us    = 97.1    # 3.6KB
disk_io_part_index_us     = 85.3    # 2.8KB
disk_io_unify_top_us      = 85.4    # 19.4KB
disk_io_unify_part_us     = 102.7   # 4.1KB
disk_io_data_us           = 103.6   # 4KB
cache_insert_us           = 0.40    # 공통
bloom_fpr                 = 0.0082  # 10 bits/key 이론값
```

NVMe bandwidth 는 fio 측정 (4KB 0.93 GB/s ~ 1MB 21.51 GB/s) + 0.75 efficiency scaling.

---

## 부록 C · 재현 명령어

```bash
# Simulator build
cd /home/godong/hymeta_evolve
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# Sim validation (28 configs, real vs sim — final)
python3 sim_validate_28configs.py   # ~41s; output: bench_results/exp_validation_28configs.csv

# Real RocksDB 확장 실험 (2h20m, 14 policies)
bash scripts/extended_real_16t.sh  # 순차 + sim 자동 비교

# OpenEvolve 실행
cd /home/godong/hymeta_evolve
OPENAI_API_KEY=<your-api-key> openevolve-run initial_program.cpp evaluator.py \
  --config config.yaml --iterations 150 --output evolve_runs
```
