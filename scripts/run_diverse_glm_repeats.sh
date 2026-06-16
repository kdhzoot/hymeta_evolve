#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

BASE_CONFIG="${BASE_CONFIG:-config_diverse_glm.yaml}"
START="${START:-initial_program_diverse.cpp}"
ITERATIONS="${ITERATIONS:-200}"
SEEDS="${SEEDS:-27182 16180}"
WAIT_RUN_DIR="${WAIT_RUN_DIR:-}"
BATCH_ID="${BATCH_ID:-$(date +%Y%m%d_%H%M%S)_diverse_glm_repeats}"
BATCH_DIR="${BATCH_DIR:-experiments/${BATCH_ID}}"
EXP_ROOT="${EXP_ROOT:-experiments}"

mkdir -p "$BATCH_DIR/configs"

LOG="${BATCH_DIR}/batch.log"
RUN_DIRS="${BATCH_DIR}/run_dirs.txt"
STATUS="${BATCH_DIR}/status.json"
: > "$LOG"
: > "$RUN_DIRS"

log() {
  printf '[%(%Y-%m-%d %H:%M:%S)T] %s\n' -1 "$*" | tee -a "$LOG"
}

write_status() {
  local state="$1"
  python3 - "$STATUS" "$state" "$BATCH_ID" "$BATCH_DIR" <<'PY'
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

path, state, batch_id, batch_dir = sys.argv[1:]
payload = {
    "status": state,
    "batch_id": batch_id,
    "batch_dir": batch_dir,
    "updated_at_utc": datetime.now(timezone.utc).isoformat(),
}
path = Path(path)
path.write_text(json.dumps(payload, indent=2))
PY
}

make_seed_config() {
  local seed="$1"
  local out="$2"
  python3 - "$BASE_CONFIG" "$out" "$seed" <<'PY'
import sys
from pathlib import Path

import yaml

base, out, seed_s = sys.argv[1:]
seed = int(seed_s)
cfg = yaml.safe_load(Path(base).read_text())
cfg["random_seed"] = seed
cfg.setdefault("database", {})["random_seed"] = seed
cfg.setdefault("llm", {})["random_seed"] = seed
Path(out).write_text(yaml.safe_dump(cfg, sort_keys=False))
PY
}

wait_for_run() {
  local run_dir="$1"
  local output_dir="${run_dir}/output"

  log "waiting for ${run_dir}"
  while [[ ! -f "${run_dir}/status.json" ]]; do
    if ! pgrep -af "openevolve-run .*--output ${output_dir}" >/dev/null; then
      log "warning: no openevolve process found for ${run_dir}, but status.json is missing"
      return 1
    fi
    sleep 60
  done

  log "summarizing ${run_dir}"
  python3 scripts/summarize_evolve_run.py "$run_dir" | tee "${run_dir}/summary.stdout.json" >> "$LOG"
  printf '%s\n' "$run_dir" >> "$RUN_DIRS"
}

write_status "running"
log "batch ${BATCH_ID} started"

if [[ -n "$WAIT_RUN_DIR" ]]; then
  wait_for_run "$WAIT_RUN_DIR" || true
fi

for seed in $SEEDS; do
  cfg="${BATCH_DIR}/configs/config_diverse_glm_seed_${seed}.yaml"
  label="diverse_glm_s${seed}"

  make_seed_config "$seed" "$cfg"
  log "starting seed ${seed}: ${label}"

  ITERATIONS="$ITERATIONS" \
    LABEL="$label" \
    CONFIG="$cfg" \
    START="$START" \
    EXP_ROOT="$EXP_ROOT" \
    scripts/evolve_recorded.sh 2>&1 | tee -a "$LOG"

  run_dir="$(ls -td "${EXP_ROOT}"/*_"${label}"_iter"${ITERATIONS}" 2>/dev/null | head -1 || true)"
  if [[ -n "$run_dir" ]]; then
    printf '%s\n' "$run_dir" >> "$RUN_DIRS"
  fi
done

log "refreshing comparison"
python3 scripts/compare_experiments.py "$EXP_ROOT" | tee "${EXP_ROOT}/latest_comparison.txt" | tee -a "$LOG"
write_status "completed"
log "batch ${BATCH_ID} completed"
