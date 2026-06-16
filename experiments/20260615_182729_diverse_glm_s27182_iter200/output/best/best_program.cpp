#include "sim_engine.hpp"
namespace hymeta {

// EVOLVE-BLOCK-START
// FULL for hot/cached SSTs; UNIFY for medium; PARTITIONED for cold/large.
// Key: partition load overhead dominates. FULL avoids it when metadata fits.
Scheme select_scheme(const SSTStats& s) {
  if (s.level <= 2) return SCHEME_FULL;
  double hot = double(s.access_count) / (s.num_keys + 1.0);
  bool mf = s.full_metadata_size < 400000;
  if (s.cache_hit_rate > 0.55 && hot > 0.8 && mf) return SCHEME_FULL;
  if (s.cache_hit_rate > 0.85 && hot > 0.05 && mf) return SCHEME_FULL;
  if (s.access_count > 150000 && mf && s.num_keys < 500000) return SCHEME_FULL;
  if (s.filter_rejection_rate < 0.3 && s.point_lookup_count > s.scan_count && mf && s.num_keys < 500000) return SCHEME_FULL;
  if (s.scan_count > s.point_lookup_count * 2 && s.num_keys > 200000) return SCHEME_PARTITIONED;
  if (s.filter_rejection_rate > 0.5) return SCHEME_PARTITIONED;
  if (s.unify_num_partitions > 0 && s.num_keys < 800000) return SCHEME_UNIFY;
  if (s.unify_num_partitions > 0 && hot > 0.3) return SCHEME_UNIFY;
  return SCHEME_PARTITIONED;
}
// EVOLVE-BLOCK-END

}
