#include "sim_engine.hpp"
namespace hymeta {

// EVOLVE-BLOCK-START
// Workload-adaptive hybrid policy:
// - Small hot SSTs → FULL: eliminates partition indirection overhead,
//   small enough that full metadata fits in cache
// - Scan-heavy SSTs → UNIFY: unified metadata reduces scan cache misses
// - Large cold SSTs → PARTITIONED: saves memory for rarely-accessed data
//
// Available SSTStats fields:
//   s.level, s.num_keys, s.access_count, s.point_lookup_count, s.scan_count
//   s.filter_rejection_rate, s.cache_hit_rate, s.full_metadata_size
//   s.partitioned_num_partitions, s.unify_num_partitions
Scheme select_scheme(const SSTStats& s) {
  // Small hot SSTs → FULL: no partition indirection, fits in cache
  if (s.level <= 1 && s.num_keys < 50000) {
    return SCHEME_FULL;
  }

  // Cache-rich SSTs → PARTITIONED: already well-cached, UNIFY wastes capacity
  if (s.cache_hit_rate > 0.93) {
    return SCHEME_PARTITIONED;
  }

  // High filter rejection + decent caching → PARTITIONED: lookups skip
  // partitions early, so partitioned saves memory without slowdown
  if (s.filter_rejection_rate > 0.5 && s.cache_hit_rate > 0.82) {
    return SCHEME_PARTITIONED;
  }

  // Hot SSTs in 0.85-0.93 cache range → UNIFY: unified layout saves
  // per-lookup cache misses for frequently-accessed data, avoids
  // scheme thrashing that PARTITIONED causes for zipfian-hot SSTs
  if (s.access_count > 100 && s.cache_hit_rate > 0.85) {
    return SCHEME_UNIFY;
  }

  double scan_ratio = s.scan_count / (s.access_count + 1.0);

  // Scan-heavy → UNIFY: unified index avoids partition-chasing
  if (scan_ratio > 0.10) {
    return SCHEME_UNIFY;
  }

  // Cache pressure with moderate access → UNIFY: fewer cache lines → fewer misses
  if (s.cache_hit_rate < 0.70 && s.access_count > 60) {
    return SCHEME_UNIFY;
  }

  // Cold SSTs → UNIFY: unified layout fewer misses
  if (s.cache_hit_rate < 0.55) {
    return SCHEME_UNIFY;
  }

  // Default: PARTITIONED for large/cold/well-cached SSTs
  return SCHEME_PARTITIONED;
}
// EVOLVE-BLOCK-END

}
