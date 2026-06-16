#!/bin/bash
# Background experiment (real DB): Full vs Partitioned at tight cache budgets.
# 4 cache sizes x 2 schemes x 10 min = ~80 min total.
set -euo pipefail

DB_BENCH=/home/godong/himeta/db_bench
DB=/work/db/250gb_himeta
NUM=2949840175
OUT=/home/godong/hymeta_evolve/bench_results
DURATION=600
TS=$(date '+%Y%m%d_%H%M%S')
LOG="$OUT/exp_cache_budget_real_16t_${TS}.log"
CSV="$OUT/exp_cache_budget_real_16t_${TS}.csv"

DB_SIZE=$((250 * 1024 * 1024 * 1024))    # 250 GB

mkdir -p "$OUT"
echo "cache_pct,cache_bytes,scheme,us_per_op,ops_sec,cache_hit,cache_miss,duration_s" > "$CSV"

echo "[exp_cache_budget_real_16t] TS=$TS  log=$LOG" | tee "$LOG"
echo "DB=$DB NUM=$NUM duration=${DURATION}s" | tee -a "$LOG"

common_flags=(
  --db="$DB"
  --use_existing_db=true
  --num="$NUM"
  --duration="$DURATION"
  --threads=16
  --key_size=48
  --value_size=43
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
  --index_shortening_mode=1
)

drop() {
  sync
  sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
}

run_one() {
  local pct="$1"
  local scheme="$2"
  local cache_bytes
  cache_bytes=$(awk -v db="$DB_SIZE" -v p="$pct" 'BEGIN { printf "%.0f", db * p / 100 }')
  local pct_label="${pct//./p}"
  local run_log="$OUT/exp_cb_16t_${pct_label}pct_${scheme}.log"

  echo "==== pct=${pct}%  cache=${cache_bytes}  scheme=${scheme}  $(date) ====" | tee -a "$LOG"
  drop
  "$DB_BENCH" --benchmarks=readrandom \
    "${common_flags[@]}" \
    --metadata_format_preference="$scheme" \
    --cache_size="$cache_bytes" \
    2>&1 | tee "$run_log" > /dev/null

  # Parse key metrics from run_log
  local us_per_op ops_sec cache_hit cache_miss
  us_per_op=$(grep -m1 "readrandom.*micros/op" "$run_log" | awk '{print $3}')
  ops_sec=$(grep -m1 "readrandom.*ops/sec" "$run_log" | awk '{print $5}')
  cache_hit=$(grep -m1 "rocksdb.block.cache.hit COUNT" "$run_log" | awk '{print $NF}')
  cache_miss=$(grep -m1 "rocksdb.block.cache.miss COUNT" "$run_log" | awk '{print $NF}')
  echo "  us/op=${us_per_op}  ops_sec=${ops_sec}  cache_hit=${cache_hit}  cache_miss=${cache_miss}" | tee -a "$LOG"

  echo "${pct},${cache_bytes},${scheme},${us_per_op},${ops_sec},${cache_hit},${cache_miss},${DURATION}" >> "$CSV"
}

for pct in 5 2 1 0.5; do
  for scheme in full partitioned; do
    run_one "$pct" "$scheme"
  done
  echo "" | tee -a "$LOG"
done

echo "[exp_cache_budget_real_16t] all done $(date)" | tee -a "$LOG"
echo "CSV: $CSV"
