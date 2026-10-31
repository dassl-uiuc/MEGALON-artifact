#!/usr/bin/env python3
"""Plot an evaluation before vs after the coherence patch, in the style of the paper's figure.

Usage: plot_patch_compare.py <results.csv> <eval2|eval3|eval4> <out.png>

results.csv columns: eval,system,phase,w_ratio,zipf,num_obj,scr_mb,avg_latency_us,throughput_mops
  system: MEGALON | HCMeta | HCMeta-local;  phase: before | after

  eval2 -> Figure 5 (a, b):   throughput and average latency vs dataset size (read-only)
  eval3 -> Figure 6 (a, b):   throughput vs dataset size, 5% and 50% writes
  eval4 -> Figure 7 (b)(ii):  throughput vs SCR size, 50% writes, Zipf 0.7
"""
import csv
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402

INK, INK2 = "#0b0b0b", "#52514e"
SYSTEMS = [  # name, color, marker (categorical slots 1-3, validated as a set)
    ("MEGALON", "#2a78d6", "o"),
    ("HCMeta", "#eb6834", "s"),
    ("HCMeta-local", "#1baf7a", "^"),
]
PHASES = [("before", "--"), ("after", "-")]

SIZE_LABEL = "Dataset size (million objects)"
TPUT_LABEL = "Throughput (Mops/s)"
# eval -> (title, panels); a panel is (subtitle, row filter, x field, y field, x label, y label)
FIGURES = {
    "eval2": ("Figure 5: before vs. after patch", [
        ("(a) Throughput", lambda r: True, "num_obj", "throughput_mops", SIZE_LABEL, TPUT_LABEL),
        ("(b) Average latency", lambda r: True, "num_obj", "avg_latency_us", SIZE_LABEL, "Latency (µs)"),
    ]),
    "eval3": ("Figure 6: before vs. after patch", [
        ("(a) 5% write", lambda r: float(r["w_ratio"]) == 0.05, "num_obj", "throughput_mops", SIZE_LABEL, TPUT_LABEL),
        ("(b) 50% write", lambda r: float(r["w_ratio"]) == 0.5, "num_obj", "throughput_mops", SIZE_LABEL, TPUT_LABEL),
    ]),
    "eval4": ("Figure 7(b)(ii): before vs. after patch", [
        ("50% write, less skewed", lambda r: True, "scr_mb", "throughput_mops", "SCR size (MB)", TPUT_LABEL),
    ]),
}


def place_labels(ax, ends, min_gap_pt=11):
    """Direct-label each series at its right end (identity is never color-alone), nudging labels apart
    vertically so that series ending close together stay readable."""
    if not ends:
        return
    fig = ax.figure
    fig.canvas.draw()
    to_pt = 72.0 / fig.dpi
    pts = sorted(((ax.transData.transform(xy)[1] * to_pt, xy, name) for xy, name in ends))
    ys = [p[0] for p in pts]
    for i in range(1, len(ys)):  # push upward until every pair is min_gap_pt apart
        ys[i] = max(ys[i], ys[i - 1] + min_gap_pt)
    for (y0, xy, name), y in zip(pts, ys):
        ax.annotate(name, xy, xytext=(8, y - y0), textcoords="offset points", va="center", fontsize=9)


def xval(row, field):
    return int(row[field]) / 1e6 if field == "num_obj" else float(row[field])


def main(csv_path, eval_name, out_path):
    title, panels = FIGURES[eval_name]
    with open(csv_path) as fh:
        rows = [r for r in csv.DictReader(fh) if r["eval"] == eval_name]
    if not rows:
        sys.exit(f"no {eval_name} rows in {csv_path}")
    present = [s for s in SYSTEMS if any(r["system"] == s[0] for r in rows)]

    plt.rcParams.update({"font.size": 10, "axes.edgecolor": INK2, "axes.labelcolor": INK,
                         "xtick.color": INK2, "ytick.color": INK2, "text.color": INK})
    width = 5.5 * len(panels) if len(panels) > 1 else 7.5
    fig, axes = plt.subplots(1, len(panels), figsize=(width, 4.2), facecolor="white", squeeze=False)

    for ax, (subtitle, keep, xf, yf, xlabel, ylabel) in zip(axes[0], panels):
        ax.set_facecolor("white")
        sel = [r for r in rows if keep(r)]
        xs_all = sorted({xval(r, xf) for r in sel})
        ends = []
        for name, color, marker in present:
            last = None
            for phase, style in PHASES:
                pts = sorted((xval(r, xf), float(r[yf])) for r in sel if r["system"] == name and r["phase"] == phase)
                if not pts:
                    continue
                filled = phase == "after"
                ax.plot([p[0] for p in pts], [p[1] for p in pts], linestyle=style, color=color, linewidth=2,
                        marker=marker, markersize=7, markerfacecolor=color if filled else "white",
                        markeredgecolor=color, markeredgewidth=1.6, zorder=3 if filled else 2)
                last = pts[-1]
            if last:
                ends.append((last, name))
        if xf == "scr_mb":  # SCR sizes double from point to point
            ax.set_xscale("log", base=2)
        ax.set_xticks(xs_all)
        ax.set_xticklabels([f"{x:g}" for x in xs_all])
        ax.minorticks_off()
        ax.set_xlim(*((xs_all[0] / 1.3, xs_all[-1] * 2.2) if xf == "scr_mb" else (xs_all[0] - 0.9, xs_all[-1] + 4.5)))
        ax.set_ylim(bottom=0)
        ax.set_xlabel(xlabel)
        ax.set_ylabel(ylabel)
        ax.set_title(subtitle, loc="left", fontsize=11)
        ax.grid(False)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        place_labels(ax, ends)

    handles = [Line2D([], [], color=c, marker=m, linewidth=2, markersize=7, label=n) for n, c, m in present]
    handles += [Line2D([], [], color=INK2, linestyle="--", linewidth=2, marker="o", markerfacecolor="white",
                       markeredgecolor=INK2, label="before patch"),
                Line2D([], [], color=INK2, linestyle="-", linewidth=2, marker="o", markerfacecolor=INK2,
                       markeredgecolor=INK2, label="after patch")]
    fig.suptitle(title, fontsize=12, y=0.995)
    fig.legend(handles=handles, loc="upper center", ncol=len(handles), frameon=False, bbox_to_anchor=(0.5, 0.95))
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(out_path, dpi=200, facecolor="white", bbox_inches="tight")
    print("wrote", out_path)


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[2] not in FIGURES:
        sys.exit(__doc__)
    main(*sys.argv[1:])
