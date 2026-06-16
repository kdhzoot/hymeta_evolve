# HyMeta Evolve Experiments

This directory stores comparable OpenEvolve runs.

Run a diversity-oriented experiment:

```bash
ITERATIONS=200 RUN_NAME=diverse_200_a scripts/run_diverse_experiment.sh
```

Each run directory contains:

- `experiment_manifest.json`: run metadata and config snapshot
- `start_program.cpp`: exact starting policy
- `config_snapshot.yaml`: exact OpenEvolve config
- `best/best_program.cpp`: best evolved policy
- `best/best_program_info.json`: OpenEvolve best metrics
- `experiment_summary.json`: normalized run-level summary
- `per_scenario_summary.csv`: normalized scenario-level metrics

Master comparison files:

- `runs.csv`: one row per run
- `per_scenario.csv`: one row per run × scenario
