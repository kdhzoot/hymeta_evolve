#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

ITERATIONS="${ITERATIONS:-200}"
START="${START:-initial_program_diverse.cpp}"
CONFIG="${CONFIG:-config_diverse.yaml}"
RUN_NAME="${RUN_NAME:-diverse_${ITERATIONS}_$(date +%Y%m%d_%H%M%S)}"
OUTPUT="${OUTPUT:-experiments/$RUN_NAME}"

mkdir -p "$OUTPUT"

finish() {
  local code=$?
  if [[ -f "$OUTPUT/experiment_manifest.json" ]]; then
    python3 scripts/summarize_evolve_run.py "$OUTPUT" || true
  fi
  exit "$code"
}
trap finish EXIT

python3 - "$RUN_NAME" "$OUTPUT" "$ITERATIONS" "$START" "$CONFIG" <<'PY'
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

import yaml

run_name, output, iterations, start, config = sys.argv[1:]
cfg = yaml.safe_load(Path(config).read_text())
manifest = {
    "run_name": run_name,
    "output_dir": str(Path(output).resolve()),
    "started_at": datetime.now(timezone.utc).isoformat(),
    "iterations_requested": int(iterations),
    "start_program": start,
    "config": config,
    "config_snapshot": cfg,
}
Path(output, "experiment_manifest.json").write_text(json.dumps(manifest, indent=2))
Path(output, "start_program.cpp").write_text(Path(start).read_text())
Path(output, "config_snapshot.yaml").write_text(Path(config).read_text())
PY

openevolve-run "$START" evaluator.py \
  --config "$CONFIG" \
  --iterations "$ITERATIONS" \
  --output "$OUTPUT"

python3 scripts/summarize_evolve_run.py "$OUTPUT"
