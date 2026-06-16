"""
28-config validation: sim vs real RocksDB throughput comparison.

Scope:
  Pure: 12 = {Full, Partitioned, Unify} × {5%, 2%, 1%, 0.5%} × 16T
  Hybrid: 16 = {P1, P2, P3, P4} × {2%, 1%, 0.5%, 0.2%} × 16T

Approach (optimized):
  1. Pre-build 7 binaries (one per unique policy) sequentially. Each ~10s.
  2. Run all 28 × 2 (post-hoc + DES) = 56 sims in parallel using num_ops=3M.
  3. Each sim ~2s; with parallel pool 8, ~15s total.

Total runtime: ~70s (build) + ~20s (sim) = ~1.5 min.
"""
import csv
import glob
import json
import multiprocessing as mp
import shutil
import statistics
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).parent.resolve()
BUILD = HERE / "build"
LAYOUT = "/home/godong/hymeta_evolve/bench_results/sst_layout.json"
RESULTS = Path("/home/godong/hymeta_evolve/bench_results")
BIN_DIR = Path("/tmp/hymeta_sim_bins")

OUT_CSV = RESULTS / "exp_validation_28configs.csv"
OUT_REPORT = RESULTS / "exp_validation_28configs_report.txt"

DB_SIZE = 250 * 1024 ** 3
NUM_OPS = 10_000_000  # sweet spot: 5% cache fully warms but doesn't over-saturate
THREADS = 16

SEED_HYBRID = """\
#include "sim_engine.hpp"
namespace hymeta {{
static constexpr Scheme m[7] = {{ {s} }};
Scheme select_scheme(const SSTStats& s) {{
  int lv = s.level; if (lv<0) return SCHEME_FULL; if (lv>=7) return SCHEME_UNIFY;
  return m[lv];
}}
}}
"""
SEED_ALL = """\
#include "sim_engine.hpp"
namespace hymeta {{
Scheme select_scheme(const SSTStats&) {{ return {c}; }}
}}
"""


def parse_pref(pref):
    full_last, part_last = map(int, pref.split(","))
    schemes = []
    for lv in range(7):
        if lv <= full_last:
            schemes.append("SCHEME_FULL")
        elif part_last >= 0 and lv <= part_last:
            schemes.append("SCHEME_PARTITIONED")
        else:
            schemes.append("SCHEME_UNIFY")
    return schemes


def policy_key(mode, level_pref):
    if mode == "himeta":
        return f"hyb_{level_pref.replace(',', '_')}"
    return mode


def write_seed(mode, level_pref):
    if mode == "himeta":
        src = SEED_HYBRID.format(s=",".join(parse_pref(level_pref)))
    else:
        const = {"full": "SCHEME_FULL", "partitioned": "SCHEME_PARTITIONED",
                 "unify": "SCHEME_UNIFY"}[mode]
        src = SEED_ALL.format(c=const)
    (HERE / "initial_program.cpp").write_text(src)


def prebuild(policies):
    """Sequentially build 7 unique-policy binaries to /tmp/hymeta_sim_bins/."""
    BIN_DIR.mkdir(exist_ok=True)
    for i, (mode, lp) in enumerate(policies, 1):
        key = policy_key(mode, lp)
        bin_path = BIN_DIR / f"sim_{key}"
        t0 = time.time()
        print(f"  [{i}/{len(policies)}] Building {key:<12} ...", flush=True, end=" ")
        write_seed(mode, lp)
        subprocess.run(["cmake", "--build", str(BUILD), "-j4", "--target", "sim_main"],
                       check=True, capture_output=True)
        shutil.copy2(BUILD / "sim_main", bin_path)
        print(f"done ({time.time() - t0:.1f}s)", flush=True)
    return {policy_key(m, l): str(BIN_DIR / f"sim_{policy_key(m, l)}") for m, l in policies}


def latest_unify5_csv():
    cands = sorted(glob.glob(str(RESULTS / "exp_cache_budget_real_16t_unify5_*.csv")))
    return Path(cands[-1]) if cands else None


def load_real_configs():
    configs = []
    f = RESULTS / "exp_cache_budget_real_16t_20260420_210507.csv"
    with f.open() as fp:
        for row in csv.DictReader(fp):
            configs.append({
                "label": f"all_{row['scheme'].capitalize()}", "mode": row["scheme"],
                "level_pref": "", "cache_pct": float(row["cache_pct"]),
                "cache_bytes": int(row["cache_bytes"]),
                "real_us": float(row["us_per_op"]), "real_ops_sec": float(row["ops_sec"]),
                "category": "pure",
            })

    f = RESULTS / "exp_extended_16t.csv"
    with f.open() as fp:
        reader = csv.reader(fp)
        next(reader)
        for fields in reader:
            if len(fields) == 12:
                _, label, mode, lp1, lp2, cache_pct, cache_bytes, us, ops, *_ = fields
                level_pref = f"{lp1},{lp2}"
            else:
                _, label, mode, level_pref, cache_pct, cache_bytes, us, ops, *_ = fields
            cache_pct_f = float(cache_pct)
            if label == "all_Unify" and cache_pct_f in (2.0, 1.0, 0.5):
                configs.append({
                    "label": "all_Unify", "mode": "unify", "level_pref": "",
                    "cache_pct": cache_pct_f, "cache_bytes": int(cache_bytes),
                    "real_us": float(us), "real_ops_sec": float(ops),
                    "category": "pure",
                })
            elif label.startswith(("P1_", "P2_", "P3_", "P4_")) and cache_pct_f == 0.2:
                configs.append({
                    "label": label, "mode": "himeta", "level_pref": level_pref,
                    "cache_pct": cache_pct_f, "cache_bytes": int(cache_bytes),
                    "real_us": float(us), "real_ops_sec": float(ops),
                    "category": "hybrid",
                })

    fu5 = latest_unify5_csv()
    if fu5:
        with fu5.open() as fp:
            for row in csv.DictReader(fp):
                configs.append({
                    "label": "all_Unify", "mode": "unify", "level_pref": "",
                    "cache_pct": float(row["cache_pct"]),
                    "cache_bytes": int(row["cache_bytes"]),
                    "real_us": float(row["us_per_op"]),
                    "real_ops_sec": float(row["ops_sec"]),
                    "category": "pure",
                })

    for pct_label in ("2pct", "1pct", "0p5pct"):
        cands = sorted(glob.glob(str(RESULTS / f"exp_hybrid_16t_{pct_label}_*.csv")))
        if not cands:
            continue
        with Path(cands[-1]).open() as fp:
            reader = csv.reader(fp)
            next(reader)
            for fields in reader:
                if len(fields) == 10:
                    policy, lp1, lp2, cache_pct, cache_bytes, us, ops, *_ = fields
                    level_pref = f"{lp1},{lp2}"
                else:
                    policy, level_pref, cache_pct, cache_bytes, us, ops, *_ = fields
                configs.append({
                    "label": policy, "mode": "himeta", "level_pref": level_pref,
                    "cache_pct": float(cache_pct), "cache_bytes": int(cache_bytes),
                    "real_us": float(us), "real_ops_sec": float(ops),
                    "category": "hybrid",
                })
    return configs


def run_sim_task(args):
    """Single sim run. Returns dict with raw outputs (no errors computed yet)."""
    bin_path, cache_bytes, des_mode = args
    r = subprocess.run(
        [bin_path, f"--layout={LAYOUT}",
         f"--cache-bytes={cache_bytes}", f"--num-ops={NUM_OPS}",
         f"--num-threads={THREADS}", "--found-rate=0.63",
         "--seed=42", "--dist=uniform", f"--des-mode={des_mode}"],
        check=True, capture_output=True, text=True,
    )
    return json.loads(r.stdout.strip())


def main():
    print(f"=== 28-config sim validation (num_ops={NUM_OPS:,}) ===\n", flush=True)

    print("[1/3] Loading real RocksDB measurements ...")
    configs = load_real_configs()
    n_pure = sum(1 for c in configs if c["category"] == "pure")
    n_hyb = sum(1 for c in configs if c["category"] == "hybrid")
    print(f"  Loaded {len(configs)} configs ({n_pure} pure + {n_hyb} hybrid)\n",
          flush=True)

    # Group by unique policy
    groups = defaultdict(list)
    for cfg in configs:
        groups[(cfg["mode"], cfg["level_pref"])].append(cfg)
    policies = list(groups.keys())
    print(f"[2/3] Pre-building {len(policies)} unique-policy binaries to {BIN_DIR}:")
    t_build = time.time()
    bin_map = prebuild(policies)
    print(f"  Build phase done in {time.time() - t_build:.1f}s\n", flush=True)

    # Build task list: post-hoc only (DES unreliable at high cache, dropped per user)
    print(f"[3/3] Running {len(configs)} sims (post-hoc only) in parallel ...")
    tasks = []
    for cfg in configs:
        bp = bin_map[policy_key(cfg["mode"], cfg["level_pref"])]
        tasks.append((bp, cfg["cache_bytes"], 0))

    t_sim = time.time()
    pool_size = min(8, mp.cpu_count())
    completed = 0
    results = [None] * len(tasks)
    with mp.Pool(pool_size) as pool:
        for idx, res in enumerate(pool.imap(run_sim_task, tasks)):
            results[idx] = res
            completed += 1
            if completed % 8 == 0 or completed == len(tasks):
                print(f"  progress {completed}/{len(tasks)} "
                      f"({time.time() - t_sim:.1f}s)", flush=True)
    print(f"  Sim phase done in {time.time() - t_sim:.1f}s\n", flush=True)

    # Aggregate (post-hoc only)
    rows = []
    for i, cfg in enumerate(configs):
        sim0 = results[i]
        real_us = cfg["real_us"]; real_ops = cfg["real_ops_sec"]
        s0u = sim0["effective_us_per_op"]
        s0o = sim0["throughput_ops_sec_total"]
        rows.append({
            "category": cfg["category"], "label": cfg["label"], "mode": cfg["mode"],
            "level_pref": cfg["level_pref"], "cache_pct": cfg["cache_pct"],
            "cache_bytes": cfg["cache_bytes"],
            "real_us_per_op": real_us, "real_ops_sec": real_ops,
            "sim_us_per_op": s0u, "sim_ops_sec": s0o,
            "err_us_pct": (s0u - real_us) / real_us * 100,
            "err_ops_pct": (s0o - real_ops) / real_ops * 100,
            "sim_hit_rate": sim0["cache_hit_rate"],
        })

    with OUT_CSV.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    def mae(vs):
        a = [abs(v) for v in vs]
        return (statistics.mean(a) if a else 0.0, max(a) if a else 0.0)

    out = ["", "=" * 80,
           f"28-config sim vs real validation (num_ops={NUM_OPS:,}, post-hoc, throughput-focused)",
           "=" * 80, ""]
    out.append(f"{'category':<10} {'N':>4}  {'MAE us/op':>14}  {'MAE throughput':>18}")
    out.append("-" * 60)
    for cat in ("pure", "hybrid", "all"):
        sub = rows if cat == "all" else [r for r in rows if r["category"] == cat]
        if not sub:
            continue
        a, b = mae([r["err_us_pct"] for r in sub])
        c, d = mae([r["err_ops_pct"] for r in sub])
        out.append(f"{cat:<10} {len(sub):>4}  "
                   f"{a:>6.2f}% (max{b:>5.1f})  "
                   f"{c:>6.2f}% (max{d:>5.1f})")
    out.append("")
    out.append("Per-config (throughput, ops/sec):")
    out.append("-" * 70)
    out.append(f"{'cat':<8} {'label':<22} {'cache%':>6} | "
               f"{'real_K':>7} {'sim_K':>7} {'err%':>7}")
    out.append("-" * 70)
    for r in rows:
        out.append(f"{r['category']:<8} {r['label']:<22} "
                   f"{r['cache_pct']:>5.1f}% | "
                   f"{r['real_ops_sec']/1000:>6.1f}K {r['sim_ops_sec']/1000:>6.1f}K "
                   f"{r['err_ops_pct']:>+6.2f}%")
    text = "\n".join(out)
    print(text)
    OUT_REPORT.write_text(text)
    print(f"\nWrote: {OUT_CSV}")
    print(f"Wrote: {OUT_REPORT}")

    # Restore default seed (P1)
    write_seed("himeta", "2,3")
    subprocess.run(["cmake","--build",str(BUILD),"-j4","--target","sim_main"],
                   check=True, capture_output=True)


if __name__ == "__main__":
    main()
