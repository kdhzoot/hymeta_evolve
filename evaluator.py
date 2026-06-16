"""
OpenEvolve evaluator for HyMeta metadata-scheme policy.

Protocol (per OpenEvolve docs):
  evaluate(program_path: str) -> dict
    program_path: absolute path to the evolved initial_program.cpp (our
                  EVOLVE-BLOCK target).
    returns: dict with a numeric "fitness" to MAXIMIZE plus auxiliary metrics.

Fitness:
  speedup_i = baseline_us_i / candidate_us_i        (per scenario)
  fitness   = geomean_i(speedup_i)                  (weighted geometric mean)

  Baseline = all-Partitioned policy, computed once on first call and cached
  to baseline_cache.json (keyed by scenarios + num_ops + found_rate + seed).
  fitness = 1.0 → equal to baseline, 1.2 → 20% faster avg, 2.0 → 2× faster.

Pipeline per candidate:
  1. Ensure baseline cache (lazy compute on first call).
  2. Copy program_path into initial_program.cpp.
  3. Rebuild sim_main (incremental).
  4. Run sim_main across SCENARIOS (currently 4, all 16T).
  5. Compute speedup vs baseline, aggregate as weighted geomean.

Robustness:
  - Timeout per sim run: 60 s.
  - Build / runtime / timeout failures → fitness = 0.0 (worst).
"""
import hashlib
import json
import math
import os
import shutil
import subprocess
import time
from pathlib import Path

from openevolve.evaluation_result import EvaluationResult

HERE = Path(__file__).parent.resolve()
BUILD_DIR = HERE / "build"
SIM_BIN = BUILD_DIR / "sim_main"
LAYOUT = "/home/godong/hymeta_evolve/bench_results/sst_layout.json"
INIT_PROG = HERE / "initial_program.cpp"
BASELINE_CACHE = HERE / "baseline_cache.json"

DB_SIZE_BYTES = 250 * 1024 ** 3
# Sweet spot for real-DB accuracy (empirically determined on 2% 16T):
#   1M ops:  sim +30% (cache still warming)
#   3M ops:  sim +3%  ← matches real best
#   5M ops:  sim -5%  (sim over-warms slightly)
#   10M ops: sim -12% (too optimistic)
# At 3M ops each scenario takes ~1s → 4 scenarios ~4s, good for OpenEvolve.
NUM_OPS = 3_000_000
FOUND_RATE = 0.63
SEED = 42
# Dynamic policy: 0 = static (select_scheme called once with stats=0).
# >0 = call select_scheme every N ops with live cumulative stats. SSTs whose
# new scheme differs flip immediately; old-scheme cache entries age out via
# CLOCK (different schemes use different cache key tags, so no explicit
# invalidation needed).
# Default = 20 epochs over the 3M-op workload — fine-grained dynamic
# adaptation. Policy re-decides scheme per SST 20 times during evaluation
# based on accumulated stats (cache_hit_rate, filter_rejection_rate, etc.).
REAPPLY_EVERY = 150_000

# Evaluation scenarios: 5 cache sizes × 3 distributions = 15 scenarios,
# all 16T. Cache % spans the full operating range:
#   2.0%  — easy regime (most metadata fits)
#   1.0%  — saturation starts
#   0.5%  — saturation
#   0.25% — extreme saturation (Full-heavy thrashes)
#   0.1%  — pathological saturation (only Unify viable for most SSTs)
# Distribution mix exposes the policy to uniform (no hot SSTs),
# zipfian (heavy hotspot), and mixgraph (RocksDB FB-trace with 17% seek
# mix so policy can use scan_count signal).
# (cache_pct, num_threads, distribution, weight)
SCENARIOS = [
    (2.0,  16, "uniform",  1.0),
    (1.0,  16, "uniform",  1.0),
    (0.5,  16, "uniform",  1.0),
    (0.25, 16, "uniform",  1.0),
    (0.1,  16, "uniform",  1.0),
    (2.0,  16, "zipfian",  1.0),
    (1.0,  16, "zipfian",  1.0),
    (0.5,  16, "zipfian",  1.0),
    (0.25, 16, "zipfian",  1.0),
    (0.1,  16, "zipfian",  1.0),
    (2.0,  16, "mixgraph", 1.0),
    (1.0,  16, "mixgraph", 1.0),
    (0.5,  16, "mixgraph", 1.0),
    (0.25, 16, "mixgraph", 1.0),
    (0.1,  16, "mixgraph", 1.0),
]

# Baseline policy source — all-Partitioned at every level.
BASELINE_POLICY_SRC = """\
#include "sim_engine.hpp"
namespace hymeta {
Scheme select_scheme(const SSTStats&) { return SCHEME_PARTITIONED; }
}
"""

FAIL_FITNESS = 0.0  # geomean speedup; 0 = "infinitely slow vs baseline"


def _ensure_build_configured():
    if not (BUILD_DIR / "CMakeCache.txt").exists():
        BUILD_DIR.mkdir(exist_ok=True)
        subprocess.run(
            ["cmake", "-DCMAKE_BUILD_TYPE=Release", ".."],
            cwd=str(BUILD_DIR),
            check=True,
            capture_output=True,
        )


def _rebuild(timeout_s: float = 60.0) -> bool:
    try:
        subprocess.run(
            ["cmake", "--build", str(BUILD_DIR), "-j4", "--target", "sim_main"],
            check=True,
            capture_output=True,
            text=True,
            timeout=timeout_s,
        )
        return True
    except subprocess.CalledProcessError as e:
        err_path = HERE / "last_build_error.log"
        err_path.write_text((e.stdout or "") + "\n---STDERR---\n" + (e.stderr or ""))
        return False
    except subprocess.TimeoutExpired:
        return False


def _run_one(cache_bytes: int, num_threads: int, dist: str = "uniform",
             timeout_s: float = 60.0) -> dict:
    try:
        r = subprocess.run(
            [
                str(SIM_BIN),
                f"--layout={LAYOUT}",
                f"--cache-bytes={cache_bytes}",
                f"--num-threads={num_threads}",
                f"--num-ops={NUM_OPS}",
                f"--found-rate={FOUND_RATE}",
                f"--seed={SEED}",
                f"--dist={dist}",
                "--des-mode=0",
                f"--reapply-every={REAPPLY_EVERY}",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=timeout_s,
        )
        return json.loads(r.stdout.strip())
    except Exception as e:
        return {"error": repr(e)}


def _baseline_key() -> str:
    """Stable key over SCENARIOS + NUM_OPS + FOUND_RATE + SEED + policy src."""
    payload = json.dumps({
        "scenarios": SCENARIOS,
        "num_ops": NUM_OPS,
        "found_rate": FOUND_RATE,
        "seed": SEED,
        "reapply_every": REAPPLY_EVERY,
        "policy": BASELINE_POLICY_SRC,
    }, sort_keys=True).encode()
    return hashlib.sha1(payload).hexdigest()[:12]


def _ensure_baseline() -> dict:
    """Return {scenario_idx → baseline_us}. Compute + cache on first call."""
    key = _baseline_key()
    if BASELINE_CACHE.exists():
        try:
            cache = json.loads(BASELINE_CACHE.read_text())
            if cache.get("key") == key:
                return {int(k): float(v) for k, v in cache["per_scenario_us"].items()}
        except Exception:
            pass  # corrupt cache → recompute

    print(f"[evaluator] Computing baseline (all-Partitioned, key={key})...")
    saved = INIT_PROG.read_text()
    try:
        INIT_PROG.write_text(BASELINE_POLICY_SRC)
        _ensure_build_configured()
        if not _rebuild():
            raise RuntimeError("baseline build failed")
        per_scenario_us = {}
        for i, (pct, nt, dist, _w) in enumerate(SCENARIOS):
            cache_bytes = int(DB_SIZE_BYTES * pct / 100)
            out = _run_one(cache_bytes, nt, dist)
            if "error" in out:
                raise RuntimeError(f"baseline scenario {i} failed: {out['error']}")
            per_scenario_us[i] = out["effective_us_per_op"]
            print(f"  scenario {i} ({pct}% {nt}T {dist}): "
                  f"{per_scenario_us[i]:.2f} us/op")
    finally:
        # Restore caller's initial_program.cpp so next build uses it.
        INIT_PROG.write_text(saved)

    BASELINE_CACHE.write_text(json.dumps({
        "key": key,
        "scenarios": SCENARIOS,
        "num_ops": NUM_OPS,
        "found_rate": FOUND_RATE,
        "seed": SEED,
        "reapply_every": REAPPLY_EVERY,
        "per_scenario_us": {str(k): v for k, v in per_scenario_us.items()},
    }, indent=2))
    return per_scenario_us


def evaluate(program_path: str) -> dict:
    program_path = str(Path(program_path).resolve())
    target = INIT_PROG

    # Compute baseline once (cached to disk). Done BEFORE we overwrite
    # initial_program.cpp with the candidate.
    baseline_us = _ensure_baseline()

    if program_path != str(target):
        shutil.copyfile(program_path, target)

    _ensure_build_configured()
    t_build_start = time.time()
    if not _rebuild():
        # Surface the compiler error to the LLM via artifacts channel so
        # subsequent iterations can self-correct typos / missing includes.
        build_err = ""
        err_log = HERE / "last_build_error.log"
        if err_log.exists():
            build_err = err_log.read_text()[-2000:]  # tail only
        return EvaluationResult(
            metrics={
                "fitness": FAIL_FITNESS,
                "combined_score": FAIL_FITNESS,
                "build_s": time.time() - t_build_start,
            },
            artifacts={
                "build_error": build_err,
                "hint": ("Valid Scheme enum values are exactly: SCHEME_FULL, "
                         "SCHEME_PARTITIONED, SCHEME_UNIFY. Other names like "
                         "SCHEME_UNIFIED do NOT exist."),
            },
        )
    build_s = time.time() - t_build_start

    per_scenario = []
    runtime_s = 0.0
    log_speedup_sum = 0.0
    total_weight = 0.0
    for i, (pct, nt, dist, w) in enumerate(SCENARIOS):
        cache_bytes = int(DB_SIZE_BYTES * pct / 100)
        t0 = time.time()
        out = _run_one(cache_bytes, nt, dist)
        runtime_s += time.time() - t0
        if "error" in out:
            return {
                "fitness": FAIL_FITNESS,
                "combined_score": FAIL_FITNESS,
                "error": out["error"],
                "build_s": build_s,
                "runtime_s": runtime_s,
            }
        cand_us = out["effective_us_per_op"]
        base_us = baseline_us[i]
        speedup = base_us / cand_us if cand_us > 0 else 0.0
        if speedup <= 0:
            return {
                "fitness": FAIL_FITNESS,
                "combined_score": FAIL_FITNESS,
                "error": f"non-positive us/op in scenario {i}",
                "build_s": build_s,
                "runtime_s": runtime_s,
            }
        log_speedup_sum += w * math.log(speedup)
        total_weight += w
        per_scenario.append({
            "pct": pct,
            "threads": nt,
            "dist": dist,
            "candidate_us_per_op": cand_us,
            "baseline_us_per_op": base_us,
            "speedup": speedup,
            "cache_hit_rate": out["cache_hit_rate"],
            "metadata_hit_rate": out.get("metadata_hit_rate"),
            "final_scheme_full": out.get("final_scheme_full"),
            "final_scheme_partitioned": out.get("final_scheme_partitioned"),
            "final_scheme_unify": out.get("final_scheme_unify"),
            "scheme_transitions": out.get("scheme_transitions"),
            "reapply_count": out.get("reapply_count"),
        })

    geomean_speedup = math.exp(log_speedup_sum / max(1e-9, total_weight))
    # combined_score is what OpenEvolve uses for MAP-Elites selection /
    # best-program tracking. Must equal fitness (otherwise OpenEvolve falls
    # back to averaging all numeric metrics, which dilutes the signal with
    # per_scenario cache_hit_rates etc.).
    return {
        "fitness": geomean_speedup,
        "combined_score": geomean_speedup,
        "geomean_speedup": geomean_speedup,
        "per_scenario": per_scenario,
        "build_s": build_s,
        "runtime_s": runtime_s,
    }


if __name__ == "__main__":
    out = evaluate(str(INIT_PROG))
    print(json.dumps(out, indent=2))
