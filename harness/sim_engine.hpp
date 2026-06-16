// HyMeta C++ Simulator - core engine interface.
// Port of /home/godong/hymeta_evolve/{lsm_tree,cache,io_model,config}.py
// with single-thread point-lookup + DES multi-thread saturation model.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace hymeta {

// ---- Enums ----

enum Scheme : int {
  SCHEME_FULL = 0,
  SCHEME_PARTITIONED = 1,
  SCHEME_UNIFY = 2,
};

enum BlockType : int {
  BT_FULL_FILTER = 0,
  BT_FULL_INDEX = 1,
  BT_PART_FILTER_TOP = 2,    // FilterPartIdx — top-level index of filter partitions
  BT_PART_FILTER_LEAF = 3,   // partition filter blocks
  BT_PART_INDEX_TOP = 4,     // top-level index of index partitions (separately measured)
  BT_PART_INDEX_LEAF = 5,    // partition index blocks
  BT_UNIFY_TOP = 6,
  BT_UNIFY_PART = 7,
  BT_DATA = 8,
  BT_COUNT = 9,
};

// ---- Config (matches config.py IOCostConfig) ----

struct IOCostConfig {
  // Cache lookup per block type (hit path, us).
  double cache_lookup_full_filter_us = 0.110;
  double cache_lookup_full_index_us = 0.184;
  double cache_lookup_part_filter_top_us = 0.177;   // FilterPartIdx, ~14KB
  double cache_lookup_part_filter_leaf_us = 0.273;
  double cache_lookup_part_index_top_us = 0.177;    // ~14KB top (assume ~= filter_top)
  double cache_lookup_part_index_leaf_us = 0.273;
  double cache_lookup_unify_top_us = 0.141;
  double cache_lookup_unify_part_us = 0.281;
  double cache_lookup_data_us = 0.311;
  double cache_insert_us = 0.40;

  // Disk I/O per block type (cold path, us). Partitioned values are calibrated
  // from the 5GB noshort DB measurement (cold, cache=1, single-thread); other
  // schemes are calibrated from the 250GB cold4 measurement.
  double disk_io_full_filter_us = 289.8;
  double disk_io_full_index_us = 194.2;
  double disk_io_part_filter_top_us = 75.4;   // FilterPartIdx, ~14.5KB
  double disk_io_part_filter_leaf_us = 99.1;  // ~3.6KB
  double disk_io_part_index_top_us = 76.7;    // ~14.7KB (NEW separation)
  double disk_io_part_index_leaf_us = 104.9;  // ~4KB
  double disk_io_unify_top_us = 85.4;
  double disk_io_unify_part_us = 102.7;
  double disk_io_data_us = 103.6;

  // Scheme-specific compute.
  double full_filter_compute_us = 0.35;
  double partitioned_filter_compute_us = 0.90;
  double unify_filter_compute_us = 0.99;
  double full_index_compute_us = 1.71;
  double partitioned_index_compute_us = 1.66;
  double unify_index_compute_us = 0.21;

  // Bandwidth saturation (post-hoc model).
  double disk_bandwidth_efficiency = 0.75;
  // (block_size_bytes, GB/s) table measured via fio.
  std::vector<std::pair<int, double>> disk_bandwidth_table = {
      {4 * 1024, 0.93e9},     {16 * 1024, 2.98e9},    {64 * 1024, 7.03e9},
      {128 * 1024, 11.27e9},  {256 * 1024, 15.47e9},  {512 * 1024, 19.25e9},
      {1024 * 1024, 21.51e9},
  };

  // Bloom FPR.
  double bloom_false_positive_rate = 0.0082;
};

struct LSMConfig {
  int num_levels = 7;
  int block_size = 4096;
  int metadata_block_size = 4096;
  int key_size = 48;      // real DB (fillrandom)
  int value_size = 43;    // real DB (fillrandom)
  int bloom_bits_per_key = 10;
  int64_t total_num_keys = 2949840175LL;
  // Index shortening vs naive (key_size + 8) * num_blocks.
  double index_shortening_factor = 0.37;
};

struct CacheConfig {
  int64_t capacity = 64LL << 20;  // 64 MB default
};

struct WorkloadConfig {
  // "uniform" | "zipfian" | "mixgraph"
  std::string distribution = "uniform";
  double zipfian_constant = 0.99;
  int64_t num_operations = 5'000'000;
  double found_rate = 0.63;  // probability a lookup hits a real key
  uint64_t seed = 42;

  // Mixgraph parameters (RocksDB db_bench port). Defaults from the
  // "Characterizing, Modeling, and Benchmarking RocksDB Key-Value Workloads
  // at Facebook" paper. Sim ignores Put (write modeling not implemented),
  // so put_ratio is folded into get internally.
  double mix_get_ratio = 0.83;
  double mix_seek_ratio = 0.17;
  // Prefix hotness:  f(x) = a*exp(b*x) + c*exp(d*x)
  double mix_keyrange_a = 14.18;
  double mix_keyrange_b = -0.3093;
  double mix_keyrange_c = 0.0;
  double mix_keyrange_d = 0.0;
  int64_t mix_keyrange_num = 30;
  // Within-keyrange power distribution:  y = a*x^b
  double mix_key_a = 0.002312;
  double mix_key_b = 0.3467;
};

struct SimConfig {
  LSMConfig lsm;
  CacheConfig cache;
  IOCostConfig io;
  WorkloadConfig wl;
  int num_threads = 1;         // DES virtual thread count
  std::string layout_path;     // JSON or bin file
};

// ---- SST file ----

struct SSTFile {
  int sst_id = 0;
  int level = 0;
  int64_t num_keys = 0;
  int64_t min_key = 0;
  int64_t max_key = 0;

  // Metadata sizes (computed from LSMConfig).
  int64_t full_index_size = 0;
  int64_t full_filter_size = 0;
  int64_t partitioned_filter_top_size = 0;   // FilterPartIdx block size
  int64_t partitioned_index_top_size = 0;    // separately measured top index
  int64_t partitioned_num_filter_partitions = 0;
  int64_t partitioned_num_index_partitions = 0;
  int64_t unify_top_index_size = 0;
  int64_t unify_num_partitions = 0;

  Scheme active_scheme = SCHEME_FULL;

  // Per-SST runtime stats (exposed to select_scheme via SSTStats).
  uint64_t access_count = 0;
  uint64_t point_lookup_count = 0;
  uint64_t scan_count = 0;
  uint64_t filter_checked = 0;
  uint64_t filter_rejected = 0;
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;

  bool contains(int64_t key) const { return key >= min_key && key <= max_key; }
};

// Stats snapshot passed to select_scheme (evolve target).
struct SSTStats {
  int sst_id;
  int level;
  int64_t num_keys;
  uint64_t access_count;
  uint64_t point_lookup_count;
  uint64_t scan_count;
  double filter_rejection_rate;
  double cache_hit_rate;
  int64_t full_metadata_size;
  int64_t partitioned_num_partitions;
  int64_t unify_num_partitions;
};

// ---- Cache ----

// Priority matches RocksDB Cache::Priority. HIGH entries start with initial
// CLOCK usage = 3; LOW entries start with usage = 1. Eviction logic is
// otherwise identical — HIGH just takes 3 sweeps to evict, LOW takes 1.
// Lookup hit still bumps usage to 3 (saturated). With
// cache_index_and_filter_blocks_with_high_priority=true (RocksDB default),
// data blocks are LOW and filter/index blocks are HIGH.
enum CachePriority : int { PRIO_LOW = 0, PRIO_HIGH = 1 };

struct CacheEntry {
  uint64_t key;
  int size;
  BlockType block_type;
  int sst_id;
};

struct ClockEntry {
  CacheEntry entry;
  int usage;  // 0..3
};

class HyperClockShard {
 public:
  explicit HyperClockShard(int64_t capacity) : capacity_(capacity) {}

  // Returns nullptr on miss, pointer to entry on hit (temporary - do not store).
  const CacheEntry* lookup(uint64_t key);
  // Insert if absent. Evicts via 2nd-chance CLOCK if needed.
  // priority=HIGH starts with usage=3, LOW with usage=1.
  void insert(uint64_t key, int size, BlockType bt, int sst_id,
              CachePriority priority);

  int64_t current_size = 0;
  uint64_t hits = 0, misses = 0, evictions = 0;
  uint64_t data_hits = 0, data_misses = 0;
  uint64_t metadata_hits = 0, metadata_misses = 0;

 private:
  void evict_one();
  int64_t capacity_;
  std::unordered_map<uint64_t, ClockEntry> entries_;
  std::deque<uint64_t> clock_queue_;
};

class HyperClockCache {
 public:
  HyperClockCache(int64_t capacity, int num_shard_bits = -1);

  const CacheEntry* lookup(uint64_t key) {
    return shards_[key & shard_mask_].lookup(key);
  }
  void insert(uint64_t key, int size, BlockType bt, int sst_id,
              CachePriority priority = PRIO_HIGH) {
    shards_[key & shard_mask_].insert(key, size, bt, sst_id, priority);
  }

  // Aggregated stats.
  uint64_t total_hits() const;
  uint64_t total_misses() const;
  uint64_t metadata_hits() const;
  uint64_t metadata_misses() const;
  uint64_t eviction_count() const;
  int64_t current_size() const;
  int64_t capacity() const { return capacity_; }

 private:
  int64_t capacity_;
  int num_shards_;
  uint64_t shard_mask_;
  std::vector<HyperClockShard> shards_;
};

// ---- Disk queue (DES) ----

// Single-server FIFO disk. Service time derived from bandwidth_for_size(size)
// which is the fio-measured, efficiency-scaled NVMe bandwidth. Models the
// aggregate contention when N virtual threads share the disk.
class DiskQueue {
 public:
  explicit DiskQueue(const IOCostConfig* cfg) : cfg_(cfg) {}

  // Submit an I/O of `size` arriving at `arrival_us`. Returns completion time.
  double submit(double arrival_us, int size);

  double next_free_us = 0.0;

 private:
  const IOCostConfig* cfg_;
};

// ---- Phase-level stats (matches real-DB cold*/warm* logs) ----
// A "phase" is filter or index evaluation. A phase is ALL_HIT if every block
// access within it was a cache hit; HAD_MISS if any block access missed.
// Tracked separately per scheme so we can compare against RocksDB's
// `filter Partitioned HAD_MISS ...` / `index Full ALL_HIT ...` etc.
enum Phase : int { PH_FILTER = 0, PH_INDEX = 1, PH_COUNT_PHASE = 2 };

struct PhaseStats {
  uint64_t all_hit_count = 0;
  uint64_t had_miss_count = 0;
  double all_hit_total_us = 0.0;
  double had_miss_total_us = 0.0;
};

// ---- I/O Model ----

class IOModel {
 public:
  IOModel(const IOCostConfig& cfg, HyperClockCache* cache);

  // Single-thread path: returns total us cost for this op.
  double point_lookup(SSTFile& sst, int64_t key, bool key_exists, std::mt19937_64& rng);

  // DES path: thread-time based. Caller passes `t_in` and `disk`. Returns
  // the new thread-time after the op completes (includes queue waits).
  double point_lookup_des(SSTFile& sst, int64_t key, bool key_exists,
                           std::mt19937_64& rng, double t_in, DiskQueue& disk);

  // Post-hoc saturation model (single-thread trace → N-thread throughput).
  double effective_us_per_op(double base_us_per_op, int64_t op_count, int num_threads) const;

  double bandwidth_for_size(double avg_io_size_bytes) const;

  uint64_t total_io_count = 0;
  double total_io_cost_us = 0.0;
  int64_t total_miss_bytes = 0;

  // phase_stats_[scheme][phase] — accessible for JSON output.
  std::array<std::array<PhaseStats, PH_COUNT_PHASE>, 3> phase_stats_{};

 private:
  double access_block(uint64_t cache_key, int size, BlockType bt, SSTFile& sst);
  void record_phase(int scheme, Phase ph, bool had_miss, double phase_us);
  // DES version: returns new thread-time.
  double access_block_des(uint64_t cache_key, int size, BlockType bt, SSTFile& sst,
                           double t_in, DiskQueue& disk);

  const IOCostConfig& cfg_;
  HyperClockCache* cache_;
  std::array<double, BT_COUNT> hit_cost_{};
  std::array<double, BT_COUNT> miss_cost_{};
};

// ---- LSM Tree ----

class LSMTree {
 public:
  LSMTree(const LSMConfig& cfg, std::vector<SSTFile> ssts);

  std::vector<SSTFile>& all_ssts() { return ssts_; }
  const std::vector<SSTFile>& all_ssts() const { return ssts_; }

  // Returns SSTs that might contain `key`, sorted by level ascending.
  // Output written into `out` (caller-provided, cleared by callee).
  void get_ssts_for_key(int64_t key, std::vector<SSTFile*>& out);

  const LSMConfig& config() const { return cfg_; }

  void compute_metadata_sizes();  // called in ctor

 private:
  LSMConfig cfg_;
  std::vector<SSTFile> ssts_;
  // Per-level indices into ssts_, sorted by min_key for L1+.
  std::vector<std::vector<int>> levels_;
};

// ---- Cache key encoding ----
// Packs (sst_id, block_tag, partition_id) into a 64-bit int so lookups avoid
// string hashing. Matches io_model.py layout: sst_id in low bits (shard
// distribution), tag in mid, partition in high bits.
inline uint64_t make_cache_key(int sst_id, int tag, uint64_t partition = 0) {
  return (uint64_t)sst_id | ((uint64_t)tag << 28) | (partition << 36);
}

// ---- Top-level sim result ----

struct SimResult {
  double base_us_per_op = 0.0;
  double effective_us_per_op = 0.0;
  int64_t op_count = 0;
  int64_t bytes_miss = 0;
  double cache_hit_rate = 0.0;
  double metadata_hit_rate = 0.0;
  uint64_t evictions = 0;
  double sim_wall_s = 0.0;
};

}  // namespace hymeta

// ---- Policy hook (evolved) ----
// Implemented in initial_program.cpp inside an EVOLVE-BLOCK.
namespace hymeta {
Scheme select_scheme(const SSTStats& s);
}
