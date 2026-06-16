#include "workload.hpp"

#include <algorithm>
#include <cmath>

namespace hymeta {

WorkloadGen::WorkloadGen(const WorkloadConfig& cfg, int64_t max_key)
    : cfg_(cfg),
      max_key_(max_key),
      num_items_(max_key + 1),
      rng_key_(cfg.seed),
      rng_exist_(cfg.seed ^ 0x9E3779B97F4A7C15ULL),
      rng_op_(cfg.seed ^ 0xBF58476D1CE4E5B9ULL),
      U_(0.0, 1.0),
      Uk_(0, max_key) {
  if (cfg.distribution == "mixgraph") {
    mode_ = MODE_MIXGRAPH;
    init_mixgraph(num_items_);
  } else if (cfg.distribution == "zipfian" && cfg.zipfian_constant >= 1e-6) {
    mode_ = MODE_ZIPFIAN;
    init_zipfian(num_items_, cfg.zipfian_constant);
  } else {
    mode_ = MODE_UNIFORM;
  }
}

// ---------------- Zipfian ----------------

double WorkloadGen::compute_zeta_approx(int64_t n, double theta) {
  // H_{n,theta} ≈ n^(1-theta) / (1-theta) + 0.5 for theta in (0,1), theta != 1.
  if (std::abs(theta - 1.0) < 1e-9) return std::log((double)n) + 0.5772156649;
  return std::pow((double)n, 1.0 - theta) / (1.0 - theta) + 0.5;
}

void WorkloadGen::init_zipfian(int64_t num_items, double theta) {
  theta_ = theta;
  if (num_items <= 1'000'000) {
    double z = 0.0;
    for (int64_t i = 1; i <= num_items; ++i) z += 1.0 / std::pow((double)i, theta);
    zeta_n_ = z;
  } else {
    zeta_n_ = compute_zeta_approx(num_items, theta);
  }
  zeta_2_ = 1.0 + 1.0 / std::pow(2.0, theta);
  alpha_ = 1.0 / (1.0 - theta);
  eta_ = (1.0 - std::pow(2.0 / (double)num_items, 1.0 - theta)) /
         (1.0 - zeta_2_ / zeta_n_);
}

int64_t WorkloadGen::zipfian_next() {
  double u = U_(rng_key_);
  double uz = u * zeta_n_;
  if (uz < 1.0) return 0;
  if (uz < 1.0 + std::pow(0.5, theta_)) return 1;
  return (int64_t)((double)num_items_ *
                    std::pow(eta_ * u - eta_ + 1.0, alpha_));
}

// ---------------- Mixgraph ----------------
// Direct port of RocksDB db_bench_tool.cc:
//   PowerCdfInversion / ParetoCdfInversion / GenerateTwoTermExpKeys /
//   QueryDecider (Get + Seek only — we fold Put into Get since the sim
//   does not model writes).

int64_t WorkloadGen::power_cdf_inversion(double u, double a, double b) {
  if (a == 0.0 || b == 0.0) return 0;
  return (int64_t)std::ceil(std::pow(u / a, 1.0 / b));
}

int64_t WorkloadGen::pareto_cdf_inversion(double u, double theta, double k,
                                           double sigma) {
  double ret = (k == 0.0)
                   ? (theta - sigma * std::log(u))
                   : (theta + sigma * (std::pow(u, -k) - 1.0) / k);
  return (int64_t)std::ceil(ret);
}

void WorkloadGen::init_mixgraph(int64_t num_items) {
  // ---- Build prefix (keyrange) hotness list ----
  int64_t krn = std::max<int64_t>(1, cfg_.mix_keyrange_num);
  mix_keyrange_size_ = std::max<int64_t>(1, num_items / krn);

  int64_t amplify = 0;
  int64_t keyrange_start = 0;
  mix_keyranges_.clear();
  for (int64_t pfx = krn; pfx >= 1; --pfx) {
    // Two-term exponential probability for this keyrange.
    double p = cfg_.mix_keyrange_a * std::exp(cfg_.mix_keyrange_b * (double)pfx)
             + cfg_.mix_keyrange_c * std::exp(cfg_.mix_keyrange_d * (double)pfx);
    if (p < 1e-16) p = 0.0;
    // Amplify by 1/min_p so that all integer ranges are non-negative.
    if (amplify == 0 && p > 0.0) {
      amplify = (int64_t)std::floor(1.0 / p) + 1;
    }
    KeyrangeUnit u;
    u.keyrange_start = keyrange_start;
    u.keyrange_access = (p <= 0.0) ? 0 : (int64_t)std::floor(amplify * p);
    u.keyrange_keys = mix_keyrange_size_;
    mix_keyranges_.push_back(u);
    keyrange_start += u.keyrange_access;
  }
  mix_rand_max_ = std::max<int64_t>(1, keyrange_start);

  // Shuffle so hot ranges are not concentrated at one end of the key space.
  // RocksDB seeds with mix_rand_max_ for reproducibility.
  std::mt19937_64 shuffle_rng((uint64_t)mix_rand_max_);
  for (int64_t i = 0; i < krn; ++i) {
    int64_t pos = (int64_t)(shuffle_rng() % (uint64_t)krn);
    std::swap(mix_keyranges_[(size_t)i], mix_keyranges_[(size_t)pos]);
  }
  // Recalculate start offsets after shuffle.
  int64_t offset = 0;
  for (auto& u : mix_keyranges_) {
    u.keyrange_start = offset;
    offset += u.keyrange_access;
  }

  // ---- Build query type prefix-sum (get vs seek; put folded into get) ----
  double sum = cfg_.mix_get_ratio + cfg_.mix_seek_ratio;
  if (sum <= 0) {
    // Degenerate: all gets.
    mix_type_thresholds_ = {mix_type_range_};
    return;
  }
  double get_norm = cfg_.mix_get_ratio / sum;
  double seek_norm = cfg_.mix_seek_ratio / sum;
  int get_thresh = (int)std::ceil(mix_type_range_ * get_norm);
  int seek_thresh = get_thresh + (int)std::ceil(mix_type_range_ * seek_norm);
  mix_type_thresholds_ = {get_thresh, seek_thresh};
  mix_type_range_ = seek_thresh;
}

int64_t WorkloadGen::mixgraph_next_key(uint64_t ini_rand) {
  int64_t keyrange_rand = (int64_t)(ini_rand % (uint64_t)mix_rand_max_);

  // Binary search for which keyrange this rand falls into.
  int64_t lo = 0, hi = (int64_t)mix_keyranges_.size();
  while (lo + 1 < hi) {
    int64_t mid = lo + (hi - lo) / 2;
    if (keyrange_rand < mix_keyranges_[(size_t)mid].keyrange_start)
      hi = mid;
    else
      lo = mid;
  }
  int64_t kr_id = lo;

  // Within the keyrange, pick an offset via power-law CDF inversion.
  int64_t key_offset;
  if (cfg_.mix_key_a == 0.0 || cfg_.mix_key_b == 0.0) {
    key_offset = (int64_t)(ini_rand % (uint64_t)mix_keyrange_size_);
  } else {
    double u = (double)(ini_rand % (uint64_t)mix_keyrange_size_) /
               (double)mix_keyrange_size_;
    int64_t key_seed = power_cdf_inversion(u, cfg_.mix_key_a, cfg_.mix_key_b);
    std::mt19937_64 rand_key((uint64_t)key_seed);
    key_offset = (int64_t)(rand_key() % (uint64_t)mix_keyrange_size_);
  }
  int64_t key = mix_keyrange_size_ * kr_id + key_offset;
  if (key < 0) key = 0;
  if (key >= num_items_) key = num_items_ - 1;
  return key;
}

// ---------------- Dispatch ----------------

int64_t WorkloadGen::next_key() {
  if (mode_ == MODE_UNIFORM) {
    return Uk_(rng_key_);
  }
  if (mode_ == MODE_ZIPFIAN) {
    int64_t raw = zipfian_next();
    if (raw < 0) raw = 0;
    if (raw >= num_items_) raw = num_items_ - 1;
    // Scrambled: FNV-hash to break cluster.
    uint64_t h = fnv_hash((uint64_t)raw) % (uint64_t)num_items_;
    return (int64_t)h;
  }
  // MIXGRAPH
  uint64_t ini_rand = rng_key_();
  return mixgraph_next_key(ini_rand);
}

OpType WorkloadGen::next_op_type() {
  if (mode_ != MODE_MIXGRAPH) return OP_GET;
  // RocksDB-style: pick rand in [0, mix_type_range_) and find first type
  // whose prefix-sum threshold exceeds it. Index 0 = get, 1 = seek.
  int pos = (int)(rng_op_() % (uint64_t)std::max(1, mix_type_range_));
  for (size_t i = 0; i < mix_type_thresholds_.size(); ++i) {
    if (pos < mix_type_thresholds_[i]) {
      return (i == 0) ? OP_GET : OP_SEEK;
    }
  }
  return OP_GET;
}

}  // namespace hymeta
