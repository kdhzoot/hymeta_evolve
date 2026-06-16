#include "sim_engine.hpp"
namespace hymeta {

Scheme select_scheme(const SSTStats& s) {
  if (s.access_count == 0) {
    if (s.level <= 3) return SCHEME_FULL;
    return SCHEME_UNIFY;
  }

  const double scan_ratio = (double)s.scan_count / (double)s.access_count;
  const bool small_sst = s.num_keys < 30000 || s.full_metadata_size < (2LL << 20);
  const bool hot_and_filtered = s.cache_hit_rate >= 0.74 && s.filter_rejection_rate >= 0.28;
  // Cache-dominant: high hit rate even with low filter rejection (mixgraph pattern)
  const bool cache_dominant = s.cache_hit_rate >= 0.82 && s.level <= 3;

  // Very cold or deep: Unify saves metadata budget
  if (s.cache_hit_rate < 0.55) return SCHEME_UNIFY;
  if (s.level >= 4 && !cache_dominant) return SCHEME_UNIFY;

  // Scan-heavy SSTs benefit from partitioned index blocks
  // Guard: only promote if cache hit is moderate (hot SSTs need Full, not Partitioned)
  if (scan_ratio > 0.18 && s.level >= 2 && s.cache_hit_rate < 0.72) return SCHEME_PARTITIONED;

  // Hot SSTs: Full for best point-lookup latency
  if ((small_sst && s.cache_hit_rate >= 0.65) || hot_and_filtered || cache_dominant)
    return SCHEME_FULL;

  // Good filter + cache at shallow levels
  if (s.filter_rejection_rate >= 0.34 && s.cache_hit_rate >= 0.62 && s.level <= 2)
    return SCHEME_FULL;

  // Warm shallow SSTs: Full critical for mixgraph high-hit tiers
  if (s.level <= 2 && s.cache_hit_rate >= 0.68) return SCHEME_FULL;

  // Level 3 warm SSTs with decent hit rate
  if (s.level == 3 && s.cache_hit_rate >= 0.72) return SCHEME_FULL;

  // Low access count: prefer stable Unify to avoid transitions
  if (s.access_count < 5) return SCHEME_UNIFY;

  // Default to Unify to minimize transitions for ambiguous SSTs
  return SCHEME_UNIFY;
}
// EVOLVE-BLOCK-END

}
