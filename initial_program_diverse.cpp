#include "sim_engine.hpp"
namespace hymeta {

// EVOLVE-BLOCK-START
// Diversity seed: start from the evaluator baseline, all-Partitioned.
//
// OpenEvolve should discover improvements from a neutral 1.0 baseline rather
// than inherit a hand-written hybrid policy. Useful directions include static
// level-family policies, cache-pressure policies, metadata-budget policies,
// workload-aware policies, and hot-small-SST promotion.
//
// Available SSTStats fields:
//   s.level, s.num_keys, s.access_count, s.point_lookup_count, s.scan_count
//   s.filter_rejection_rate, s.cache_hit_rate, s.full_metadata_size
//   s.partitioned_num_partitions, s.unify_num_partitions
Scheme select_scheme(const SSTStats&) {
  return SCHEME_PARTITIONED;
}
// EVOLVE-BLOCK-END

}
