#!/bin/bash
# 5GB DB index_shortening_mode comparison.
# For each variant (short=1, noshort=0):
#   1. fillrandom load
#   2. drain pending compactions (single-thread readrandom loop until pending==0)
#   3. drop caches
#   4. cold partitioned measurement
# Then diff IdxPartTop AvgSize(B) between the two logs.

set -euo pipefail

DB_BENCH=/home/godong/himeta/db_bench
OUT_DIR=/home/godong/hymeta_evolve/bench_results
LOG_DIR=/home/godong/hymeta_evolve/log_5gb_shortening
mkdir -p "$OUT_DIR" "$LOG_DIR"

TARGET_DB_GB=5
KV_SIZE=91
KEY_SIZE=48
VALUE_SIZE=43
NKEYS=$(( TARGET_DB_GB * 1024 * 1024 * 1024 / KV_SIZE ))   # 59,055,940
COLD_READS=100000
DRAIN_READS=200000
DRAIN_CACHE=$((8 * 1024 * 1024 * 1024))
MIB=$((1024 * 1024))

# Shared flags (no DB-specific or shortening flags)
common=(
  --num="$NKEYS"
  --threads=1
  --key_size="$KEY_SIZE"
  --value_size="$VALUE_SIZE"
  --bloom_bits=10
  --block_size=4096
  --compression_type=none
  --checksum_type=1
  --use-himeta-scheme=true
  --cache_type=hyper_clock_cache
  --cache_numshardbits=-1
  --cache_index_and_filter_blocks=true
  --use_direct_reads=true
  --use_direct_io_for_flush_and_compaction=true
  --disable_wal=true
  --open_files=-1
  --statistics=true
  --perf_level=3
  --histogram=true
  --seed=42
  --enable_index_compression=false
)

# Load-specific (matching load_hi.sh)
load_extra=(
  --benchmarks=fillrandom,stats,levelstats
  --max_write_buffer_number=50
  --write_buffer_size=$((MIB * 512))
  --min_write_buffer_number_to_merge=1
  --max_background_jobs=128
  --level0_file_num_compaction_trigger=8
  --level0_slowdown_writes_trigger=24
  --level0_stop_writes_trigger=40
  --writable_file_max_buffer_size=$((MIB * 64))
  --compaction_readahead_size=$((MIB * 2))
  --compaction_style=0
  --max_bytes_for_level_base=$((MIB * 256))
  --target_file_size_base=$((MIB * 64))
  --memtablerep=vector
  --allow_concurrent_memtable_write=false
)

drop_caches() {
  sync
  sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
}

run_variant() {
  local label="$1"      # short | noshort
  local shortmode="$2"  # 1 | 0
  local db_dir="/work/db/${TARGET_DB_GB}gb_himeta_${label}"

  echo "============================================================"
  echo "[$(date '+%H:%M:%S')] Variant: $label (index_shortening_mode=$shortmode)"
  echo "DB dir: $db_dir"
  echo "============================================================"

  if [[ -d "$db_dir" ]]; then
    echo "[ERROR] DB already exists: $db_dir" >&2
    exit 1
  fi
  mkdir -p "$db_dir"

  # ---- Phase 1: LOAD ----
  echo "[$(date '+%H:%M:%S')] LOAD start"
  "$DB_BENCH" \
    "${common[@]}" \
    "${load_extra[@]}" \
    --index_shortening_mode="$shortmode" \
    --db="$db_dir" \
    --use_existing_db=false \
    --seed=12345678 \
    --cache_size=$((8 * 1024 * 1024 * 1024)) \
    > "$LOG_DIR/${label}_load.log" 2>&1
  echo "[$(date '+%H:%M:%S')] LOAD done"

  # ---- Phase 2: DRAIN (loop readrandom until pending compaction == 0) ----
  local attempt=0
  while :; do
    attempt=$((attempt + 1))
    echo "[$(date '+%H:%M:%S')] DRAIN attempt #$attempt"
    "$DB_BENCH" \
      "${common[@]}" \
      --benchmarks=readrandom,stats,levelstats \
      --index_shortening_mode="$shortmode" \
      --db="$db_dir" \
      --use_existing_db=true \
      --reads="$DRAIN_READS" \
      --cache_size="$DRAIN_CACHE" \
      > "$LOG_DIR/${label}_drain_${attempt}.log" 2>&1

    # Pending compaction bytes — extract from "Estimate Pending Compaction Bytes"
    # in stats. RocksDB prints "Estimated Pending Compaction Bytes: <num>"
    local pending
    pending=$(grep -oE "Estimated Pending Compaction Bytes: [0-9]+" \
              "$LOG_DIR/${label}_drain_${attempt}.log" | tail -1 | awk '{print $NF}')
    if [[ -z "$pending" ]]; then
      # Try alternate phrasing
      pending=$(grep -oE "Pending compaction bytes: [0-9]+" \
                "$LOG_DIR/${label}_drain_${attempt}.log" | tail -1 | awk '{print $NF}')
    fi
    pending=${pending:-unknown}
    echo "[$(date '+%H:%M:%S')] pending compaction bytes after attempt $attempt = $pending"

    if [[ "$pending" == "0" ]]; then
      echo "[$(date '+%H:%M:%S')] DRAIN done (pending=0)"
      break
    fi
    if [[ "$attempt" -ge 10 ]]; then
      echo "[WARN] drain still pending after $attempt attempts; giving up"
      break
    fi
    sleep 5
  done

  # ---- Phase 3: COLD MEASUREMENT ----
  drop_caches
  echo "[$(date '+%H:%M:%S')] COLD measurement start"
  local cold_log="$OUT_DIR/cold_partitioned_5gb_${label}.log"
  "$DB_BENCH" \
    "${common[@]}" \
    --benchmarks=readrandom \
    --index_shortening_mode="$shortmode" \
    --db="$db_dir" \
    --use_existing_db=true \
    --reads="$COLD_READS" \
    --cache_size=1 \
    --metadata_format_preference=partitioned \
    > "$cold_log" 2>&1
  echo "[$(date '+%H:%M:%S')] COLD done -> $cold_log"
}

run_variant short   1
run_variant noshort 0

echo
echo "================ COMPARISON: IdxPartTop (Partitioned index TOP-LEVEL) ================"
for label in short noshort; do
  echo
  echo "--- $label (index_shortening_mode=$([[ $label == short ]] && echo 1 || echo 0)) ---"
  grep -E "BlockType|FilterPartIdx|IdxPartTop|^Index|^Filter " \
    "$OUT_DIR/cold_partitioned_5gb_${label}.log" || true
done
echo
echo "DONE: $(date '+%Y-%m-%d %H:%M:%S')"
