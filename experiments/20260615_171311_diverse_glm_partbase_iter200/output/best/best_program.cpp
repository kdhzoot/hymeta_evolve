#include "sim_engine.hpp"
namespace hymeta {

// EVOLVE-BLOCK-START
// Level-aware adaptive scheme selection:
// - Top levels (0-2): small, frequently-accessed SSTs → SCHEME_FULL
//   improves cache hit by co-locating index+data metadata
// - Middle levels (3-4): SCHEME_UNIFY for moderate size,
//   balances metadata overhead vs cache footprint
// - Bottom levels (5+): large SSTs → SCHEME_PARTITIONED
//   reduces cache pressure from bloated unified blocks
Scheme select_scheme(const SSTStats& s) {
  if (s.level <= 2) return SCHEME_FULL;
  if (s.level <= 4) return SCHEME_UNIFY;
  return SCHEME_PARTITIONED;
}
// EVOLVE-BLOCK-END

}
