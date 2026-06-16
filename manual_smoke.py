"""
Manual smoke test for the evolution pipeline (no LLM needed).

Drops 5 hand-crafted select_scheme() bodies into initial_program.cpp, runs the
full evaluator on each, and prints a ranking. This exercises the complete
build+run+score path that OpenEvolve will drive.
"""
import json
import os
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).parent.resolve()
import sys
sys.path.insert(0, str(HERE))
from evaluator import evaluate  # noqa: E402


CANDIDATES = {
    "FullOnly":  "return SCHEME_FULL;",
    "PartOnly":  "return SCHEME_PARTITIONED;",
    "UnifyOnly": "return SCHEME_UNIFY;",
    "L0-L2_Full_else_Unify": """\
if (s.level <= 2) return SCHEME_FULL;
return SCHEME_UNIFY;""",
    "L0-L2_Full_L3_Part_else_Unify": """\
if (s.level <= 2) return SCHEME_FULL;
if (s.level == 3) return SCHEME_PARTITIONED;
return SCHEME_UNIFY;""",
    "HyMeta_hybrid_seed": """\
if (s.level <= 2) return SCHEME_FULL;
double pr = s.access_count == 0 ? 1.0
              : (double)s.point_lookup_count / (double)s.access_count;
if (s.access_count > 100 && s.cache_hit_rate > 0.8) return SCHEME_FULL;
if (pr > 0.8) return SCHEME_UNIFY;
if (pr < 0.3) return SCHEME_PARTITIONED;
return SCHEME_UNIFY;""",
}


PROG_TEMPLATE = """\
#include "sim_engine.hpp"
namespace hymeta {{
// EVOLVE-BLOCK-START
Scheme select_scheme(const SSTStats& s) {{
  (void)s;
  {body}
}}
// EVOLVE-BLOCK-END
}}  // namespace hymeta
"""


def write_policy(body: str):
    (HERE / "initial_program.cpp").write_text(
        PROG_TEMPLATE.format(body=body.replace("\n", "\n  "))
    )


def main():
    # Save original for restore.
    orig = (HERE / "initial_program.cpp").read_text()

    results = []
    for name, body in CANDIDATES.items():
        write_policy(body)
        out = evaluate(str(HERE / "initial_program.cpp"))
        results.append((name, out))

    # Restore original.
    (HERE / "initial_program.cpp").write_text(orig)

    # Print ranking (higher fitness = lower us/op).
    results.sort(key=lambda kv: -kv[1]["fitness"])
    print(f"\n{'rank':>4} {'policy':<32} {'avg_us/op':>10}  per-scenario (pct%/N_us)")
    print("-" * 110)
    for i, (name, out) in enumerate(results, start=1):
        scens = " ".join(
            f"{s['pct']:.1f}%/{s['threads']}={s['effective_us_per_op']:.1f}"
            for s in out.get("per_scenario", [])
        )
        print(f"{i:>4} {name:<32} {out['avg_us_per_op']:>9.2f}  {scens}")

    with open(HERE / "manual_smoke_results.json", "w") as f:
        json.dump(results, f, indent=2, default=str)


if __name__ == "__main__":
    main()
