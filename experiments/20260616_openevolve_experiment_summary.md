# OpenEvolve HyMeta Experiment Summary

Date: 2026-06-16 KST

This note summarizes the OpenEvolve experiments run on the HyMeta scheme-selection policy. Sensitive connection material is intentionally omitted.

## Goal

The goal was to evolve `select_scheme(const SSTStats&)` from a neutral all-Partitioned baseline and discover higher-performing or more diverse policies for choosing among:

- `SCHEME_FULL`
- `SCHEME_PARTITIONED`
- `SCHEME_UNIFY`

The evaluator score is a geometric mean speedup versus the all-Partitioned baseline. A score of `1.0` means equal to all-Partitioned.

## Runtime Setup

Model serving used an SSH-forwarded OpenAI-compatible vLLM endpoint:

- API base for OpenEvolve/OpenAI SDK: `http://localhost:50000/v1`
- Model: `glm-5.1`
- API key value used by SDK: a local placeholder token for the forwarded endpoint

Important note: the OpenAI SDK base URL must be `/v1`, not `/v1/models`. Health checks can query `/v1/models`.

The GLM endpoint required disabling thinking output in the chat template:

```yaml
llm:
  extra_body:
    chat_template_kwargs:
      enable_thinking: false
```

OpenEvolve installed package was patched locally to support `llm.extra_body`:

- `/home/godong/.local/lib/python3.12/site-packages/openevolve/config.py`
- `/home/godong/.local/lib/python3.12/site-packages/openevolve/llm/openai.py`

## Baseline Correction

Early smoke/debug runs accidentally started from a non-neutral hybrid/P2-like policy. Those runs produced `1.2843` at iteration 0/1 and are invalid for the all-Partitioned baseline comparison.

Invalid or excluded runs:

- `20260615_140246_debug_iter1`
- `20260615_154713_glm_smoke_iter1`
- `20260615_162848_diverse_glm_iter200`

The corrected initial program is all-Partitioned:

```cpp
Scheme select_scheme(const SSTStats&) {
  return SCHEME_PARTITIONED;
}
```

The corrected source files were set to all-Partitioned:

- `/home/godong/hymeta_evolve/initial_program.cpp`
- `/home/godong/hymeta_evolve/initial_program_diverse.cpp`

## Main Config

Primary config:

- `/home/godong/hymeta_evolve/config_diverse_glm.yaml`

Important settings:

```yaml
llm:
  primary_model: "glm-5.1"
  temperature: 1.0
  top_p: 0.95
  max_tokens: 4096

prompt:
  num_top_programs: 2
  num_diverse_programs: 6
  include_artifacts: true
  use_template_stochasticity: true

database:
  population_size: 180
  archive_size: 90
  num_islands: 6
  migration_interval: 40
  migration_rate: 0.05
  elite_selection_ratio: 0.05
  exploration_ratio: 0.55
  exploitation_ratio: 0.30
  feature_dimensions:
    - complexity
    - diversity
    - score
```

Current feature dimensions are OpenEvolve built-ins:

- `complexity`: roughly code length
- `diversity`: code-level diversity
- `score`: fitness-derived score

They do not directly encode policy behavior such as Unify ratio or transition rate.

## Correct Runs

These runs started from the all-Partitioned baseline and are valid for comparison.

| Run | Status | Iterations | Best Fitness | Best Iteration | Best Program |
| --- | --- | ---: | ---: | ---: | --- |
| `20260615_171311_diverse_glm_partbase_iter200` | completed | 200 | `1.182825086` | 5 | `92f8a430-0c86-46d6-84c2-27c2eae2e68d` |
| `20260615_182729_diverse_glm_s27182_iter200` | completed | 200 | `1.206468338` | 169 | `08b10079-04e1-48bc-95c4-24f68d79d1f1` |
| `20260615_202341_diverse_glm_s16180_iter200` | completed | 200 | `1.149079907` | 165 | `c8a12d08-db3b-4e22-a7d1-40e77ce586e0` |

Best program files:

- `experiments/20260615_171311_diverse_glm_partbase_iter200/output/best/best_program.cpp`
- `experiments/20260615_182729_diverse_glm_s27182_iter200/output/best/best_program.cpp`
- `experiments/20260615_202341_diverse_glm_s16180_iter200/output/best/best_program.cpp`

Checkpoint files are under each run's `output/checkpoints/checkpoint_200/`.

## Strategy Comparison

All three best policies are Unify-heavy, but they differ in how they use Full and Partitioned.

| Run | Fitness | Full Ratio | Partitioned Ratio | Unify Ratio | Transitions | Interpretation |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `partbase` | `1.182825` | `1.285%` | `0.000%` | `98.715%` | `0` | Static level-based policy, almost pure Unify |
| `seed27182` | `1.206468` | `2.018%` | `14.599%` | `83.383%` | `19748` | Best policy; adaptive hotness/cache/metadata policy |
| `seed16180` | `1.149080` | `0.000%` | `12.333%` | `87.667%` | `15526` | Workload-adaptive but has a high-cache zipfian regression |

### Partbase Best

Policy shape:

- `level <= 2`: Full
- `level <= 4`: Unify
- otherwise: Partitioned

Observed behavior:

- Despite the level rule, evaluated scenarios ended up almost entirely Unify.
- No scheme transitions.
- Strong simple baseline: `1.1828`.

### Seed 27182 Best

Policy shape:

- Full for low levels, hot/cache-resident SSTs, point-lookup-heavy SSTs with affordable metadata.
- Partitioned for scan-heavy large SSTs or high filter rejection.
- Unify for medium-size or warm SSTs.

Key final rules:

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

This was the best overall policy. It improved on the static Unify-heavy solution by reintroducing Partitioned for selected large/filter-heavy cases and Full for a small number of hot/cache-resident cases.

Per-distribution geomean speedups:

- uniform: `1.233747`
- zipfian: `1.255852`
- mixgraph: `1.133400`

### Seed 16180 Best

Policy shape:

- Partitioned for very high cache-hit SSTs and high filter rejection with decent cache.
- Unify for hot, scan-heavy, low-cache, or cold SSTs.
- Full branch exists but did not materially trigger in final scenarios.

Weakness:

- `zipfian` at `2.0%` cache had speedup `0.92149`, worse than baseline.
- This regression pulled down the overall geomean despite good low-cache speedups.

## OpenEvolve Internal Behavior

The `seed27182` run was inspected through:

- `output/logs/openevolve_20260615_182729.log`
- `output/checkpoints/checkpoint_200/metadata.json`
- `output/checkpoints/checkpoint_200/programs/*.json`

Each program JSON includes:

- `id`
- `code`
- `parent_id`
- `generation`
- `iteration_found`
- `metrics`
- `metadata.island`
- `prompts.diff_user`

The prompt field stores:

- `system`
- `user`
- `responses`

For the best program, the stored user prompt was about `41KB` and included current program information, previous attempts, metrics, code, and model response history.

### Island Results for Seed 27182

Final island summary from checkpoint 200:

| Island | Count | Best | Average | Median | Notes |
| ---: | ---: | ---: | ---: | ---: | --- |
| 0 | 27 | `1.206468` | `1.123834` | `1.206391` | Winning lineage |
| 1 | 23 | `1.010738` | `1.007950` | `1.010612` | Near-baseline lineage |
| 2 | 26 | `1.000003` | `0.996280` | `1.000000` | Mostly stalled |
| 3 | 26 | `1.168660` | `1.153246` | `1.155544` | Strong secondary lineage |
| 4 | 26 | `1.152048` | `1.115349` | `1.152048` | Mid-performing lineage |
| 5 | 21 | `1.152048` | `1.012339` | `0.999802` | Mixed quality |

Feature-map cell counts:

```text
island 0: 13 cells
island 1: 13 cells
island 2: 12 cells
island 3: 14 cells
island 4: 7 cells
island 5: 8 cells
archive: 90 programs
```

Island generations:

```text
[34, 33, 33, 33, 31, 33]
```

Because `migration_interval` was `40`, no island reached the migration threshold during this 200-iteration run. The experiment therefore behaved mostly like six semi-independent island searches rather than a migration-heavy island model.

### Winning Lineage for Seed 27182

Best ancestry:

```text
initial all-Partitioned
  -> iter 7,   fitness 1.017350255
  -> iter 49,  fitness 1.030632897
  -> iter 109, fitness 1.206390722
  -> iter 169, fitness 1.206468338
```

The largest improvement occurred at iteration 109, when the policy relaxed the Partitioned thresholds and expanded Unify fallback coverage.

The final iteration 169 improvement was small but added more aggressive Full rules:

- `cache_hit_rate > 0.85 && hot > 0.05 && metadata fits`
- lower `access_count` Full threshold from `300000` to `150000`

## Interpretation

The repeated all-Partitioned experiments were useful:

- The best corrected result improved from `1.1828` to `1.2065`.
- Different seeds produced meaningfully different policy lineages.
- However, all top policies still converged toward Unify-heavy behavior.

The current OpenEvolve diversity settings preserve code-shape diversity more than behavior-level policy diversity. This is because the current feature dimensions are `complexity/diversity/score`, not metrics such as actual scheme ratios.

## Recommended Next Experiment

Add evaluator-returned behavior features and use them as MAP-Elites dimensions.

Suggested evaluator metrics:

- `unify_ratio`
- `full_ratio`
- `transition_rate`

Suggested config:

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

Keep:

```yaml
fitness: geomean speedup vs all-Partitioned
combined_score: same as fitness
```

Do not add a diversity bonus into fitness. Keeping fitness pure makes result interpretation cleaner.

Also consider lowering migration interval:

```yaml
database:
  migration_interval: 15
```

This would let a 200-iteration, 6-island run actually perform migrations. With the current `migration_interval: 40`, the 200-iteration run did not migrate.

## Useful Commands

Summarize all experiments:

```bash
python3 scripts/compare_experiments.py experiments
```

Inspect the best program for the winning run:

```bash
sed -n '/EVOLVE-BLOCK-START/,/EVOLVE-BLOCK-END/p' \
  experiments/20260615_182729_diverse_glm_s27182_iter200/output/best/best_program.cpp
```

Inspect checkpoint metadata:

```bash
python3 - <<'PY'
import json
from pathlib import Path
p = Path('experiments/20260615_182729_diverse_glm_s27182_iter200/output/checkpoints/checkpoint_200/metadata.json')
print(json.dumps(json.loads(p.read_text()).keys(), indent=2, default=str))
PY
```

Inspect one program's stored prompt:

```bash
python3 - <<'PY'
import json
from pathlib import Path
pid = '08b10079-04e1-48bc-95c4-24f68d79d1f1'
p = Path('experiments/20260615_182729_diverse_glm_s27182_iter200/output/checkpoints/checkpoint_200/programs') / f'{pid}.json'
d = json.loads(p.read_text())
print(d['prompts']['diff_user'].keys())
print(d['prompts']['diff_user']['user'][:2000])
PY
```
