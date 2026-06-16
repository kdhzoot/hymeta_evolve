#!/bin/bash
# Cold + warm bench for 250GB himeta DB (runsh-style flags)
set -euo pipefail

DB_BENCH=/home/godong/himeta/db_bench
DB=/work/db/250gb_himeta
NUM=2949840175
OUT=/home/godong/hymeta_evolve/bench_results

COLD_READS=100000
WARM_READS=500000
WARM_CACHE=$((32 * 1024 * 1024 * 1024))

common_flags=(
  --db="$DB"
  --use_existing_db=true
  --num="$NUM"
  --threads=1
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
  # use_direct_reads=true bypasses page cache, so drop_caches is best-effort
  sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
}

for scheme in full partitioned unify; do
  echo "==== COLD $scheme ===="
  drop
  "$DB_BENCH" --benchmarks=readrandom \
    "${common_flags[@]}" \
    --metadata_format_preference="$scheme" \
    --cache_size=1 \
    --reads="$COLD_READS" \
    2>&1 | tee "$OUT/cold4_${scheme}.log" > /dev/null
  echo "saved $OUT/cold4_${scheme}.log"
done

for scheme in full partitioned unify; do
  echo "==== WARM $scheme ===="
  drop
  "$DB_BENCH" --benchmarks=readrandom \
    "${common_flags[@]}" \
    --metadata_format_preference="$scheme" \
    --cache_size="$WARM_CACHE" \
    --reads="$WARM_READS" \
    2>&1 | tee "$OUT/warm4_${scheme}.log" > /dev/null
  echo "saved $OUT/warm4_${scheme}.log"
done

echo "all done"
