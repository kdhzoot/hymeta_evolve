#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

ITERATIONS="${ITERATIONS:-200}"
LABEL="${LABEL:-diverse}"
CONFIG="${CONFIG:-config_diverse.yaml}"
START="${START:-initial_program_diverse.cpp}"
EXP_ROOT="${EXP_ROOT:-experiments}"
SKIP_LLM_CHECK="${SKIP_LLM_CHECK:-0}"

RUN_ID="$(date +%Y%m%d_%H%M%S)_${LABEL}_iter${ITERATIONS}"
RUN_DIR="${EXP_ROOT}/${RUN_ID}"
OUTPUT_DIR="${RUN_DIR}/output"

mkdir -p "$RUN_DIR" "$OUTPUT_DIR"

cp "$CONFIG" "${RUN_DIR}/config.snapshot.yaml"
cp "$START" "${RUN_DIR}/start.snapshot.cpp"
cp evaluator.py "${RUN_DIR}/evaluator.snapshot.py"
cp initial_program.cpp "${RUN_DIR}/initial_program.before.cpp"

python3 - "$RUN_DIR" "$RUN_ID" "$LABEL" "$ITERATIONS" "$CONFIG" "$START" "$OUTPUT_DIR" <<'PY'
import hashlib
import json
import os
import platform
import sys
from datetime import datetime, timezone
from pathlib import Path

run_dir, run_id, label, iterations, config, start, output_dir = sys.argv[1:]

def sha256(path):
    data = Path(path).read_bytes()
    return hashlib.sha256(data).hexdigest()

metadata = {
    "run_id": run_id,
    "label": label,
    "created_at_utc": datetime.now(timezone.utc).isoformat(),
    "cwd": os.getcwd(),
    "hostname": platform.node(),
    "iterations": int(iterations),
    "config": config,
    "start": start,
    "output_dir": output_dir,
    "command": [
        "openevolve-run",
        start,
        "evaluator.py",
        "--config",
        config,
        "--iterations",
        iterations,
        "--output",
        output_dir,
    ],
    "sha256": {
        "config": sha256(config),
        "start": sha256(start),
        "evaluator": sha256("evaluator.py"),
        "baseline_cache": sha256("baseline_cache.json") if Path("baseline_cache.json").exists() else None,
    },
}
Path(run_dir, "metadata.json").write_text(json.dumps(metadata, indent=2))
print(json.dumps(metadata, indent=2))
PY

if [[ "$SKIP_LLM_CHECK" != "1" ]]; then
  api_base="$(python3 - "$CONFIG" <<'PY'
import sys
import yaml

with open(sys.argv[1]) as f:
    cfg = yaml.safe_load(f)
print(cfg.get("llm", {}).get("api_base", "").rstrip("/"))
PY
)"
  api_key="$(python3 - "$CONFIG" <<'PY'
import sys
import yaml

with open(sys.argv[1]) as f:
    cfg = yaml.safe_load(f)
print(cfg.get("llm", {}).get("api_key", ""))
PY
)"
  if [[ -n "$api_base" ]]; then
    curl_args=(-fsS --max-time 5)
    if [[ -n "$api_key" ]]; then
      curl_args+=(-H "Authorization: Bearer ${api_key#Bearer }")
    fi
    if ! curl "${curl_args[@]}" "${api_base}/models" >/dev/null; then
      python3 - "$RUN_DIR" <<'PY'
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

run_dir = sys.argv[1]
status = {
    "status": "failed_preflight",
    "exit_code": 20,
    "reason": "LLM endpoint did not respond to /models preflight",
    "finished_at_utc": datetime.now(timezone.utc).isoformat(),
}
Path(run_dir, "status.json").write_text(json.dumps(status, indent=2))
PY
      python3 scripts/summarize_evolve_run.py "$RUN_DIR" | tee "${RUN_DIR}/summary.stdout.json"
      python3 scripts/compare_experiments.py "$EXP_ROOT" | tee "${EXP_ROOT}/latest_comparison.txt"
      echo "LLM preflight failed for ${api_base}/models; not starting evolution." >&2
      exit 20
    fi
  fi
fi

set +e
openevolve-run "$START" evaluator.py \
  --config "$CONFIG" \
  --iterations "$ITERATIONS" \
  --output "$OUTPUT_DIR" \
  2>&1 | tee "${RUN_DIR}/console.log"
exit_code="${PIPESTATUS[0]}"
set -e

python3 - "$RUN_DIR" "$exit_code" <<'PY'
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

run_dir, exit_code = sys.argv[1], int(sys.argv[2])
status = {
    "status": "completed" if exit_code == 0 else "failed",
    "exit_code": exit_code,
    "finished_at_utc": datetime.now(timezone.utc).isoformat(),
}
Path(run_dir, "status.json").write_text(json.dumps(status, indent=2))
PY

cp "${RUN_DIR}/initial_program.before.cpp" initial_program.cpp

python3 scripts/summarize_evolve_run.py "$RUN_DIR" | tee "${RUN_DIR}/summary.stdout.json"
python3 scripts/compare_experiments.py "$EXP_ROOT" | tee "${EXP_ROOT}/latest_comparison.txt"

exit "$exit_code"
