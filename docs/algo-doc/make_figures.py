#!/usr/bin/env python3
"""Generate figures for the SA / sa-stress / orchestrator documentation PDF."""
import os
import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

OUT = os.path.dirname(os.path.abspath(__file__))
plt.rcParams.update({
    "font.size": 11,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "figure.dpi": 150,
    "savefig.bbox": "tight",
})

BLUE = "#2b6cb0"
GREEN = "#2f855a"
ORANGE = "#dd6b20"
RED = "#c53030"
GREY = "#718096"


def fig_cooling():
    """Sawtooth temperature schedule + acceptance-probability curve (two panels)."""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(9, 3.4))

    # --- Panel 1: wave-based temperature decay (Phase 1 params) ---
    # Two decays: within a wave currentTemp *= decT (0.999) every step (fast);
    # between waves startingTemp *= decTW (0.99) once (slow, ~1%/wave).
    T0 = 50.0
    decT = 0.999
    decTW = 0.99
    steps_per_wave = 700     # illustrative: real waves run all the way to tLim
    n_waves = 10
    xs, ys = [], []
    peak_x, peak_y = [], []
    step = 0
    wave_start_T = T0
    for wave in range(n_waves):
        T = wave_start_T
        peak_x.append(step)
        peak_y.append(T)
        for _ in range(steps_per_wave):
            xs.append(step)
            ys.append(T)
            T *= decT
            step += 1
        wave_start_T *= decTW   # line 1874: wave-start temperature drops 1%/wave
    ax1.plot(xs, ys, color=BLUE, lw=1.2, label="$T$ (per step, $\\times decT$)")
    ax1.plot(peak_x, peak_y, "o--", color=RED, lw=1.2, ms=4,
             label="wave-start envelope ($\\times decTW$)")
    ax1.set_title("Phase 1 temperature (waves)")
    ax1.set_xlabel("annealing step")
    ax1.set_ylabel("temperature $T$")
    ax1.legend(fontsize=7.5, frameon=False, loc="upper right")
    ax1.annotate("each wave starts 1% lower\n($50 \\to 49.5 \\to 49.0\\dots$)\n"
                 "$decTW$ needs ~hundreds of\nwaves to reach $t_{Lim}$",
                 xy=(peak_x[3], peak_y[3]),
                 xytext=(steps_per_wave * 3.4, T0 * 0.55),
                 fontsize=7.5, color=GREY,
                 arrowprops=dict(arrowstyle="->", color=GREY, lw=0.8))
    ax1.set_ylim(0, T0 * 1.08)

    # --- Panel 2: acceptance probability exp(-dC/T) ---
    dC = np.linspace(0, 10, 200)
    for T, c, lbl in [(50, BLUE, "$T=50$ (hot)"),
                      (5, GREEN, "$T=5$"),
                      (1, ORANGE, "$T=1$"),
                      (0.1, RED, "$T=0.1$ (cold)")]:
        ax2.plot(dC, np.exp(-dC / T), color=c, lw=1.6, label=lbl)
    ax2.set_title("Metropolis acceptance  $e^{-\\Delta C/T}$")
    ax2.set_xlabel("$\\Delta C$  (worsening move)")
    ax2.set_ylabel("accept probability")
    ax2.legend(fontsize=8, frameon=False)
    ax2.set_ylim(0, 1.02)

    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_cooling.pdf"))
    plt.close(fig)


def fig_results():
    """Grouped bar chart: our old best vs new best vs rival vs GD'25 winner."""
    graphs = ["A6\n(200/3000)", "A7\n(500/1740)", "A8\n(10466/20288)",
              "A9\n(2519/4938)"]
    old = [632, 39, 31, 21]
    new = [624, 28, 7, 10]
    rival = [621, 31, 12, 13]
    winner = [568, 31, 15, 12]

    x = np.arange(len(graphs))
    w = 0.2
    fig, ax = plt.subplots(figsize=(9, 3.8))
    ax.bar(x - 1.5 * w, old, w, label="our old best", color=GREY)
    ax.bar(x - 0.5 * w, new, w, label="our new best", color=GREEN)
    ax.bar(x + 0.5 * w, rival, w, label="rival team", color=BLUE)
    ax.bar(x + 1.5 * w, winner, w, label="GD'25 winner", color=ORANGE)
    ax.set_yscale("log")
    ax.set_ylabel("k  (max crossings on any edge, log)")
    ax.set_title("k-value per contest graph  —  lower is better")
    ax.set_xticks(x)
    ax.set_xticklabels(graphs)
    ax.legend(fontsize=8, frameon=False, ncol=4, loc="upper right")
    for i, n in enumerate(new):
        ax.text(i - 0.5 * w, n * 1.08, str(n), ha="center", va="bottom",
                fontsize=8, color=GREEN, fontweight="bold")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_results.pdf"))
    plt.close(fig)


def fig_stress_effect():
    """sa vs sa-stress: init quality collapses k on sparse graphs, hurts on dense."""
    graphs = ["A6 (dense\nm/n=15)", "A7", "A8", "A9"]
    sa = [624, 39, 31, 21]         # plain-sa / pre-stress baseline
    sastress = [737, 28, 7, 10]    # stress-init + SA
    x = np.arange(len(graphs))
    w = 0.36
    fig, ax = plt.subplots(figsize=(8.5, 3.6))
    b1 = ax.bar(x - w / 2, sa, w, label="SA (no stress init)", color=GREY)
    b2 = ax.bar(x + w / 2, sastress, w, label="sa-stress", color=GREEN)
    ax.set_yscale("log")
    ax.set_ylabel("k  (log)")
    ax.set_title("Effect of stress/force-directed initialisation")
    ax.set_xticks(x)
    ax.set_xticklabels(graphs)
    ax.legend(fontsize=9, frameon=False)
    for rects in (b1, b2):
        for r in rects:
            ax.text(r.get_x() + r.get_width() / 2, r.get_height() * 1.05,
                    f"{int(r.get_height())}", ha="center", va="bottom",
                    fontsize=8)
    ax.annotate("stress HURTS\non dense A6", xy=(0 + w / 2, 737),
                xytext=(0.55, 300), fontsize=8, color=RED,
                arrowprops=dict(arrowstyle="->", color=RED, lw=0.9))
    ax.annotate("3-4x drop\non sparse graphs", xy=(2 + w / 2, 7),
                xytext=(1.4, 2.3), fontsize=8, color=GREEN,
                arrowprops=dict(arrowstyle="->", color=GREEN, lw=0.9))
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_stress.pdf"))
    plt.close(fig)


def fig_warmchain():
    """Orchestrator warm-chain: monotone improvement across leases (e2e test data)."""
    fig, ax = plt.subplots(figsize=(8.5, 3.6))
    # From the B=220s e2e run: total-crossing envelope, warm-chained across leases.
    series = {
        "A6 (dense, 4 leases)": ([0, 1, 2, 3, 4], [1492, 980, 830, 770, 746], RED),
        "A1": ([0, 1, 2], [95, 30, 9], BLUE),
        "A9": ([0, 1, 2], [2383, 120, 11], GREEN),
    }
    for lbl, (xs, ys, c) in series.items():
        ax.plot(xs, ys, "o-", color=c, lw=1.8, ms=5, label=lbl)
    ax.set_yscale("log")
    ax.set_xlabel("lease index (warm-chained continuations)")
    ax.set_ylabel("best total crossings (log)")
    ax.set_title("Orchestrator warm-chaining stays monotone across leases")
    ax.legend(fontsize=9, frameon=False)
    ax.set_xticks([0, 1, 2, 3, 4])
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_warmchain.pdf"))
    plt.close(fig)


def _box(ax, x, y, w, h, text, color):
    b = FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.02,rounding_size=0.08",
                       linewidth=1.3, edgecolor=color, facecolor=color + "22")
    ax.add_patch(b)
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center", fontsize=9)


def _arrow(ax, x1, y1, x2, y2):
    ax.add_patch(FancyArrowPatch((x1, y1), (x2, y2), arrowstyle="-|>",
                                 mutation_scale=14, lw=1.3, color="#4a5568"))


def fig_orchestrator_flow():
    """Block diagram of the orchestrator loop."""
    fig, ax = plt.subplots(figsize=(9, 4.2))
    ax.set_xlim(0, 10)
    ax.set_ylim(0, 8)
    ax.axis("off")

    _box(ax, 0.3, 6.6, 2.4, 1.0, "Classify graphs\n(n, m) -> method", BLUE)
    _box(ax, 3.4, 6.6, 3.0, 1.0, "EXPLORE\n1 quantum / graph\n(hardest first)", GREEN)
    _box(ax, 7.0, 6.6, 2.7, 1.0, "measure dk/dt slope\nfrom .trace envelope", ORANGE)

    _box(ax, 3.4, 4.2, 3.0, 1.1,
         "REALLOCATE\nbid = dk/dt x Q\nserve top bidder", GREEN)
    _box(ax, 7.0, 4.3, 2.7, 0.9, "warm-chain lease\n(sa-warm, --init input)", BLUE)

    _box(ax, 0.3, 4.3, 2.4, 0.9, "Convergence check\ndrop flat graphs", RED)
    _box(ax, 3.4, 1.9, 3.0, 1.0, "budget exhausted?", GREY)
    _box(ax, 7.0, 1.9, 2.7, 1.0, "verify + write\nsubmission / graph", ORANGE)

    _arrow(ax, 2.7, 7.1, 3.4, 7.1)
    _arrow(ax, 6.4, 7.1, 7.0, 7.1)
    _arrow(ax, 8.35, 6.6, 4.9, 5.3)          # slope -> reallocate
    _arrow(ax, 6.4, 4.75, 7.0, 4.75)         # reallocate -> warm lease
    _arrow(ax, 8.35, 4.3, 8.35, 3.0)         # lease -> down
    _arrow(ax, 7.0, 2.4, 6.4, 2.4)           # to budget check
    _arrow(ax, 3.4, 4.6, 2.7, 4.7)           # reallocate -> convergence
    _arrow(ax, 1.5, 4.3, 1.5, 3.0)
    _arrow(ax, 1.5, 2.9, 3.4, 2.4)           # convergence loop -> budget
    _arrow(ax, 4.9, 2.9, 4.9, 4.2)           # budget: no -> reallocate (loop)
    ax.text(5.05, 3.5, "no", fontsize=8, color=GREY)
    ax.text(6.55, 2.55, "yes", fontsize=8, color=GREY)

    fig.savefig(os.path.join(OUT, "fig_flow.pdf"))
    plt.close(fig)


if __name__ == "__main__":
    fig_cooling()
    fig_results()
    fig_stress_effect()
    fig_warmchain()
    fig_orchestrator_flow()
    print("figures written to", OUT)
