#!/usr/bin/env python3
"""Summarize one recorded OpenEvolve experiment.

Expected run layout:
  experiments/<run_id>/
    metadata.json
    output/
      run.log
      best/best_program_info.json

Writes:
  summary.json
  per_scenario.csv
  improvements.csv
"""
from __future__ import annotations

import csv
import json
import re
import sys
from pathlib import Path


def load_json(path: Path) -> dict:
    if not path.exists():
        return {}
    return json.loads(path.read_text())


def parse_iteration_metrics(log_path: Path) -> list[dict]:
    if not log_path.exists():
        return []

    records: list[dict] = []
    current_iter: int | None = None
    current_time: str | None = None

    iter_re = re.compile(
        r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}),\d+ - INFO - "
        r"Iteration (\d+):"
    )
    fitness_re = re.compile(r"Metrics: fitness=([0-9.]+)")

    for line in log_path.read_text(errors="ignore").splitlines():
        m = iter_re.match(line)
        if m:
            current_time = m.group(1)
            current_iter = int(m.group(2))
            continue
        if current_iter is None:
            continue
        m = fitness_re.search(line)
        if m:
            records.append(
                {
                    "iteration": current_iter,
                    "fitness": float(m.group(1)),
                    "timestamp": current_time,
                }
            )
            current_iter = None
            current_time = None

    return records


def best_improvements(records: list[dict]) -> list[dict]:
    best = float("-inf")
    out: list[dict] = []
    for rec in records:
        fitness = rec["fitness"]
        if fitness > best + 1e-12:
            prev = None if best == float("-inf") else best
            out.append(
                {
                    "iteration": rec["iteration"],
                    "timestamp": rec["timestamp"],
                    "fitness": fitness,
                    "delta": 0.0 if prev is None else fitness - prev,
                }
            )
            best = fitness
    return out


def write_per_scenario(path: Path, best_info: dict) -> None:
    rows = best_info.get("metrics", {}).get("per_scenario", [])
    preferred = [
        "pct",
        "threads",
        "dist",
        "candidate_us_per_op",
        "baseline_us_per_op",
        "speedup",
        "cache_hit_rate",
        "metadata_hit_rate",
        "final_scheme_full",
        "final_scheme_partitioned",
        "final_scheme_unify",
        "scheme_transitions",
        "reapply_count",
    ]
    extra = sorted({k for row in rows for k in row.keys()} - set(preferred))
    fields = [k for k in preferred if any(k in row for row in rows)] + extra
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for row in rows:
            w.writerow({k: row.get(k, "") for k in fields})


def write_improvements(path: Path, improvements: list[dict]) -> None:
    fields = ["iteration", "timestamp", "fitness", "delta"]
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for row in improvements:
            w.writerow(row)


def append_index(exp_root: Path, summary: dict) -> None:
    index_path = exp_root / "index.csv"
    fields = [
        "run_id",
        "status",
        "label",
        "iterations_requested",
        "iterations_completed",
        "best_fitness",
        "best_iteration",
        "best_program_id",
        "config",
        "start",
        "output_dir",
    ]
    existing: dict[str, dict] = {}
    if index_path.exists():
        with index_path.open(newline="") as f:
            for row in csv.DictReader(f):
                existing[row["run_id"]] = row

    row = {k: summary.get(k, "") for k in fields}
    row["output_dir"] = str(summary.get("output_dir", ""))
    existing[row["run_id"]] = row

    with index_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for run_id in sorted(existing):
            w.writerow(existing[run_id])


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: summarize_evolve_run.py <experiment-run-dir>", file=sys.stderr)
        return 2

    run_dir = Path(sys.argv[1]).resolve()
    output_dir = run_dir / "output"
    metadata = load_json(run_dir / "metadata.json")
    status = load_json(run_dir / "status.json")
    best_info = load_json(output_dir / "best" / "best_program_info.json")
    records = parse_iteration_metrics(output_dir / "run.log")
    improvements = best_improvements(records)

    metrics = best_info.get("metrics", {})
    best_iteration = best_info.get("iteration")
    if improvements:
        # The checkpoint best origin is usually more precise, but this fallback
        # keeps summaries usable even if best_program_info is missing.
        best_iteration = best_iteration or improvements[-1]["iteration"]

    summary = {
        "run_id": run_dir.name,
        "status": status.get("status", "unknown"),
        "exit_code": status.get("exit_code"),
        "label": metadata.get("label", ""),
        "config": metadata.get("config", ""),
        "start": metadata.get("start", ""),
        "iterations_requested": metadata.get("iterations"),
        "iterations_completed": len(records),
        "best_fitness": metrics.get("fitness"),
        "best_combined_score": metrics.get("combined_score"),
        "best_iteration": best_iteration,
        "best_generation": best_info.get("generation"),
        "best_program_id": best_info.get("id"),
        "best_parent_id": best_info.get("parent_id"),
        "best_runtime_s": metrics.get("runtime_s"),
        "best_build_s": metrics.get("build_s"),
        "num_improvements": len(improvements),
        "output_dir": str(output_dir),
    }

    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2))
    write_per_scenario(run_dir / "per_scenario.csv", best_info)
    write_improvements(run_dir / "improvements.csv", improvements)

    append_index(run_dir.parent, summary)
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
