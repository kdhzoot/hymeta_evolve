#include "sim_engine.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hymeta {

// ---------------- HyperClockShard ----------------

const CacheEntry* HyperClockShard::lookup(uint64_t key) {
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    misses += 1;
    return nullptr;
  }
  ClockEntry& ce = it->second;
  // RocksDB Lookup: FetchAdd(kAcquireIncrement) and Release: FetchAdd(
  // kReleaseIncrement). Both counters increase by 1 per Lookup+Release cycle,
  // so the "countdown" (acquire_count when no refs outstanding) grows without
  // cap BUT is capped back to kMaxCountdown-1 = 2 on the next CLOCK sweep.
  // We model this simply: increment on hit, no cap here.
  ce.usage += 1;
  hits += 1;
  if (ce.entry.block_type == BT_DATA)
    data_hits += 1;
  else
    metadata_hits += 1;
  return &ce.entry;
}

// Exactly matches RocksDB hyper_clock_cache.cc::GetInitialCountdown().
// Source: /home/godong/himeta/cache/clock_cache.cc:47-63
static constexpr int kHighCountdown = 3;
static constexpr int kLowCountdown = 2;    // DEFAULT when priority not set
static constexpr int kBottomCountdown = 1;
static constexpr int kMaxCountdown = kHighCountdown;  // cap on ClockUpdate

void HyperClockShard::insert(uint64_t key, int size, BlockType bt, int sst_id,
                              CachePriority priority) {
  auto it = entries_.find(key);
  if (it != entries_.end()) return;
  if (size > capacity_) return;
  while (current_size + size > capacity_ && !entries_.empty()) {
    evict_one();
  }
  ClockEntry ce;
  ce.entry = CacheEntry{key, size, bt, sst_id};
  // Match RocksDB: LOW=2 is the default (cache_high_pri_pool_ratio=0 means
  // all inserts are Cache::Priority::LOW, giving kLowCountdown=2).
  // Cold entries survive 2 CLOCK sweeps before eviction.
  ce.usage = (priority == PRIO_HIGH) ? kHighCountdown : kLowCountdown;
  entries_.emplace(key, ce);
  clock_queue_.push_back(key);
  current_size += size;
  if (bt == BT_DATA)
    data_misses += 1;
  else
    metadata_misses += 1;
}

void HyperClockShard::evict_one() {
  // RocksDB ClockUpdate (clock_cache.cc:133-152):
  //   if (Visible && acquire_count > 0) {
  //     new_count = min(acquire_count - 1, kMaxCountdown - 1);  // cap at 2
  //   } else if (acquire_count == 0) {
  //     evict;
  //   }
  // After a Lookup hit, acquire_count can grow >kMaxCountdown, but on the
  // NEXT sweep it's capped back to 2. So lookups grant "reset to 2" effect
  // rather than unbounded boost.
  while (!clock_queue_.empty()) {
    uint64_t k = clock_queue_.front();
    clock_queue_.pop_front();
    auto it = entries_.find(k);
    if (it == entries_.end()) continue;
    ClockEntry& ce = it->second;
    if (ce.usage == 0) {
      current_size -= ce.entry.size;
      entries_.erase(it);
      evictions += 1;
      return;
    }
    // Cap at kMaxCountdown - 1 = 2 after decrement (matches RocksDB).
    int new_count = ce.usage - 1;
    if (new_count > kMaxCountdown - 1) new_count = kMaxCountdown - 1;
    ce.usage = new_count;
    clock_queue_.push_back(k);
  }
}

// ---------------- HyperClockCache ----------------

static int auto_shard_bits(int64_t capacity) {
  if (capacity <= 0) return 0;
  int64_t target = std::max<int64_t>(1, capacity / (512LL << 20));
  target = std::min<int64_t>(64, target);
  int bits = 0;
  while ((1 << bits) < target) bits += 1;
  return bits;
}

HyperClockCache::HyperClockCache(int64_t capacity, int num_shard_bits)
    : capacity_(capacity) {
  if (num_shard_bits < 0) num_shard_bits = auto_shard_bits(capacity);
  num_shards_ = 1 << std::max(0, num_shard_bits);
  shard_mask_ = (uint64_t)num_shards_ - 1;
  int64_t per_shard = (capacity + num_shards_ - 1) / num_shards_;
  shards_.reserve(num_shards_);
  for (int i = 0; i < num_shards_; ++i) shards_.emplace_back(per_shard);
}

uint64_t HyperClockCache::total_hits() const {
  uint64_t n = 0;
  for (auto& s : shards_) n += s.hits;
  return n;
}
uint64_t HyperClockCache::total_misses() const {
  uint64_t n = 0;
  for (auto& s : shards_) n += s.misses;
  return n;
}
uint64_t HyperClockCache::metadata_hits() const {
  uint64_t n = 0;
  for (auto& s : shards_) n += s.metadata_hits;
  return n;
}
uint64_t HyperClockCache::metadata_misses() const {
  uint64_t n = 0;
  for (auto& s : shards_) n += s.metadata_misses;
  return n;
}
uint64_t HyperClockCache::eviction_count() const {
  uint64_t n = 0;
  for (auto& s : shards_) n += s.evictions;
  return n;
}
int64_t HyperClockCache::current_size() const {
  int64_t n = 0;
  for (auto& s : shards_) n += s.current_size;
  return n;
}

// ---------------- IOModel ----------------

IOModel::IOModel(const IOCostConfig& cfg, HyperClockCache* cache)
    : cfg_(cfg), cache_(cache) {
  double ins = cfg.cache_insert_us;
  hit_cost_[BT_DATA] = cfg.cache_lookup_data_us;
  hit_cost_[BT_FULL_FILTER] = cfg.cache_lookup_full_filter_us;
  hit_cost_[BT_FULL_INDEX] = cfg.cache_lookup_full_index_us;
  hit_cost_[BT_PART_FILTER_TOP] = cfg.cache_lookup_part_filter_top_us;
  hit_cost_[BT_PART_FILTER_LEAF] = cfg.cache_lookup_part_filter_leaf_us;
  hit_cost_[BT_PART_INDEX_TOP] = cfg.cache_lookup_part_index_top_us;
  hit_cost_[BT_PART_INDEX_LEAF] = cfg.cache_lookup_part_index_leaf_us;
  hit_cost_[BT_UNIFY_TOP] = cfg.cache_lookup_unify_top_us;
  hit_cost_[BT_UNIFY_PART] = cfg.cache_lookup_unify_part_us;

  miss_cost_[BT_DATA] = cfg.disk_io_data_us + ins;
  miss_cost_[BT_FULL_FILTER] = cfg.disk_io_full_filter_us + ins;
  miss_cost_[BT_FULL_INDEX] = cfg.disk_io_full_index_us + ins;
  miss_cost_[BT_PART_FILTER_TOP] = cfg.disk_io_part_filter_top_us + ins;
  miss_cost_[BT_PART_FILTER_LEAF] = cfg.disk_io_part_filter_leaf_us + ins;
  miss_cost_[BT_PART_INDEX_TOP] = cfg.disk_io_part_index_top_us + ins;
  miss_cost_[BT_PART_INDEX_LEAF] = cfg.disk_io_part_index_leaf_us + ins;
  miss_cost_[BT_UNIFY_TOP] = cfg.disk_io_unify_top_us + ins;
  miss_cost_[BT_UNIFY_PART] = cfg.disk_io_unify_part_us + ins;
}

// RocksDB cache_index_and_filter_blocks_with_high_priority=true (default):
// data blocks LOW, everything else HIGH.
static inline CachePriority priority_for(BlockType bt) {
  return (bt == BT_DATA) ? PRIO_LOW : PRIO_HIGH;
}

double IOModel::access_block(uint64_t cache_key, int size, BlockType bt,
                              SSTFile& sst) {
  const CacheEntry* e = cache_->lookup(cache_key);
  if (e != nullptr) {
    sst.cache_hits += 1;
    return hit_cost_[bt];
  }
  sst.cache_misses += 1;
  cache_->insert(cache_key, size, bt, sst.sst_id, priority_for(bt));
  total_io_count += 1;
  total_miss_bytes += size;
  double c = miss_cost_[bt];
  total_io_cost_us += c;
  return c;
}

double IOModel::access_block_des(uint64_t cache_key, int size, BlockType bt,
                                  SSTFile& sst, double t_in, DiskQueue& disk) {
  const CacheEntry* e = cache_->lookup(cache_key);
  if (e != nullptr) {
    sst.cache_hits += 1;
    return t_in + hit_cost_[bt];
  }
  sst.cache_misses += 1;
  cache_->insert(cache_key, size, bt, sst.sst_id, priority_for(bt));
  total_io_count += 1;
  total_miss_bytes += size;
  double arrival = t_in + hit_cost_[bt];
  double complete = disk.submit(arrival, size);
  return complete + cfg_.cache_insert_us;
}

double DiskQueue::submit(double arrival_us, int size) {
  // service_us = size (bytes) / bandwidth (bytes/s) * 1e6
  // Bandwidth interpolated from fio table, scaled by efficiency.
  const auto& tbl = cfg_->disk_bandwidth_table;
  double eff = cfg_->disk_bandwidth_efficiency;
  double bw = 15e9 * eff;
  if (!tbl.empty()) {
    if (size <= tbl.front().first) {
      bw = tbl.front().second * eff;
    } else if (size >= tbl.back().first) {
      bw = tbl.back().second * eff;
    } else {
      for (size_t i = 0; i + 1 < tbl.size(); ++i) {
        auto lo = tbl[i];
        auto hi = tbl[i + 1];
        if (size >= lo.first && size <= hi.first) {
          double frac = (double)(size - lo.first) / (double)(hi.first - lo.first);
          bw = (lo.second + frac * (hi.second - lo.second)) * eff;
          break;
        }
      }
    }
  }
  double service_us = (double)size / bw * 1'000'000.0;
  double start = arrival_us > next_free_us ? arrival_us : next_free_us;
  next_free_us = start + service_us;
  return next_free_us;
}

double IOModel::point_lookup_des(SSTFile& sst, int64_t key, bool key_exists,
                                  std::mt19937_64& rng, double t_in,
                                  DiskQueue& disk) {
  sst.access_count += 1;
  sst.point_lookup_count += 1;
  std::uniform_real_distribution<double> U(0.0, 1.0);
  int part_size = 4096;
  double t = t_in;

  auto bloom_rejects = [&]() {
    if (key_exists) return false;
    return U(rng) > cfg_.bloom_false_positive_rate;
  };

  if (sst.active_scheme == SCHEME_FULL) {
    t = access_block_des(make_cache_key(sst.sst_id, 1), (int)sst.full_filter_size,
                          BT_FULL_FILTER, sst, t, disk);
    t += cfg_.full_filter_compute_us;
    sst.filter_checked += 1;
    if (bloom_rejects()) {
      sst.filter_rejected += 1;
      return t;
    }
    t = access_block_des(make_cache_key(sst.sst_id, 2), (int)sst.full_index_size,
                          BT_FULL_INDEX, sst, t, disk);
    t += cfg_.full_index_compute_us;
    t = access_block_des(make_cache_key(sst.sst_id, 9, (uint64_t)(key % 10000)),
                          part_size, BT_DATA, sst, t, disk);
    return t;
  }

  if (sst.active_scheme == SCHEME_PARTITIONED) {
    int64_t span = sst.max_key - sst.min_key;
    int64_t fpid = 0, ipid = 0;
    if (span > 0) {
      double r = (double)(key - sst.min_key) / (double)span;
      int64_t nf = sst.partitioned_num_filter_partitions;
      int64_t ni = sst.partitioned_num_index_partitions;
      fpid = std::min<int64_t>((int64_t)(r * nf), nf - 1);
      ipid = std::min<int64_t>((int64_t)(r * ni), ni - 1);
    }
    t = access_block_des(make_cache_key(sst.sst_id, 3),
                          (int)sst.partitioned_filter_top_size,
                          BT_PART_FILTER_TOP, sst, t, disk);
    t = access_block_des(make_cache_key(sst.sst_id, 4, (uint64_t)fpid),
                          part_size, BT_PART_FILTER_LEAF, sst, t, disk);
    t += cfg_.partitioned_filter_compute_us;
    sst.filter_checked += 1;
    if (bloom_rejects()) {
      sst.filter_rejected += 1;
      return t;
    }
    t = access_block_des(make_cache_key(sst.sst_id, 5),
                          (int)sst.partitioned_index_top_size,
                          BT_PART_INDEX_TOP, sst, t, disk);
    t = access_block_des(make_cache_key(sst.sst_id, 6, (uint64_t)ipid),
                          part_size, BT_PART_INDEX_LEAF, sst, t, disk);
    t += cfg_.partitioned_index_compute_us;
    t = access_block_des(make_cache_key(sst.sst_id, 9, (uint64_t)(key % 10000)),
                          part_size, BT_DATA, sst, t, disk);
    return t;
  }

  // UNIFY
  int64_t span = sst.max_key - sst.min_key;
  int64_t pid = 0;
  int64_t np = sst.unify_num_partitions;
  if (span > 0) {
    double r = (double)(key - sst.min_key) / (double)span;
    pid = std::min<int64_t>((int64_t)(r * np), np - 1);
  }
  t = access_block_des(make_cache_key(sst.sst_id, 7),
                        (int)sst.unify_top_index_size, BT_UNIFY_TOP, sst, t, disk);
  t = access_block_des(make_cache_key(sst.sst_id, 8, (uint64_t)pid), part_size,
                        BT_UNIFY_PART, sst, t, disk);
  t += cfg_.unify_filter_compute_us;
  sst.filter_checked += 1;
  if (bloom_rejects()) {
    sst.filter_rejected += 1;
    return t;
  }
  t += cfg_.unify_index_compute_us;
  t = access_block_des(make_cache_key(sst.sst_id, 9, (uint64_t)(key % 10000)),
                        part_size, BT_DATA, sst, t, disk);
  return t;
}

void IOModel::record_phase(int scheme, Phase ph, bool had_miss, double phase_us) {
  PhaseStats& s = phase_stats_[scheme][ph];
  if (had_miss) {
    s.had_miss_count += 1;
    s.had_miss_total_us += phase_us;
  } else {
    s.all_hit_count += 1;
    s.all_hit_total_us += phase_us;
  }
}

double IOModel::point_lookup(SSTFile& sst, int64_t key, bool key_exists,
                              std::mt19937_64& rng) {
  sst.access_count += 1;
  sst.point_lookup_count += 1;
  std::uniform_real_distribution<double> U(0.0, 1.0);
  double cost = 0.0;
  int part_size = 4096;
  int scheme_idx = (int)sst.active_scheme;

  auto bloom_rejects = [&]() {
    if (key_exists) return false;
    return U(rng) > cfg_.bloom_false_positive_rate;
  };

  if (sst.active_scheme == SCHEME_FULL) {
    // --- Filter phase: 1 block + bloom compute ---
    uint64_t miss0 = total_io_count;
    double fp = 0.0;
    fp += access_block(make_cache_key(sst.sst_id, 1), (int)sst.full_filter_size,
                        BT_FULL_FILTER, sst);
    fp += cfg_.full_filter_compute_us;
    record_phase(scheme_idx, PH_FILTER, total_io_count > miss0, fp);
    cost += fp;

    sst.filter_checked += 1;
    if (bloom_rejects()) {
      sst.filter_rejected += 1;
      return cost;
    }

    // --- Index phase: 1 block + bsearch compute ---
    miss0 = total_io_count;
    double ip = 0.0;
    ip += access_block(make_cache_key(sst.sst_id, 2), (int)sst.full_index_size,
                        BT_FULL_INDEX, sst);
    ip += cfg_.full_index_compute_us;
    record_phase(scheme_idx, PH_INDEX, total_io_count > miss0, ip);
    cost += ip;

    cost += access_block(make_cache_key(sst.sst_id, 9, (uint64_t)(key % 10000)),
                         part_size, BT_DATA, sst);
    return cost;
  }

  if (sst.active_scheme == SCHEME_PARTITIONED) {
    int64_t span = sst.max_key - sst.min_key;
    int64_t fpid, ipid;
    if (span <= 0) {
      fpid = ipid = 0;
    } else {
      double ratio = (double)(key - sst.min_key) / (double)span;
      int64_t nf = sst.partitioned_num_filter_partitions;
      int64_t ni = sst.partitioned_num_index_partitions;
      fpid = std::min<int64_t>((int64_t)(ratio * nf), nf - 1);
      ipid = std::min<int64_t>((int64_t)(ratio * ni), ni - 1);
    }
    // --- Filter phase: top + filter_partition + compute ---
    uint64_t miss0 = total_io_count;
    double fp = 0.0;
    fp += access_block(make_cache_key(sst.sst_id, 3),
                        (int)sst.partitioned_filter_top_size,
                        BT_PART_FILTER_TOP, sst);
    fp += access_block(make_cache_key(sst.sst_id, 4, (uint64_t)fpid), part_size,
                        BT_PART_FILTER_LEAF, sst);
    fp += cfg_.partitioned_filter_compute_us;
    record_phase(scheme_idx, PH_FILTER, total_io_count > miss0, fp);
    cost += fp;

    sst.filter_checked += 1;
    if (bloom_rejects()) {
      sst.filter_rejected += 1;
      return cost;
    }
    // --- Index phase: top + index_partition + compute ---
    miss0 = total_io_count;
    double ip = 0.0;
    ip += access_block(make_cache_key(sst.sst_id, 5),
                        (int)sst.partitioned_index_top_size,
                        BT_PART_INDEX_TOP, sst);
    ip += access_block(make_cache_key(sst.sst_id, 6, (uint64_t)ipid), part_size,
                        BT_PART_INDEX_LEAF, sst);
    ip += cfg_.partitioned_index_compute_us;
    record_phase(scheme_idx, PH_INDEX, total_io_count > miss0, ip);
    cost += ip;

    cost += access_block(make_cache_key(sst.sst_id, 9, (uint64_t)(key % 10000)),
                         part_size, BT_DATA, sst);
    return cost;
  }

  // UNIFY: filter phase loads top + unify_partition (shared with index).
  // The index phase reuses those blocks (only compute), so index is always
  // ALL_HIT when filter was ALL_HIT (and its us is just compute).
  int64_t span = sst.max_key - sst.min_key;
  int64_t pid = 0;
  int64_t np = sst.unify_num_partitions;
  if (span > 0) {
    double ratio = (double)(key - sst.min_key) / (double)span;
    pid = std::min<int64_t>((int64_t)(ratio * np), np - 1);
  }
  uint64_t miss0 = total_io_count;
  double fp = 0.0;
  fp += access_block(make_cache_key(sst.sst_id, 7),
                      (int)sst.unify_top_index_size, BT_UNIFY_TOP, sst);
  fp += access_block(make_cache_key(sst.sst_id, 8, (uint64_t)pid), part_size,
                      BT_UNIFY_PART, sst);
  fp += cfg_.unify_filter_compute_us;
  record_phase(scheme_idx, PH_FILTER, total_io_count > miss0, fp);
  cost += fp;

  sst.filter_checked += 1;
  if (bloom_rejects()) {
    sst.filter_rejected += 1;
    return cost;
  }
  // Index phase in Unify = compute only (blocks reused from filter).
  double ip = cfg_.unify_index_compute_us;
  record_phase(scheme_idx, PH_INDEX, /*had_miss=*/false, ip);
  cost += ip;
  cost += access_block(make_cache_key(sst.sst_id, 9, (uint64_t)(key % 10000)),
                       part_size, BT_DATA, sst);
  return cost;
}

double IOModel::bandwidth_for_size(double avg_io_size_bytes) const {
  const auto& tbl = cfg_.disk_bandwidth_table;
  double eff = cfg_.disk_bandwidth_efficiency;
  if (tbl.empty()) return 15e9 * eff;
  if (avg_io_size_bytes <= tbl.front().first) return tbl.front().second * eff;
  if (avg_io_size_bytes >= tbl.back().first) return tbl.back().second * eff;
  for (size_t i = 0; i + 1 < tbl.size(); ++i) {
    auto lo = tbl[i];
    auto hi = tbl[i + 1];
    if (avg_io_size_bytes >= lo.first && avg_io_size_bytes <= hi.first) {
      double frac =
          (avg_io_size_bytes - lo.first) / (double)(hi.first - lo.first);
      return (lo.second + frac * (hi.second - lo.second)) * eff;
    }
  }
  return 15e9 * eff;
}

double IOModel::effective_us_per_op(double base_us_per_op, int64_t op_count,
                                     int num_threads) const {
  if (op_count <= 0 || num_threads <= 1) return base_us_per_op;
  if (total_io_count <= 0 || total_miss_bytes <= 0) return base_us_per_op;
  double avg_io_size = (double)total_miss_bytes / (double)total_io_count;
  double bw = bandwidth_for_size(avg_io_size);
  if (bw <= 0) return base_us_per_op;
  double demand_per_op = (double)total_miss_bytes / (double)op_count;
  double base_ops = 1'000'000.0 / base_us_per_op;
  double aggregate = num_threads * base_ops * demand_per_op;
  if (aggregate <= bw) return base_us_per_op;
  double saturated = num_threads * demand_per_op / bw * 1'000'000.0;
  return std::max(base_us_per_op, saturated);
}

// ---------------- LSMTree ----------------

LSMTree::LSMTree(const LSMConfig& cfg, std::vector<SSTFile> ssts)
    : cfg_(cfg), ssts_(std::move(ssts)) {
  levels_.assign(cfg.num_levels, {});
  for (int i = 0; i < (int)ssts_.size(); ++i) {
    int lv = ssts_[i].level;
    if (lv >= 0 && lv < cfg.num_levels) levels_[lv].push_back(i);
  }
  for (int lv = 1; lv < cfg.num_levels; ++lv) {
    std::sort(levels_[lv].begin(), levels_[lv].end(),
              [&](int a, int b) { return ssts_[a].min_key < ssts_[b].min_key; });
  }
  compute_metadata_sizes();
}

void LSMTree::compute_metadata_sizes() {
  int kvs = cfg_.key_size + cfg_.value_size;
  int keys_per_block = std::max(1, cfg_.block_size / kvs);
  for (auto& s : ssts_) {
    int64_t num_blocks =
        std::max<int64_t>(1, (s.num_keys + keys_per_block - 1) / keys_per_block);
    int64_t raw_index = num_blocks * (cfg_.key_size + 8);
    s.full_index_size =
        std::max<int64_t>(1, (int64_t)(raw_index * cfg_.index_shortening_factor));
    s.full_filter_size =
        std::max<int64_t>(1, s.num_keys * cfg_.bloom_bits_per_key / 8);

    int64_t part = cfg_.metadata_block_size;
    s.partitioned_num_filter_partitions =
        std::max<int64_t>(1, (s.full_filter_size + part - 1) / part);
    s.partitioned_num_index_partitions =
        std::max<int64_t>(1, (s.full_index_size + part - 1) / part);
    // FilterPartIdx and Index top are separate blocks (each holds entries for
    // its own partition set). Each entry ≈ key_size + 8 (BlockHandle).
    s.partitioned_filter_top_size =
        s.partitioned_num_filter_partitions * (cfg_.key_size + 8);
    s.partitioned_index_top_size =
        s.partitioned_num_index_partitions * (cfg_.key_size + 8);

    int64_t total_meta = s.full_index_size + s.full_filter_size;
    s.unify_num_partitions =
        std::max<int64_t>(1, (total_meta + part - 1) / part);
    s.unify_top_index_size = s.unify_num_partitions * (cfg_.key_size + 8);
  }
}

void LSMTree::get_ssts_for_key(int64_t key, std::vector<SSTFile*>& out) {
  out.clear();
  for (int idx : levels_[0]) {
    if (ssts_[idx].contains(key)) out.push_back(&ssts_[idx]);
  }
  for (int lv = 1; lv < cfg_.num_levels; ++lv) {
    const auto& lst = levels_[lv];
    if (lst.empty()) continue;
    int lo = 0, hi = (int)lst.size() - 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      SSTFile& s = ssts_[lst[mid]];
      if (key < s.min_key)
        hi = mid - 1;
      else if (key > s.max_key)
        lo = mid + 1;
      else {
        out.push_back(&s);
        break;
      }
    }
  }
}

}  // namespace hymeta
