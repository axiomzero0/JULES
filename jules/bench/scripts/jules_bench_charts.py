#!/usr/bin/env python3
"""Benchmark charts for the JULES vs GCC vs Clang report.

Follows typesetting/charts.md: no top/right spines, dashed grid at 20%
opacity, legend without border placed outside the data area, palette
colors only, no internal chart title (captions live in the document).
"""
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.font_manager as fm
fm.fontManager.addfont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
import matplotlib.pyplot as plt

plt.rcParams["font.sans-serif"] = ["DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False

# Palette (cascade, seed 19)
ACCENT = "#94771d"       # julesc (XS tier)
HEADER_FILL = "#504a37"  # gcc (M tier)
ACCENT_2 = "#4faccb"     # clang (XS tier)
TEXT_PRIMARY = "#242320"
TEXT_MUTED = "#85827a"
BORDER = "#c5bfab"

KERNELS = ["fib", "tak", "primes", "mandel", "flops", "inthash"]
CONFIG = {"julesc": "julesc-aot", "gcc": "gcc-O3", "clang": "clang-O3"}
COLORS = {"julesc": ACCENT, "gcc": HEADER_FILL, "clang": ACCENT_2}

rows = list(csv.DictReader(open("/home/z/my-project/jules/bench/results/results.csv")))
def med(kernel, config):
    return float(next(r for r in rows if r["kernel"] == kernel and r["config"] == config)["median_s"])
def ctime(kernel, config):
    return float(next(r for r in rows if r["kernel"] == kernel and r["config"] == config)["compile_s"])

def style_axes(ax):
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_color(BORDER)
    ax.spines["bottom"].set_color(BORDER)
    ax.tick_params(colors=TEXT_MUTED, labelsize=10)
    for lbl in ax.get_xticklabels() + ax.get_yticklabels():
        lbl.set_color(TEXT_PRIMARY)

# ---- Figure 1: runtime grouped bars (log scale) --------------------------
fig, ax = plt.subplots(figsize=(7.4, 3.6), constrained_layout=True)
n = len(KERNELS)
width = 0.26
xs = list(range(n))
for i, (key, label) in enumerate([("julesc", "julesc (AOT)"),
                                   ("gcc", "gcc -O3"),
                                   ("clang", "clang -O3")]):
    vals = [med(k, CONFIG[key]) for k in KERNELS]
    bars = ax.bar([x + (i - 1) * width for x in xs], vals, width,
                  color=COLORS[key], label=label, edgecolor="none", zorder=3)
    for b, v in zip(bars, vals):
        ax.annotate(f"{v*1000:.0f}", (b.get_x() + b.get_width() / 2, v),
                    xytext=(0, 2), textcoords="offset points",
                    ha="center", fontsize=7.5, color=TEXT_MUTED, zorder=4)
ax.set_yscale("log")
ax.set_ylabel("CPU time per run (s, log scale)", fontsize=10.5, color=TEXT_PRIMARY)
ax.set_xticks(xs)
ax.set_xticklabels(KERNELS)
ax.grid(axis="y", linestyle="--", linewidth=0.5, alpha=0.2, zorder=0)
style_axes(ax)
ax.legend(loc="upper left", bbox_to_anchor=(0.0, 1.14), ncol=3, frameon=False,
          fontsize=9.5, handlelength=1.2, columnspacing=1.6)
fig.savefig("/home/z/my-project/jules/bench/results/fig_runtime.png", dpi=200)
plt.close(fig)

# ---- Figure 2: compile time grouped bars ---------------------------------
fig, ax = plt.subplots(figsize=(7.4, 3.2), constrained_layout=True)
for i, (key, label) in enumerate([("julesc", "julesc (AOT)"),
                                   ("gcc", "gcc -O3"),
                                   ("clang", "clang -O3")]):
    vals = [ctime(k, CONFIG[key]) * 1000 for k in KERNELS]
    bars = ax.bar([x + (i - 1) * width for x in xs], vals, width,
                  color=COLORS[key], label=label, edgecolor="none", zorder=3)
    for b, v in zip(bars, vals):
        ax.annotate(f"{v:.0f}", (b.get_x() + b.get_width() / 2, v),
                    xytext=(0, 2), textcoords="offset points",
                    ha="center", fontsize=7.5, color=TEXT_MUTED, zorder=4)
ax.set_ylabel("Compile+link time (ms)", fontsize=10.5, color=TEXT_PRIMARY)
ax.set_xticks(xs)
ax.set_xticklabels(KERNELS)
ax.grid(axis="y", linestyle="--", linewidth=0.5, alpha=0.2, zorder=0)
style_axes(ax)
ax.legend(loc="upper left", bbox_to_anchor=(0.0, 1.16), ncol=3, frameon=False,
          fontsize=9.5, handlelength=1.2, columnspacing=1.6)
fig.savefig("/home/z/my-project/jules/bench/results/fig_compile.png", dpi=200)
plt.close(fig)
print("charts written")
