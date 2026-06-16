#!/bin/bash
# Real-DB benchmark — extended scenarios at 16T, --duration=600s each.
#
# Self-contained: inlines the per-config runner (no external one_policy.sh).
#
# Scope (14 configs by default):
#   2%/1%/0.5% cache: P5_hymeta_default(2,2) + all_Unify  (6)
#   0.2% cache: P1-P5 + all_Full/Part/Unify              (8)
#
# Total wall time: ~14 × 10 min ≈ 2 h 20 min.
# Output:
#   bench_results/exp_extended_16t.csv  (consolidated CSV, appended)
#   bench_results/exp_ext_16t_<pct>_<label>_<TS>.log  (per-run db_bench log)
set -euo pipefail

DB_BENCH=/home/godong/himeta/db_bench
DB=/work/db/250gb_himeta
NUM=2949840175
OUT=/home/godong/hymeta_evolve/bench_results
DURATION=600
DB_SIZE=$((250 * 1024 * 1024 * 1024))
CSV="$OUT/exp_extended_16t.csv"
MASTER_LOG="$OUT/exp_extended_master.log"

mkdir -p "$OUT"
[[ -f "$CSV" ]] || \
  echo "timestamp,label,mode,level_pref,cache_pct,cache_bytes,us_per_op,ops_sec,cache_hit,cache_miss,duration_s" > "$CSV"

drop_caches() { sync; sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true; }

run_one() {
  local pct="$1" label="$2" mode="$3" level_pref="${4:-}"
  local ts=$(date '+%Y%m%d_%H%M%S')
  local pct_label="${pct//./p}pct"
  local run_log="$OUT/exp_ext_16t_${pct_label}_${label}_${ts}.log"
  local cache_bytes
  cache_bytes=$(awk -v db="$DB_SIZE" -v p="$pct" 'BEGIN { printf "%.0f", db * p / 100 }')

  echo "[$(date '+%H:%M:%S')] pct=${pct}% label=${label} mode=${mode} pref=${level_pref:-N/A} cache=${cache_bytes}" \
    | tee -a "$MASTER_LOG"

  local args=(
    --benchmarks=readrandom
    --db="$DB" --use_existing_db=true --num="$NUM" --duration="$DURATION" --threads=16
    --key_size=48 --value_size=43 --bloom_bits=10 --block_size=4096
    --compression_type=none --checksum_type=1
    --use_himeta_scheme=true
    --cache_type=hyper_clock_cache --cache_numshardbits=-1
    --cache_index_and_filter_blocks=true
    --use_direct_reads=true --use_direct_io_for_flush_and_compaction=true
    --disable_wal=true --open_files=-1
    --statistics=true --perf_level=3 --histogram=true --seed=42
    --enable_index_compression=false --index_shortening_mode=1
    --cache_size="$cache_bytes"
    --metadata_format_preference="$mode"
  )
  if [[ "$mode" == "himeta" ]]; then
    [[ -n "$level_pref" ]] || { echo "ERROR: himeta needs level_pref" >&2; exit 2; }
    args+=(--himeta_level_preference="$level_pref")
  fi

  drop_caches
  "$DB_BENCH" "${args[@]}" 2>&1 | tee "$run_log" > /dev/null

  local us_per_op ops_sec cache_hit cache_miss
  us_per_op=$(grep -m1 "readrandom.*micros/op" "$run_log" | awk '{print $3}')
  ops_sec=$(grep -m1 "readrandom.*ops/sec" "$run_log" | awk '{print $5}')
  cache_hit=$(grep -m1 "rocksdb.block.cache.hit COUNT" "$run_log" | awk '{print $NF}')
  cache_miss=$(grep -m1 "rocksdb.block.cache.miss COUNT" "$run_log" | awk '{print $NF}')
  echo "  → us/op=${us_per_op}  ops/sec=${ops_sec}" | tee -a "$MASTER_LOG"
  echo "${ts},${label},${mode},${level_pref},${pct},${cache_bytes},${us_per_op},${ops_sec},${cache_hit},${cache_miss},${DURATION}" >> "$CSV"
}

echo "==== extended_real_16t start $(date) ====" | tee -a "$MASTER_LOG"

# Round 1-3: P5 + all_Unify at 2%/1%/0.5%
for pct in 2 1 0.5; do
  run_one "$pct" P5_hymeta_default himeta 2,2
  run_one "$pct" all_Unify         unify
done

# Round 4: 0.2% cache — all 8 policies
for entry in \
  "P1_seed himeta 2,3" \
  "P2_more_full himeta 3,3" \
  "P3_less_full himeta 1,3" \
  "P4_more_part himeta 2,4" \
  "P5_hymeta_default himeta 2,2" \
  "all_Full full" \
  "all_Part partitioned" \
  "all_Unify unify"; do
  run_one 0.2 $entry
done

echo "==== extended_real_16t done $(date) ====" | tee -a "$MASTER_LOG"
echo "CSV: $CSV"
