#!/usr/bin/env python3
"""Print a compact comparison table for recorded experiments."""
from __future__ import annotations

import csv
import json
import sys
from pathlib import Path


def main() -> int:
    exp_root = Path(sys.argv[1] if len(sys.argv) > 1 else "experiments")
    rows = []
    for summary_path in sorted(exp_root.glob("*/summary.json")):
        summary = json.loads(summary_path.read_text())
        rows.append(summary)

    if not rows:
        print(f"no summaries found under {exp_root}")
        return 0

    rows.sort(key=lambda r: (r.get("best_fitness") is None, -(r.get("best_fitness") or 0)))
    fields = [
        "run_id",
        "status",
        "iterations_requested",
        "iterations_completed",
        "best_fitness",
        "best_iteration",
        "num_improvements",
    ]

    out_csv = exp_root / "summary.csv"
    with out_csv.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for row in rows:
            w.writerow({k: row.get(k, "") for k in fields})

    print(
        f"{'run_id':36s} {'status':9s} {'iter':>9s} {'done':>5s} "
        f"{'fitness':>10s} {'best_it':>7s} {'impr':>5s}"
    )
    print("-" * 88)
    for row in rows:
        fit = row.get("best_fitness")
        fit_s = "" if fit is None else f"{fit:.9f}"
        print(
            f"{row.get('run_id','')[:36]:36s} "
            f"{row.get('status','')[:9]:9s} "
            f"{str(row.get('iterations_requested','')):>9s} "
            f"{str(row.get('iterations_completed','')):>5s} "
            f"{fit_s:>10s} "
            f"{str(row.get('best_iteration','')):>7s} "
            f"{str(row.get('num_improvements','')):>5s}"
        )
    print(f"\nwrote {out_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
