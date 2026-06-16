#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "sim_engine.hpp"

namespace hymeta {

// Per-op query type emitted by WorkloadGen. Mixgraph distinguishes Get vs
// Seek; uniform/zipfian always emit Get.
enum OpType : int {
  OP_GET = 0,
  OP_SEEK = 1,
};

// Scrambled Zipfian + uniform + mixgraph key generator. Mixgraph ports
// RocksDB db_bench's two-term-exponential prefix hotness + power-law within
// keyrange, plus get/seek query mix.
class WorkloadGen {
 public:
  WorkloadGen(const WorkloadConfig& cfg, int64_t max_key);

  // Returns next key.
  int64_t next_key();

  // Returns next query type. For uniform/zipfian always OP_GET. For
  // mixgraph dispatches by configured ratios.
  OpType next_op_type();

  // Pre-roll: decide if this key "exists" (fillrandom ~63% unique coverage).
  bool next_key_exists() {
    return U_(rng_exist_) < cfg_.found_rate;
  }

  int64_t num_ops() const { return cfg_.num_operations; }

 private:
  // Zipfian state (only used when constant >= 1e-6 and dist=="zipfian").
  void init_zipfian(int64_t num_items, double theta);
  int64_t zipfian_next();
  static double compute_zeta_approx(int64_t n, double theta);

  // Mixgraph state.
  void init_mixgraph(int64_t num_items);
  int64_t mixgraph_next_key(uint64_t ini_rand);
  static int64_t pareto_cdf_inversion(double u, double theta, double k,
                                       double sigma);
  static int64_t power_cdf_inversion(double u, double a, double b);

  static uint64_t fnv_hash(uint64_t x) {
    uint64_t h = 0xcbf29ce484222325ULL;
    h ^= (x & 0xFFFFFFFFULL);
    h *= 0x100000001b3ULL;
    h ^= (x >> 32) & 0xFFFFFFFFULL;
    h *= 0x100000001b3ULL;
    return h;
  }

  WorkloadConfig cfg_;
  int64_t max_key_;
  int64_t num_items_;  // max_key + 1
  enum Mode { MODE_UNIFORM, MODE_ZIPFIAN, MODE_MIXGRAPH } mode_;

  // Zipfian math.
  double theta_ = 0.0;
  double zeta_n_ = 0.0;
  double zeta_2_ = 0.0;
  double alpha_ = 0.0;
  double eta_ = 0.0;

  // Mixgraph keyrange list: each unit covers an integer range proportional
  // to its access probability. A random value in [0, mix_rand_max_) selects
  // a keyrange via binary search.
  struct KeyrangeUnit {
    int64_t keyrange_start;
    int64_t keyrange_access;
    int64_t keyrange_keys;
  };
  std::vector<KeyrangeUnit> mix_keyranges_;
  int64_t mix_rand_max_ = 0;
  int64_t mix_keyrange_size_ = 0;

  // Mixgraph query mix: prefix-sum thresholds over [0, 1000).
  // type_[i] is the exclusive upper bound for type i.
  std::vector<int> mix_type_thresholds_;
  int mix_type_range_ = 1000;

  std::mt19937_64 rng_key_;
  std::mt19937_64 rng_exist_;
  std::mt19937_64 rng_op_;
  std::uniform_real_distribution<double> U_;
  std::uniform_int_distribution<int64_t> Uk_;
};

}  // namespace hymeta
