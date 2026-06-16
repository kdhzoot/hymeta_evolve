#!/usr/bin/env python3
"""Heatmap of per-scenario speedup for best-fitness program at iter 1000."""
import json
from pathlib import Path
import matplotlib.pyplot as plt
import numpy as np

INFO = Path("/home/godong/hymeta_evolve/evolve_runs/best/best_program_info.json")
info = json.loads(INFO.read_text())
scen = info["metrics"]["per_scenario"]
fit = info["metrics"]["fitness"]

dists = ["uniform", "zipfian", "mixgraph"]
pcts = [2.0, 1.0, 0.5, 0.25, 0.1]
M = np.zeros((len(dists), len(pcts)))
for s in scen:
    i = dists.index(s["dist"])
    j = pcts.index(s["pct"])
    M[i, j] = s["speedup"]

plt.rcParams.update({
    "font.size": 24,
    "axes.labelsize": 28,
    "xtick.labelsize": 24,
    "ytick.labelsize": 24,
})

fig, ax = plt.subplots(figsize=(14, 7))
im = ax.imshow(M, cmap="RdYlGn", aspect="auto", vmin=1.0, vmax=1.55)

ax.set_xticks(range(len(pcts)))
ax.set_xticklabels([f"{p}%" for p in pcts])
ax.set_yticks(range(len(dists)))
ax.set_yticklabels(dists)
ax.set_xlabel("Cache budget")
ax.set_ylabel("Workload")

for i in range(len(dists)):
    for j in range(len(pcts)):
        v = M[i, j]
        ax.text(j, i, f"{v:.3f}", ha="center", va="center",
                fontsize=26, fontweight="bold",
                color="white" if v < 1.18 or v > 1.45 else "black")

cbar = fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)
cbar.set_label("Speedup (×)", fontsize=24)
cbar.ax.tick_params(labelsize=20)

fig.tight_layout()
out = Path("/home/godong/hymeta_evolve/evolve_runs/per_scenario_heatmap.png")
fig.savefig(out, dpi=140, bbox_inches="tight")
print(f"saved {out}; fitness={fit:.4f}, min={M.min():.4f}, max={M.max():.4f}")
