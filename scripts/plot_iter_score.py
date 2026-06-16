#!/usr/bin/env python3
import re
from datetime import datetime
from pathlib import Path
import matplotlib.pyplot as plt

LOG = Path("/home/godong/hymeta_evolve/evolve_runs/run.log")

iter_re = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}),\d+ - INFO - Iteration (\d+):")
fit_re = re.compile(r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2},\d+ - INFO - Metrics: fitness=([0-9.]+)")

iters, times, fits = [], [], []
pending = None
with LOG.open() as f:
    for line in f:
        m = iter_re.match(line)
        if m:
            pending = (int(m.group(2)), datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S"))
            continue
        if pending is not None:
            mf = fit_re.match(line)
            if mf:
                iters.append(pending[0])
                times.append(pending[1])
                fits.append(float(mf.group(1)))
                pending = None

print(f"parsed {len(iters)} points; final fitness={fits[-1]:.4f}, best={max(fits):.4f}")

t0 = times[0]
elapsed_h = [(t - t0).total_seconds() / 3600.0 for t in times]
best_so_far = []
cur = -1.0
for v in fits:
    if v > cur:
        cur = v
    best_so_far.append(cur)

plt.rcParams.update({
    "font.size": 28,
    "axes.labelsize": 32,
    "xtick.labelsize": 26,
    "ytick.labelsize": 26,
    "legend.fontsize": 26,
    "lines.linewidth": 3.0,
})

fig, ax = plt.subplots(figsize=(16, 10))
ax.scatter(iters, fits, s=24, alpha=0.4, color="#1f77b4", label="per-iter fitness")
ax.plot(iters, best_so_far, color="#d62728", linewidth=4, label="best-so-far")
ax.axhline(1.0, color="gray", linestyle="--", linewidth=2, label="baseline (1.0)")
best_iter = iters[fits.index(max(fits))]
ax.axvline(best_iter, color="#2ca02c", linestyle=":", linewidth=2.5,
           label=f"best @ iter {best_iter} ({max(fits):.4f})")
ax.set_xlabel("Iteration")
ax.set_ylabel("Fitness")
ax.grid(alpha=0.3)
ax.legend(loc="lower right")
ax.tick_params(axis="both", which="major", length=8, width=1.5)

fig.tight_layout()
out = Path("/home/godong/hymeta_evolve/evolve_runs/fitness_progress.png")
fig.savefig(out, dpi=140, bbox_inches="tight")
print(f"saved {out}")
