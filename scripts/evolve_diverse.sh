#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

ITERATIONS="${ITERATIONS:-250}"
START="${START:-initial_program_diverse.cpp}"

if [[ -z "${OUTPUT:-}" ]]; then
  OUTPUT="evolve_runs_diverse_$(date +%Y%m%d_%H%M%S)"
fi

openevolve-run "$START" evaluator.py \
  --config config_diverse.yaml \
  --iterations "$ITERATIONS" \
  --output "$OUTPUT"
