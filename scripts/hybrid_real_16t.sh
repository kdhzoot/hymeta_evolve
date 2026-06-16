#!/bin/bash
# Real-DB validation of 4 hybrid per-level policies (P1-P4) at 16T.
#
# Iterates through all cache pcts in one run (was previously 3 separate runs).
# Uses --metadata_format_preference=himeta + --himeta_level_preference=F,P
# to select scheme per SST level at runtime (no DB reload — HyMeta SSTs
# carry all 3 metadata formats).
#
# Default: pct = 2 1 0.5  (override via env: PCTS="2 1 0.5 0.2")
#
# Output:
#   bench_results/exp_hybrid_16t_<pct>pct_<TS>.csv  (one CSV per cache pct)
#   bench_results/exp_hybrid_16t_<pct>pct_<TS>.log  (master log per cache pct)
set -euo pipefail

DB_BENCH=/home/godong/himeta/db_bench
DB=/work/db/250gb_himeta
NUM=2949840175
OUT=/home/godong/hymeta_evolve/bench_results
DURATION=600
DB_SIZE=$((250 * 1024 * 1024 * 1024))

PCTS=${PCTS:-"2 1 0.5"}

drop() { sync; sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true; }

mkdir -p "$OUT"

for PCT in $PCTS; do
  TS=$(date '+%Y%m%d_%H%M%S')
  PCT_LABEL="${PCT//./p}pct"
  LOG="$OUT/exp_hybrid_16t_${PCT_LABEL}_${TS}.log"
  CSV="$OUT/exp_hybrid_16t_${PCT_LABEL}_${TS}.csv"
  CACHE_BYTES=$(awk -v db="$DB_SIZE" -v p="$PCT" 'BEGIN { printf "%.0f", db * p / 100 }')

  echo "policy,level_pref,cache_pct,cache_bytes,us_per_op,ops_sec,cache_hit,cache_miss,duration_s" > "$CSV"
  echo "[hybrid_real_16t pct=${PCT}%] TS=$TS  log=$LOG" | tee "$LOG"
  echo "DB=$DB NUM=$NUM duration=${DURATION}s cache=${CACHE_BYTES}" | tee -a "$LOG"

  common_flags=(
    --db="$DB" --use_existing_db=true --num="$NUM"
    --duration="$DURATION" --threads=16
    --key_size=48 --value_size=43 --bloom_bits=10 --block_size=4096
    --compression_type=none --checksum_type=1
    --use_himeta_scheme=true --metadata_format_preference=himeta
    --cache_type=hyper_clock_cache --cache_numshardbits=-1
    --cache_index_and_filter_blocks=true
    --use_direct_reads=true --use_direct_io_for_flush_and_compaction=true
    --disable_wal=true --open_files=-1
    --statistics=true --perf_level=3 --histogram=true --seed=42
    --enable_index_compression=false --index_shortening_mode=1
    --cache_size="$CACHE_BYTES"
  )

  run_one() {
    local name="$1" pref="$2"
    local run_log="$OUT/exp_hybrid_16t_${PCT_LABEL}_${name}_${TS}.log"
    echo "==== ${name}  pref=${pref}  pct=${PCT}%  $(date) ====" | tee -a "$LOG"
    drop
    "$DB_BENCH" --benchmarks=readrandom "${common_flags[@]}" \
      --himeta_level_preference="$pref" 2>&1 | tee "$run_log" > /dev/null

    local us_per_op ops_sec cache_hit cache_miss
    us_per_op=$(grep -m1 "readrandom.*micros/op" "$run_log" | awk '{print $3}')
    ops_sec=$(grep -m1 "readrandom.*ops/sec" "$run_log" | awk '{print $5}')
    cache_hit=$(grep -m1 "rocksdb.block.cache.hit COUNT" "$run_log" | awk '{print $NF}')
    cache_miss=$(grep -m1 "rocksdb.block.cache.miss COUNT" "$run_log" | awk '{print $NF}')
    echo "  us/op=${us_per_op}  ops/sec=${ops_sec}" | tee -a "$LOG"
    echo "${name},${pref},${PCT},${CACHE_BYTES},${us_per_op},${ops_sec},${cache_hit},${cache_miss},${DURATION}" >> "$CSV"
  }

  run_one "P1_seed"      "2,3"
  run_one "P2_more_full" "3,3"
  run_one "P3_less_full" "1,3"
  run_one "P4_more_part" "2,4"

  echo "[hybrid_real_16t pct=${PCT}%] done $(date) → CSV=$CSV" | tee -a "$LOG"
done
