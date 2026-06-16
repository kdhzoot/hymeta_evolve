#!/bin/bash
# Measure NVMe random read bandwidth across block sizes.
# Mirrors RocksDB read pattern: direct I/O, 16 threads, random read.
set -euo pipefail

TEST_DIR=/work/fio_test
TEST_FILE="$TEST_DIR/fio_randread.dat"
TEST_SIZE=10G
NTHREADS=16
RUNTIME=30
OUT=/home/godong/hymeta_evolve/bench_results/fio_nvme_bw.txt

mkdir -p "$TEST_DIR"

# Create test file once (10 GB)
if [[ ! -f "$TEST_FILE" ]] || [[ $(stat -c%s "$TEST_FILE") -lt 10737418240 ]]; then
  echo "[fio] creating 10GB test file (first time only)..."
  fio --name=prepare --rw=write --bs=1M --size="$TEST_SIZE" --filename="$TEST_FILE" \
      --direct=1 --numjobs=1 --iodepth=32 --end_fsync=1 > /dev/null 2>&1
fi

echo "=== NVMe Random Read Bandwidth Sweep ===" | tee "$OUT"
echo "target: $TEST_FILE ($(du -h $TEST_FILE | awk '{print $1}'))" | tee -a "$OUT"
echo "threads=$NTHREADS  runtime=${RUNTIME}s  direct=1" | tee -a "$OUT"
echo "" | tee -a "$OUT"
printf "%-10s %-12s %-12s %-10s\n" "bs" "GB/s" "KIOPS" "avg_us" | tee -a "$OUT"
printf "%-10s %-12s %-12s %-10s\n" "--" "----" "-----" "------" | tee -a "$OUT"

for bs in 4k 16k 64k 128k 256k 512k 1m; do
  result=$(fio --name=rand_${bs} \
               --filename="$TEST_FILE" \
               --rw=randread \
               --bs="$bs" \
               --direct=1 \
               --numjobs="$NTHREADS" \
               --iodepth=1 \
               --runtime="$RUNTIME" \
               --time_based \
               --group_reporting \
               --output-format=json 2>/dev/null)

  # Extract via grep/awk (no jq dep)
  bw_kbs=$(echo "$result" | grep -m1 '"bw"' | awk -F'[,:]' '{gsub(/[ "]/,""); print $2}')
  iops=$(echo "$result" | grep -m1 '"iops"' | awk -F'[,:]' '{gsub(/[ "]/,""); print $2}')
  lat_ns=$(echo "$result" | grep -m1 '"mean"' | awk -F'[,:]' '{gsub(/[ "]/,""); print $2}')
  lat_us=$(awk -v n="$lat_ns" 'BEGIN { printf "%.2f", n/1000 }')

  gb_s=$(awk -v b="$bw_kbs" 'BEGIN { printf "%.2f", b/1024/1024 }')
  kiops=$(awk -v i="$iops" 'BEGIN { printf "%.1f", i/1000 }')
  printf "%-10s %-12s %-12s %-10s\n" "$bs" "$gb_s" "$kiops" "$lat_us" | tee -a "$OUT"
done

echo "" | tee -a "$OUT"
echo "Result saved: $OUT"
