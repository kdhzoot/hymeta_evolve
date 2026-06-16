# HyMeta Evolve

OpenEvolve 기반 per-SST metadata scheme 정책 탐색기.
RocksDB의 HyMeta (Full/Partitioned/Unify 3-way SST 메타데이터) 위에서
어떤 scheme을 어느 레벨에 쓸지 자동 탐색.

## 디렉토리 구조

```
hymeta_evolve/
├── CMakeLists.txt             # Release -O3 -march=native, C++17
├── config.yaml                # OpenEvolve 0.2.27 config
├── evaluator.py               # fitness: 6 scenarios × 3M ops, 12s/iter
├── initial_program.cpp        # EVOLVE-BLOCK: select_scheme() (11 LoC)
├── initial_program_diverse.cpp# diversity run seed: all-partitioned baseline
├── manual_smoke.py            # LLM 없이 5 정책 수동 ranking
├── sim_validate_28configs.py  # 28 configuration simulator validation
├── scripts/                   # run/summarize/plot experiment helpers
├── docs/                      # implementation and simulator notes
└── harness/                   # 고정 infrastructure (evolve 대상 아님)
    ├── sim_engine.{hpp,cpp}   # LSMTree + HyperClockCache + IOModel + DiskQueue (915 LoC)
    ├── workload.{hpp,cpp}     # Uniform + scrambled Zipfian (118 LoC)
    ├── layout_loader.{hpp,cpp}# sst_layout.json 파서 (146 LoC)
    └── sim_main.cpp           # CLI entry + JSON stdout (237 LoC)
```

**Total**: 1,416 LoC 고정 infrastructure + **11 LoC evolve target**.

## Build + Run

```bash
cd hymeta_evolve
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/sim_main \
  --layout=/home/godong/hymeta_evolve/bench_results/sst_layout.json \
  --cache-bytes=$((250*1024*1024*1024*1/100)) \
  --num-ops=3000000 --num-threads=16 --des-mode=0 --found-rate=0.63
```

## 핵심 파일: `initial_program.cpp` (EVOLVE-BLOCK)

```cpp
#include "sim_engine.hpp"
namespace hymeta {

// EVOLVE-BLOCK-START
static constexpr Scheme kLevelScheme[7] = {
    SCHEME_FULL,         // L0
    SCHEME_FULL,         // L1
    SCHEME_FULL,         // L2
    SCHEME_PARTITIONED,  // L3  ← OpenEvolve 가 이런 레벨 배정을 탐색
    SCHEME_UNIFY,        // L4
    SCHEME_UNIFY,        // L5
    SCHEME_UNIFY,        // L6
};

Scheme select_scheme(const SSTStats& s) {
  int lv = s.level;
  if (lv < 0) return SCHEME_FULL;
  if (lv >= 7) return SCHEME_UNIFY;
  return kLevelScheme[lv];
}
// EVOLVE-BLOCK-END

}
```

## HyperClockCache 정확도

`harness/sim_engine.cpp` 의 `HyperClockCache` 는 RocksDB의 `FixedHyperClockTable`
(`/home/godong/himeta/cache/clock_cache.cc`) 을 정확히 포팅.

**핵심 규약** (RocksDB `GetInitialCountdown`, `ClockUpdate` 일치):
```cpp
insert: usage = kLowCountdown = 2   // LOW priority default
lookup hit: usage++                  // cap 없음
CLOCK sweep: if (usage==0) evict; else usage = min(usage-1, kMaxCountdown-1=2)
```

이 포팅이 hybrid 정책 정확도를 MAE 18.6% → **3.5%** 로 개선.

## 검증 현황 (2026-04-22)

### 실측 대비 sim 정확도

| 카테고리 | N | MAE | Max |
|---------|---|-----|-----|
| Pure scheme baseline | 16 | 5.71% | 17.3% (1% 16T Full saturation) |
| Hybrid P1-P4 | 12 | 3.14% | 5.12% |
| 확장 (P5 + baseline + 0.2%) | 14 | 4.11% | 20.7% (P2 @ 0.2% 극소 cache) |
| **전체** | **42** | **~5%** | - |

### 실험한 정책 (14 configurations 전부 실측 완료)

| Policy | `himeta_level_preference` | L0-L2 | L3 | L4+ | 의미 |
|--------|--------------------------|-------|----|----|------|
| P1 (seed) | 2,3 | Full | Part | Unify | 초기 선택 |
| **P2** | 3,3 | Full | **Full** | Unify | **2%~0.5% cache 최적** |
| P3 | 1,3 | L0-L1 Full | Part | Unify | 덜 Full |
| P4 | 2,4 | Full | Part | Part | L4도 Part |
| **P5** | 2,2 | Full | Unify | Unify | **0.2% cache 최적** (HyMeta 논문 default) |
| all_Full | — | Full | Full | Full | 5-2% cache 절대 우승 (but 1% 이하 포화) |
| all_Part | — | Part | Part | Part | 중간 |
| all_Unify | — | Unify | Unify | Unify | ≈ P5 |

### Policy crossover (cache 크기별 최적)

| cache | 최적 정책 | 이유 |
|-------|----------|------|
| 5% / 2% | all_Full (단독 승) | 메타 전부 fit, 미스 적음 |
| 2% hybrid | P2 (3,3) | L3까지 Full |
| 1% | P2 (3,3) | 여전히 L3 Full 유리 |
| 0.5% | P2 (3,3) | 여전히 L3 Full 유리 |
| **0.2%** | **P5 (2,2)** | L3가 Full이면 thrashing, Unify가 승 |

## OpenEvolve 실행

```bash
# Build (선행 조건)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Validation (선택)
python3 manual_smoke.py            # LLM 없이 5 정책 ranking
python3 evaluator.py               # 단일 fitness evaluation
python3 sim_validate_28configs.py  # 28개 정책 simulator validation

# Evolution (LLM API 필요)
OPENAI_API_KEY=<your-api-key> openevolve-run initial_program.cpp evaluator.py \
  --config config.yaml --iterations 150 --output evolve_runs

# Top-N cross-validation (evolve 완료 후)
# → real RocksDB에서 himeta_level_preference로 재측정
```

## 주요 한계

1. **극소 cache (0.2%) + Full-heavy 정책**: sim CLOCK이 hyper_clock_cache의
   실제 cache pressure를 과소평가 → ~20% 과대 평가. 일반 cache 조건에서는 문제 없음.
2. **Uniform workload만 검증**: Zipfian에서는 재검증 필요.
3. **Policy 적용 시 stats=0**: 현재 protocol에서 `access_count` / `cache_hit_rate`
   기반 조건은 dead branch. 동적 정책 원하면 epoch-based re-apply 필요.

## 관련 문서

- 상세 기술 문서: `./docs/IMPLEMENTATION.md`
- 시뮬레이터 문서: `./docs/SIMULATOR.md`
- OpenEvolve 실험 요약: `./experiments/20260616_openevolve_experiment_summary.md`
- 발표용 요약: `./docs/PRESENTATION_SUMMARY.md`
