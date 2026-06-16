# OpenEvolve HyMeta Experiment Summary

Date: 2026-06-16 KST

이 문서는 HyMeta metadata scheme 선택 정책을 OpenEvolve로 탐색한 실험을
발표자료로 옮기기 쉽도록 정리한 요약이다. 목표, 실험 설계, 주요 결과,
정책 해석, 다음 실험 방향을 중심으로 구성했다.

## One-Slide Summary

핵심 메시지:

- OpenEvolve로 `select_scheme(const SSTStats&)` 정책을 진화시켜 all-Partitioned baseline 대비 최대 `1.2065x` geomean speedup을 얻었다.
- 기존 high-score reference 정책도 함께 보존했다. 이 정책은 `1.2852x` geomean speedup을 보였지만, 더 강한 P2-like seed에서 출발했으므로 neutral all-Partitioned 실험과는 별도 reference로 표시한다.
- 최상위 정책들은 모두 Unify-heavy 성향을 보였지만, 최적 정책은 일부 큰 SST나 filter-heavy SST에 Partitioned를 다시 도입해 pure Unify류 정책보다 좋은 균형을 만들었다.
- 이득은 cache가 작아질수록 커졌다. `2.0%` cache에서는 약 `1.045x`, `0.1%` cache에서는 약 `1.344x` geomean speedup을 보였다.
- 현재 다양성 설정은 코드 형태의 다양성은 유지하지만, 실제 정책 행동의 다양성은 충분히 강제하지 못한다.
- 다음 실험은 `unify_ratio`, `full_ratio`, `transition_rate` 같은 behavior-level feature metric을 MAP-Elites feature dimension으로 넣는 것이 가장 유망하다.

발표용 한 문장:

> LLM 기반 program evolution은 HyMeta의 per-SST metadata scheme 선택 문제에서 사람이 정한 단일 규칙보다 더 세밀한 cache/workload-aware 정책을 찾아냈고, 특히 metadata cache pressure가 높은 구간에서 성능 향상을 크게 만들었다.

## Background

HyMeta는 SST metadata를 저장하는 방식으로 세 가지 scheme을 고려한다.

| Scheme | 직관 | 장점 | 단점 |
| --- | --- | --- | --- |
| `SCHEME_FULL` | SST metadata를 가장 풍부하게 유지 | cache에 들어맞으면 lookup 비용이 낮음 | metadata footprint가 커서 작은 cache에서 thrashing 위험 |
| `SCHEME_PARTITIONED` | metadata를 partition 단위로 나눔 | 큰 SST나 filter-heavy 상황에서 cache pressure 완화 | Full보다 접근 비용이 늘 수 있음 |
| `SCHEME_UNIFY` | 더 압축적이고 통합적인 metadata 표현 | 작은 cache에서 안정적이고 footprint가 낮음 | workload에 따라 Full/Partitioned보다 덜 공격적일 수 있음 |

핵심 문제는 모든 cache size와 workload distribution에서 항상 좋은 단일 scheme이 없다는 점이다. Cache가 넉넉하면 Full이 유리할 수 있고, cache가 매우 작으면 Unify가 유리해진다. 또한 SST level, key count, access count, scan/point lookup 비율, cache hit rate, filter rejection rate에 따라 최적 선택이 바뀔 수 있다.

따라서 이번 실험은 hand-written threshold를 고정하기보다, OpenEvolve가 C++ 정책 함수를 직접 수정하면서 더 나은 scheme selection rule을 찾게 하는 방식으로 진행했다.

## Research Question

주요 질문:

- all-Partitioned baseline에서 시작했을 때 OpenEvolve가 더 높은 성능의 HyMeta policy를 찾을 수 있는가?
- 반복 run과 높은 exploration 설정이 서로 다른 전략 family를 만들어내는가?
- 최상위 정책들은 Full, Partitioned, Unify를 어떤 비율과 조건으로 사용하는가?
- 현재 feature map 설정이 실제 정책 행동의 다양성을 충분히 유도하는가?

## Evolve Target

OpenEvolve가 바꾸는 코드는 `select_scheme(const SSTStats&)` 함수다. 고정 harness와 simulator는 그대로 두고, scheme 선택 정책만 진화시킨다.

초기 정책은 all-Partitioned baseline이다.

```cpp
Scheme select_scheme(const SSTStats&) {
  return SCHEME_PARTITIONED;
}
```

정책이 참조할 수 있는 주요 `SSTStats` 필드는 다음과 같다.

| Field | 의미 | 정책에서 기대되는 활용 |
| --- | --- | --- |
| `level` | LSM level | 낮은 level SST를 Full로 승격하거나 높은 level을 Unify/Partitioned로 제한 |
| `num_keys` | SST key 수 | 큰 SST의 metadata footprint를 제어 |
| `access_count` | 누적 접근 횟수 | hot SST 감지 |
| `point_lookup_count` | point lookup 횟수 | lookup-heavy SST 감지 |
| `scan_count` | scan 횟수 | scan-heavy SST 감지 |
| `filter_rejection_rate` | filter rejection 비율 | filter-heavy workload에서 Partitioned 선택 가능 |
| `cache_hit_rate` | metadata cache hit rate | cache-resident SST에 Full 적용 여부 판단 |
| `full_metadata_size` | Full scheme metadata 크기 | Full 적용 시 cache budget 초과 위험 판단 |
| `partitioned_num_partitions` | Partitioned partition 수 | Partitioned 적용 가능성/구조 정보 |
| `unify_num_partitions` | Unify partition 수 | Unify 적용 가능성/구조 정보 |

## Evaluation Protocol

Fitness는 all-Partitioned baseline 대비 speedup의 geometric mean이다.

```text
speedup_i = baseline_us_per_op_i / candidate_us_per_op_i
fitness   = geomean(speedup_i over all scenarios)
```

해석:

- `1.0`: all-Partitioned와 동일
- `1.1`: 평균적으로 약 10% 빠름
- `1.2`: 평균적으로 약 20% 빠름
- `0.9`: 평균적으로 약 10% 느림

평가 환경:

| Item | Value |
| --- | --- |
| Simulator | C++ HyMeta simulator in `harness/` |
| DB size | `250 GiB` |
| Operations per scenario | `3,000,000` |
| Threads | `16` |
| Found rate | `0.63` |
| Random seed | `42` |
| Dynamic reapply interval | `150,000` ops |
| Reapply count | `20` per scenario |
| Cache sizes | `2.0%`, `1.0%`, `0.5%`, `0.25%`, `0.1%` of DB size |
| Workloads | `uniform`, `zipfian`, `mixgraph` |
| Total scenarios | `15` |

Scenario design:

- `2.0%` cache: metadata가 비교적 잘 들어맞는 쉬운 구간
- `1.0%` cache: saturation이 시작되는 구간
- `0.5%` cache: metadata cache pressure가 강한 구간
- `0.25%` cache: Full-heavy policy가 흔들리기 쉬운 극단 구간
- `0.1%` cache: 대부분의 SST에 footprint 절감이 중요한 병목 구간

Workload design:

- `uniform`: 뚜렷한 hotspot 없이 넓게 접근하는 workload
- `zipfian`: hotspot이 강한 skewed workload
- `mixgraph`: RocksDB 계열 접근 패턴을 반영한 mixed workload

## OpenEvolve Configuration

Primary config:

- `config_diverse_glm.yaml`

LLM 설정:

| Setting | Value |
| --- | --- |
| Model | `glm-5.1` |
| API style | OpenAI-compatible local endpoint |
| Temperature | `1.0` |
| Top-p | `0.95` |
| Max tokens | `4096` |

Search/database 설정:

| Setting | Value | 의도 |
| --- | ---: | --- |
| `population_size` | `180` | 충분한 후보 pool 유지 |
| `archive_size` | `90` | 좋은 후보와 다양한 후보 저장 |
| `num_islands` | `6` | 병렬적인 search path 유도 |
| `migration_interval` | `40` | island 간 교류 주기 |
| `migration_rate` | `0.05` | migration 강도 |
| `elite_selection_ratio` | `0.05` | 상위 후보 보존 |
| `exploration_ratio` | `0.55` | 새로운 정책 탐색 강화 |
| `exploitation_ratio` | `0.30` | 좋은 후보 주변 개선 |

현재 feature dimensions:

| Dimension | 의미 | 한계 |
| --- | --- | --- |
| `complexity` | 대략적인 코드 길이/복잡도 | 실제 scheme 사용 비율과 직접 연결되지 않음 |
| `diversity` | 코드 형태의 차이 | 행동이 비슷한 코드도 다양하다고 볼 수 있음 |
| `score` | fitness 기반 점수 | 성능 축은 되지만 정책 구조 축은 아님 |

발표 포인트:

- 이번 설정은 "좋은 성능을 가진 서로 다른 코드"를 찾는 데는 도움이 됐다.
- 하지만 "Full-heavy / Partitioned-heavy / Unify-heavy / dynamic policy"처럼 행동이 다른 정책을 균등하게 만들지는 못했다.
- 그래서 다음 실험에서는 behavior-level feature metric이 필요하다.

## Included Policy Artifacts

발표자료에는 네 개의 policy artifact를 포함한다. 세 개는 all-Partitioned baseline에서 출발한 200-iteration 실험이고, 하나는 기존 high-score reference 정책이다.

| Policy | Scope | Iterations | Best Fitness | Best Iteration | Program ID | 요약 |
| --- | --- | ---: | ---: | ---: | --- | --- |
| `legacy12852` | high-score reference | checkpoint 50 | `1.285209293` | 44 | `0d832c19-0aee-4128-b13c-a4eccf3f044a` | P2-like seed 계열, 높은 Full 사용과 Unify fallback |
| `partbase` | neutral baseline run | 200 | `1.182825086` | 5 | `92f8a430-0c86-46d6-84c2-27c2eae2e68d` | 거의 pure Unify에 가까운 static policy |
| `seed27182` | neutral baseline run | 200 | `1.206468338` | 169 | `08b10079-04e1-48bc-95c4-24f68d79d1f1` | neutral baseline run 중 최고 성능, Unify 중심 + 선택적 Partitioned/Full |
| `seed16180` | neutral baseline run | 200 | `1.149079907` | 165 | `c8a12d08-db3b-4e22-a7d1-40e77ce586e0` | dynamic하지만 고 cache zipfian에서 약점 |

Best program files:

- `experiments/20260615_162848_diverse_glm_iter200/output/checkpoints/checkpoint_50/best_program.cpp`
- `experiments/20260615_171311_diverse_glm_partbase_iter200/output/best/best_program.cpp`
- `experiments/20260615_182729_diverse_glm_s27182_iter200/output/best/best_program.cpp`
- `experiments/20260615_202341_diverse_glm_s16180_iter200/output/best/best_program.cpp`

## Overall Result

neutral all-Partitioned 시작 조건에서 가장 좋은 run은 `seed27182`였고, all-Partitioned 대비 `1.206468x` geomean speedup을 달성했다.

| Rank | Policy | Geomean Speedup | Scope | Relative Note |
| ---: | --- | ---: | --- | --- |
| 1 | `legacy12852` | `1.285209` | reference | 기존 high-score 정책, 별도 reference로 표시 |
| 2 | `seed27182` | `1.206468` | neutral baseline | all-Partitioned 시작 조건의 최고 성능 |
| 3 | `partbase` | `1.182825` | neutral baseline | 단순한 Unify-heavy 정책도 강함 |
| 4 | `seed16180` | `1.149080` | neutral baseline | 낮은 cache에서는 좋지만 특정 구간 regression |

발표 해석:

- `legacy12852`는 가장 높은 숫자를 보여주는 reference 정책이다. 발표 그래프에는 넣되, neutral baseline search 결과와 구분해서 표시한다.
- `seed27182`는 단순히 모든 SST를 Unify로 보내지 않고, 일부 상황에서 Partitioned와 Full을 제한적으로 사용했다.
- `partbase`는 거의 pure Unify에 가까운 단순 정책인데도 강한 baseline 역할을 했다.
- `seed16180`은 dynamic policy의 가능성을 보여주지만, workload별 안정성이 부족했다.

## Workload-Level Results

Workload별 geomean speedup:

| Run | Uniform | Zipfian | Mixgraph | Interpretation |
| --- | ---: | ---: | ---: | --- |
| `legacy12852` | `1.338894` | `1.330222` | `1.191932` | 가장 높은 reference 성능, Full을 더 적극적으로 사용 |
| `partbase` | `1.203558` | `1.230779` | `1.117158` | 모든 workload에서 안정적, mixgraph 이득은 상대적으로 작음 |
| `seed27182` | `1.233747` | `1.255852` | `1.133400` | neutral baseline run 중 세 workload 모두 최고 |
| `seed16180` | `1.184217` | `1.162837` | `1.101795` | zipfian 평균이 낮아 전체 fitness 하락 |

발표 포인트:

- `legacy12852`는 세 workload 모두에서 가장 큰 speedup을 보이지만, 시작 조건이 다르므로 high-score reference로 해석한다.
- neutral baseline run만 비교하면 `seed27182`는 uniform, zipfian, mixgraph 모두에서 1등이다.
- zipfian에서 가장 큰 개선이 나왔다. Hotspot이 있는 상황에서 metadata footprint와 hot SST 처리를 함께 조정한 효과로 볼 수 있다.
- mixgraph는 세 run 모두 개선 폭이 작다. Mixed workload에서는 지나치게 공격적인 scheme 변경보다 안정적인 footprint 절감이 중요해 보인다.

## Cache-Level Results

Cache size별 geomean speedup:

| Cache Size | `legacy12852` | `partbase` | `seed27182` | `seed16180` | Best neutral baseline |
| ---: | ---: | ---: | ---: | ---: | --- |
| `2.0%` | `1.113901` | `1.054776` | `1.044971` | `0.976506` | `partbase` |
| `1.0%` | `1.222499` | `1.135417` | `1.153732` | `1.110268` | `seed27182` |
| `0.5%` | `1.345700` | `1.189908` | `1.230047` | `1.194003` | `seed27182` |
| `0.25%` | `1.468069` | `1.238744` | `1.282644` | `1.238395` | `seed27182` |
| `0.1%` | `1.303406` | `1.311572` | `1.343826` | `1.249637` | `seed27182` |

발표 포인트:

- 성능 향상은 cache가 작아질수록 커지는 경향이 뚜렷하다.
- `seed27182`는 `1.0%` 이하 cache pressure 구간에서 일관되게 가장 좋다.
- `legacy12852`는 `0.25%`까지 매우 강하지만, `0.1%` extreme cache에서는 `seed27182`보다 낮다. 더 aggressive한 Full 사용이 극단적인 cache pressure에서는 부담이 될 수 있음을 보여준다.
- `2.0%`에서는 단순 Unify-heavy인 `partbase`가 근소하게 더 좋다. 이는 cache가 넉넉할 때 dynamic threshold가 항상 이득이 되는 것은 아니라는 점을 보여준다.
- `seed16180`은 `2.0%`에서 `0.9765x`로 baseline보다 느리다. 이 한 구간의 약점이 전체 점수를 낮췄다.

## Scheme Usage

최종 scenario 전체에서의 scheme 사용 비율:

| Run | Full Ratio | Partitioned Ratio | Unify Ratio | Scheme Transitions | Interpretation |
| --- | ---: | ---: | ---: | ---: | --- |
| `legacy12852` | `11.259%` | `0.012%` | `88.729%` | `1597` | reference 정책, Full을 가장 적극적으로 사용 |
| `partbase` | `1.285%` | `0.000%` | `98.715%` | `0` | 사실상 static Unify-heavy |
| `seed27182` | `2.018%` | `14.599%` | `83.383%` | `19748` | Unify 중심이지만 Partitioned를 의미 있게 사용 |
| `seed16180` | `0.000%` | `12.333%` | `87.667%` | `15526` | Full 없이 Unify/Partitioned 사이에서 동적으로 이동 |

해석:

- 가장 좋은 정책도 `83%+` Unify를 사용한다. 작은 cache 구간이 평가에 포함되어 있어 footprint 절감이 중요했기 때문이다.
- `legacy12852`는 Full 비율이 `11.3%`로 가장 높다. 이 정책은 cache hit가 충분한 shallow/hot SST에 Full을 더 공격적으로 허용한다.
- `seed27182`의 차별점은 Unify를 기본값으로 두되, `14.6%` 정도의 SST에 Partitioned를 배치한다는 것이다.
- Full은 전체의 `2.0%` 수준으로 매우 제한적으로 쓰인다. Full은 cache에 들어맞는 hot/small metadata SST에만 쓰는 것이 안전하다는 결론에 가깝다.
- Transition이 많다는 것은 reapply epoch마다 live stats를 반영해 scheme이 바뀌는 dynamic policy라는 뜻이다.

## Best Neutral-Baseline Policy: Seed 27182

`seed27182`의 정책 형태:

- 낮은 level은 Full 후보로 본다.
- Hot하고 metadata가 작은 SST는 Full을 허용한다.
- Scan-heavy 또는 filter rejection이 높은 큰 SST는 Partitioned로 보낸다.
- 중간 크기 또는 warm SST는 Unify로 보낸다.
- 나머지는 Partitioned fallback을 둔다.

핵심 규칙:

```cpp
if (s.level <= 2) return SCHEME_FULL;
double hot = double(s.access_count) / (s.num_keys + 1.0);
bool mf = s.full_metadata_size < 400000;
if (s.cache_hit_rate > 0.55 && hot > 0.8 && mf) return SCHEME_FULL;
if (s.cache_hit_rate > 0.85 && hot > 0.05 && mf) return SCHEME_FULL;
if (s.access_count > 150000 && mf && s.num_keys < 500000) return SCHEME_FULL;
if (s.filter_rejection_rate < 0.3 && s.point_lookup_count > s.scan_count && mf && s.num_keys < 500000) return SCHEME_FULL;
if (s.scan_count > s.point_lookup_count * 2 && s.num_keys > 200000) return SCHEME_PARTITIONED;
if (s.filter_rejection_rate > 0.5) return SCHEME_PARTITIONED;
if (s.unify_num_partitions > 0 && s.num_keys < 800000) return SCHEME_UNIFY;
if (s.unify_num_partitions > 0 && hot > 0.3) return SCHEME_UNIFY;
return SCHEME_PARTITIONED;
```

발표용 해석:

- 이 정책은 "Unify를 기본으로 하되, 모든 SST를 무조건 Unify로 밀어 넣지는 않는다."
- 큰 scan-heavy SST와 filter-heavy SST는 Partitioned로 보내 cache footprint와 access pattern 사이의 균형을 맞춘다.
- Full은 hot하고 metadata footprint가 작은 SST에만 제한적으로 사용한다.
- 즉, OpenEvolve가 찾은 정책은 단일 scheme 선택이 아니라 cache pressure와 workload signal을 결합한 per-SST routing rule이다.

## Reference High-Score Policy: Legacy 1.2852

`legacy12852`는 기존 high-score reference로 포함한 네 번째 policy artifact다.

정책 형태:

- cold-start에서 `level <= 3`은 Full, 그보다 깊은 level은 Unify로 시작한다.
- cache hit rate가 낮거나 깊은 level이면 Unify를 선택해 metadata footprint를 줄인다.
- cache hit가 충분하고 shallow/hot한 SST에는 Full을 더 적극적으로 허용한다.
- scan-heavy SST에는 제한적으로 Partitioned를 사용하지만, 실제 최종 비율은 거의 0에 가깝다.

발표용 해석:

- `legacy12852`는 `1.2852x`로 가장 높은 reference score를 보인다.
- Full ratio가 `11.259%`로 다른 세 policy보다 높다.
- `0.25%` cache까지 매우 강하지만, `0.1%` cache에서는 `seed27182`보다 낮아진다.
- 따라서 발표에서는 "강한 prior seed를 사용한 reference upper-bound"로 제시하고, neutral all-Partitioned 시작 실험의 결론과는 구분한다.

## Case Study: Seed 27182 Evolution Path

`seed27182`는 neutral all-Partitioned baseline에서 시작한 run 중 가장 좋은 결과를 낸 케이스다. 이 run의 checkpoint에는 program JSON이 남아 있어서 parent chain, prompt, LLM response, metric 변화를 따라가며 진화 방향을 복원할 수 있다.

분석 대상:

- Run: `20260615_182729_diverse_glm_s27182_iter200`
- Best artifact: `experiments/20260615_182729_diverse_glm_s27182_iter200/output/best/best_program.cpp`
- Best program id: `08b10079-04e1-48bc-95c4-24f68d79d1f1`
- Checkpoint source: `output/checkpoints/checkpoint_200/programs/*.json`

### Lineage Summary

Best program의 direct parent chain은 5단계로 복원된다.

| Step | Program ID | Iteration | Fitness | Full | Partitioned | Unify | Transitions | Direction |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 0 | `5623f2fc` | 0 | `1.000000` | `0.000%` | `100.000%` | `0.000%` | 0 | all-Partitioned seed |
| 1 | `26aee537` | 7 | `1.017350` | `1.285%` | `98.715%` | `0.000%` | 0 | shallow levels become Full |
| 2 | `6832efd9` | 49 | `1.030633` | `1.943%` | `81.919%` | `16.139%` | 12585 | introduce Unify for medium SSTs |
| 3 | `22113e48` | 109 | `1.206391` | `1.943%` | `14.674%` | `83.383%` | 19710 | major shift to Unify-heavy policy |
| 4 | `08b10079` | 169 | `1.206468` | `2.018%` | `14.599%` | `83.383%` | 19748 | small Full-rule refinement |

The main jump happened at iteration 109. The policy moved from `81.9%` Partitioned to `83.4%` Unify while keeping Full rare. This suggests that, under the 15-scenario evaluator, reducing metadata footprint through Unify mattered much more than broadly expanding Full.

### Stage-by-Stage Direction

Iteration 0 to 7:

- Started from pure all-Partitioned.
- LLM introduced a cache-budget-aware policy.
- Low levels were promoted to Full.
- Runtime-signal rules for hotness, cache hit rate, scan count, filter rejection, and Unify eligibility appeared.
- Actual behavior was still almost all Partitioned: `98.7%` Partitioned, `1.3%` Full, `0%` Unify.
- Fitness rose slightly from `1.0000` to `1.0174`.

Interpretation:

- This was an exploratory step away from the neutral baseline.
- The first useful signal was not a large behavioral shift, but discovering that a small amount of Full on shallow SSTs was safe.

Iteration 7 to 49:

- Metadata threshold for Full became more permissive: `256000` to `400000`.
- Full selection thresholds were relaxed.
- Access-count and point-lookup rules were added.
- Unify was introduced for medium-size SSTs.
- Partitioned dropped from `98.7%` to `81.9%`; Unify rose to `16.1%`.
- Fitness rose from `1.0174` to `1.0306`.

Interpretation:

- This was threshold exploitation around known good candidates.
- The LLM moved from static/shallow Full toward a mixed policy that allowed Unify to replace some Partitioned choices.

Iteration 49 to 109:

- Scan-heavy Partitioned threshold became stricter: `scan > point_lookup * 1.5` to `scan > point_lookup * 2`.
- Filter-rejection Partitioned threshold became stricter: `> 0.45` to `> 0.5`.
- Unify coverage expanded: `num_keys < 600000` to `num_keys < 800000`.
- A new hotness-based Unify fallback was added: `hot > 0.3`.
- Partitioned collapsed from `81.9%` to `14.7%`; Unify rose to `83.4%`.
- Fitness jumped from `1.0306` to `1.2064`.

Interpretation:

- This was the decisive step.
- The improvement did not come from more Full. Full stayed around `1.9%`.
- The key move was making Partitioned the exception and Unify the default for a much larger region.

Iteration 109 to 169:

- A new Full rule was added for very high cache residency:

```cpp
if (s.cache_hit_rate > 0.85 && hot > 0.05 && mf) return SCHEME_FULL;
```

- Access-count threshold for Full was lowered:

```cpp
if (s.access_count > 150000 && mf && s.num_keys < 500000) return SCHEME_FULL;
```

- Full ratio increased slightly from `1.943%` to `2.018%`.
- Fitness improved only slightly: `1.206391` to `1.206468`.

Interpretation:

- This was local hill-climbing, not a new strategy.
- The LLM targeted weak high-cache scenarios by allowing a few more cache-resident SSTs to use Full.

### Prompt Context Given to the LLM

Each non-initial lineage step has a stored `prompts.diff_user` object with:

- `system`: short instruction telling the model to improve fitness while preserving diversity.
- `user`: the main prompt with current program, metrics, prior attempts, top programs, diverse programs, inspiration programs, and exact search/replace instructions.
- `responses`: previous model response text for the attempted edit.

Approximate prompt sizes:

| Iteration | System chars | User chars | Response chars | Notable prompt content |
| ---: | ---: | ---: | ---: | --- |
| 7 | 288 | 29369 | 4306 | baseline program, previous attempts, top programs around `1.0018` |
| 49 | 288 | 42035 | 4724 | current `1.0174`, top programs around `1.0316`, diverse programs |
| 109 | 288 | 41202 | 2328 | current `1.0306`, top programs already around `1.2064` |
| 169 | 288 | 41083 | 2401 | current `1.2064`, many top/diverse programs also around `1.2064` |

The prompt was not only "current code + score." It gave the model a compact local search context:

- current program id, generation, island, and metrics
- recent failed or weaker attempts
- top-performing programs and their code
- diverse programs from the archive
- inspiration programs selected as high performers
- exact current code block to replace
- required search/replace patch format

### What the LLM Appears to Have Used

At iteration 49, the LLM response explicitly compared the current program to better top programs. It identified that the current metadata threshold was too low, Full rules were too restrictive, and Unify selection was too conservative. The resulting edit relaxed thresholds and introduced broader Unify usage.

At iteration 109, the prompt already contained top programs scoring around `1.2064`. The LLM response focused on three differences:

- stricter Partitioned conditions
- broader Unify fallback
- adding a hotness-based Unify rule

This explains the large jump: the model was not inventing the final strategy from scratch at that step. It was recombining or copying a successful rule pattern that the prompt exposed through top-performing programs.

At iteration 169, the prompt showed that the current policy was already near the top. The LLM response targeted high-cache scenarios where speedups were relatively smaller, then added a narrow Full rule for cache-resident SSTs. This produced only a small improvement, which is consistent with late-stage exploitation.

### Takeaway for Presentation

The useful story is:

- Early evolution explored non-Partitioned choices.
- Mid evolution discovered that Unify should cover most SSTs.
- The decisive improvement came from making Partitioned rare rather than making Full common.
- Late evolution fine-tuned Full for a small number of hot/cache-resident SSTs.
- The prompt context matters: OpenEvolve gave the LLM not just metrics, but also top and diverse code examples, so the final policy emerged through guided recombination of previous candidates.

Suggested slide framing:

```text
all-Partitioned
  -> small Full on shallow SSTs
  -> introduce Unify for medium SSTs
  -> make Unify the dominant default
  -> fine-tune Full for hot/cache-resident SSTs
```

This makes the evolution direction easy to explain without showing every C++ threshold.

## Policy Family Comparison

### 1. Legacy 1.2852 Reference

정책 형태:

- P2-like cold-start
- shallow/hot/cache-resident SST에 Full을 적극 적용
- deep/cold SST는 Unify로 보내 footprint를 낮춤

강점:

- 전체 reference score `1.2852`
- workload별로 모두 가장 높은 speedup
- `0.25%` cache에서 `1.4681x` geomean speedup

주의점:

- all-Partitioned neutral baseline에서 출발한 run이 아니므로 별도 reference로 표시해야 한다.
- `0.1%` cache에서는 `seed27182`가 더 강하다.

### 2. Partbase Best

정책 형태:

- `level <= 2`: Full
- `level <= 4`: Unify
- otherwise: Partitioned

실제 관찰:

- 평가 scenario에서는 거의 모든 SST가 Unify로 귀결되었다.
- Transition이 없어 해석과 운영이 쉽다.
- `1.1828x`로 강한 단순 baseline이다.

발표 포인트:

- 단순 정책도 작은 cache regime에서는 상당히 강하다.
- 하지만 `seed27182`처럼 selective Partitioned를 넣으면 추가 개선이 가능하다.

### 3. Seed 27182 Best

정책 형태:

- Unify-heavy dynamic policy
- 큰 scan-heavy/filter-heavy SST에는 Partitioned
- hot하고 metadata가 작은 일부 SST에는 Full

강점:

- neutral baseline run 중 최고 fitness `1.2065`
- workload별로 모두 최고
- `1.0%` 이하 cache에서 가장 안정적으로 강함

### 4. Seed 16180 Best

정책 형태:

- Full을 거의 사용하지 않음
- Unify와 Partitioned 사이에서 dynamic하게 이동
- 낮은 cache 구간에서는 괜찮지만 high-cache zipfian에서 약점

약점:

- `zipfian`, `2.0%` cache에서 speedup `0.92149`
- 이 regression 때문에 전체 geomean이 `1.1491`로 낮아짐

발표 포인트:

- Dynamic policy가 항상 좋은 것은 아니다.
- 특정 workload/cache 구간에서 나쁜 선택을 하면 geomean 전체가 크게 영향을 받는다.
- 다음 실험에서는 평균 성능뿐 아니라 worst-case guardrail도 볼 필요가 있다.

## Main Findings

1. OpenEvolve는 all-Partitioned baseline에서 출발해 의미 있는 개선을 찾았다.

`seed27182`는 all-Partitioned 대비 `1.2065x` geomean speedup을 달성했다. 이는 metadata scheme 선택이 고정 정책보다 per-SST adaptive rule에서 더 좋아질 수 있음을 보여준다.

기존 high-score reference인 `legacy12852`는 `1.2852x`를 보였다. 다만 이 결과는 더 강한 P2-like seed 계열이므로, 발표에서는 neutral baseline search의 직접 비교 대상이 아니라 reference upper-bound로 다룬다.

2. 최상위 정책은 Unify-heavy로 수렴했다.

네 policy 모두 Unify 비율이 `83%` 이상이다. 평가가 `0.1%`부터 `2.0%`까지 cache pressure가 큰 구간을 포함하므로, metadata footprint를 줄이는 전략이 전반적으로 유리했다.

3. 최고 정책은 pure Unify가 아니라 selective Partitioned/Full을 섞었다.

`seed27182`는 `14.6%` Partitioned와 `2.0%` Full을 사용했다. 이 소량의 non-Unify 선택이 static Unify-heavy policy보다 높은 성능을 만들었다. 반면 `legacy12852`는 Full을 `11.3%`까지 더 적극적으로 사용해 높은 reference score를 만들었다.

4. 성능 향상은 cache가 작을수록 커졌다.

`seed27182`의 cache별 geomean은 `2.0%`에서 `1.045x`, `0.1%`에서 `1.344x`다. 이는 HyMeta scheme selection의 핵심 가치가 metadata cache pressure 완화에 있음을 보여준다.

5. 현재 diversity 설정은 행동 다양성까지 보장하지 않는다.

`complexity/diversity/score` feature dimension은 코드가 다르게 생긴 후보를 유지하는 데는 도움이 된다. 그러나 실제 scheme ratio나 transition pattern이 다른 정책을 적극적으로 보존하지는 않는다.

## Presentation Figure Ideas

슬라이드에 넣기 좋은 그림:

1. Problem diagram
   - SST마다 Full, Partitioned, Unify 중 하나를 선택하는 구조
   - Input signal: level, access count, scan/point lookup, cache hit, metadata size

2. Evaluation matrix
   - 5 cache sizes x 3 workloads = 15 scenarios
   - 각 cell에서 speedup을 계산하고 geomean으로 합산

3. Overall result bar chart
   - `legacy12852`, `partbase`, `seed27182`, `seed16180`의 geomean speedup
   - `legacy12852`는 reference 색상 또는 hatch로 구분

4. Cache sensitivity line chart
   - x축: cache size
   - y축: geomean speedup
   - `seed27182`가 작은 cache에서 더 강해지는 형태 강조

5. Scheme usage stacked bar
   - 네 policy의 Full/Partitioned/Unify ratio 비교
   - `seed27182`가 Unify-heavy지만 Partitioned를 의미 있게 섞는 점 강조
   - `legacy12852`가 Full을 가장 많이 쓰는 reference라는 점 강조

6. Policy rule summary
   - Hot + small metadata -> Full
   - Scan-heavy/filter-heavy large SST -> Partitioned
   - Small/warm/low-footprint default -> Unify

## Recommended Next Experiment

다음 실험의 목표는 "성능 좋은 정책"뿐 아니라 "행동이 다른 정책 family"를 더 많이 확보하는 것이다.

### Add Behavior-Level Feature Metrics

Evaluator가 다음 metric을 반환하도록 확장한다.

- `unify_ratio`
- `full_ratio`
- `partitioned_ratio`
- `transition_rate`
- `worst_scenario_speedup`
- `low_cache_geomean`
- `high_cache_geomean`

MAP-Elites feature dimension으로는 먼저 세 가지를 추천한다.

```yaml
database:
  feature_dimensions:
    - unify_ratio
    - full_ratio
    - transition_rate
  feature_bins:
    unify_ratio: 8
    full_ratio: 8
    transition_rate: 6
```

기대 효과:

- pure Unify류 정책만 상위권에 남는 현상을 줄일 수 있다.
- Full-heavy, Partitioned-heavy, dynamic transition-heavy 정책이 별도 cell에 보존된다.
- 나중에 발표할 때도 "성능 1등"과 "전략 다양성"을 함께 보여줄 수 있다.

### Keep Fitness Pure

Fitness는 계속 geomean speedup으로 둔다.

```yaml
fitness: geomean speedup vs all-Partitioned
combined_score: same as fitness
```

Diversity bonus를 fitness에 직접 더하지 않는 편이 좋다.

- 결과 해석이 깔끔하다.
- `1.20x`라는 숫자가 실제 speedup으로 남는다.
- 다양성은 MAP-Elites cell selection으로 관리한다.

### Improve Island Mixing

현재 run은 `num_islands: 6`, `migration_interval: 40`이었다. 200-iteration 실험에서는 island 간 교류가 충분히 강하지 않을 수 있다.

추천:

```yaml
database:
  migration_interval: 15
  migration_rate: 0.10
```

기대 효과:

- 좋은 rule fragment가 다른 island로 더 빨리 퍼진다.
- 서로 다른 policy family가 더 자주 recombination될 수 있다.
- 200-iteration budget에서도 island model의 효과를 더 보기 쉽다.

### Add Guardrail Metrics

`seed16180`처럼 평균은 괜찮지만 특정 scenario에서 baseline보다 느린 정책이 나올 수 있다. 다음 실험에서는 아래 metric을 같이 기록하는 것이 좋다.

- `min_speedup`
- `num_regressions`
- `worst_cache_pct`
- `worst_distribution`

이 metric은 fitness에 바로 넣기보다 artifact/auxiliary metric으로 기록하고, 후보를 분석할 때 사용한다.

## Suggested Slide Outline

1. Motivation
   - HyMeta metadata scheme 선택은 cache size와 workload에 따라 달라진다.

2. Problem Formulation
   - Per-SST `select_scheme(SSTStats)`를 진화 대상으로 정의한다.

3. Three Metadata Schemes
   - Full, Partitioned, Unify의 trade-off 표.

4. OpenEvolve Setup
   - GLM-5.1, 6 islands, high exploration, 200 iterations.

5. Evaluation Protocol
   - 15 scenarios, geomean speedup, all-Partitioned baseline.

6. Overall Results
   - neutral baseline 최고 `1.2065x`, reference `1.2852x`, 네 policy 비교.

7. Cache Sensitivity
   - 작은 cache에서 이득이 커지는 line chart.

8. Best Policy Anatomy
   - `seed27182` rule을 사람이 읽을 수 있는 decision tree로 요약.

9. Diversity Analysis
   - 현재 feature가 code-level이라 behavior diversity가 부족했다는 해석.

10. Next Steps
   - behavior metrics, migration 강화, worst-case guardrail.

## Useful Commands

Summarize experiments:

```bash
python3 scripts/compare_experiments.py experiments
```

Inspect the best program for the winning run:

```bash
sed -n '/EVOLVE-BLOCK-START/,/EVOLVE-BLOCK-END/p' \
  experiments/20260615_182729_diverse_glm_s27182_iter200/output/best/best_program.cpp
```
