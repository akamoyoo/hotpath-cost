#!/usr/bin/env python3
"""The README's figure: one history replayed by the tool, and one compiler gap it found and fixed.

  docs/figures/history.py        reads out/ (replay.py results), writes history-light.svg and history-dark.svg
"""
import datetime as dt, json
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates

TOP = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
THEMES = {   # GitHub's page colours, and the reference palette's first categorical slot for each mode
    "light": dict(fg="#1f2328", fg2="#59636e", grid="#d1d9e0", line="#2a78d6", band="#2a78d6", before="#b7bdc5"),
    "dark":  dict(fg="#e6edf3", fg2="#9198a1", grid="#3d444d", line="#3987e5", band="#3987e5", before="#4b525b"),
}

def series(build, lib, op, key):
    d = TOP / "out" / build / lib
    pts = []
    for c, date, _ in json.load(open(d / "order.json")):
        f = d / f"{c}.jsonl"
        if f.exists():
            for r in map(json.loads, f.read_text().splitlines()):
                if r["op"] == op: pts.append((dt.date.fromisoformat(date), r[key]))
    pts.sort()
    return pts

def panel(ax, t, pts, title, ylabel, bands, notes, xlim, ylim, yticks, extra=None):
    xs, ys = zip(*pts)
    ax.step(xs + (xlim[1],), ys + (ys[-1],), where="post", color=t["line"], lw=2.2, solid_capstyle="round")
    for a, b in bands:
        ax.axvspan(a, b, color=t["band"], alpha=0.12, lw=0)
    for x, y, text, ha, dy in notes:
        ax.annotate(text, (x, y), xytext=(0, dy), textcoords="offset points", ha=ha, va="bottom" if dy > 0 else "top",
                    fontsize=9.5, color=t["fg2"])
    if extra: extra(ax, t)
    ax.set_title(title, loc="left", fontsize=12, fontweight="bold", color=t["fg"], pad=8)
    ax.set_ylabel(ylabel, fontsize=9.5, color=t["fg2"])
    ax.set_xlim(*xlim); ax.set_ylim(*ylim); ax.set_yticks(yticks)
    ax.grid(axis="y", color=t["grid"], lw=0.8); ax.set_axisbelow(True)
    for s in ("top", "right", "left"): ax.spines[s].set_visible(False)
    ax.spines["bottom"].set_color(t["grid"])
    ax.tick_params(colors=t["fg2"], labelsize=9, length=0)
    ax.xaxis.set_major_locator(mdates.YearLocator()); ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y"))

def main(out_png=None):
    D = dt.date
    ab = [p for p in series("gcc-instr", "abseil", "operator[] (hit)", "instr") if p[0] >= D(2024, 10, 1)]
    for mode, t in THEMES.items():
        fig, axes = plt.subplots(2, 1, figsize=(10, 5.8), gridspec_kw=dict(hspace=0.62, height_ratios=(1, 1.05)))
        fig.patch.set_alpha(0)
        for ax in axes: ax.set_facecolor("none")
        # MPMCQueue, try_push + try_pop, ns per call (cases/mpmcqueue/RESULT.txt): before and after the fix
        ax = axes[1]
        rows = [("GCC 13", 52.5, 29.6), ("GCC 15", 52.9, 29.6), ("clang 21", 30.3, 26.6)]
        h = 0.36
        for k, (name, a, b) in enumerate(rows):
            y = len(rows) - 1 - k
            ax.barh(y + h / 2 + 0.02, a, height=h, color=t["before"], lw=0)
            ax.barh(y - h / 2 - 0.02, b, height=h, color=t["line"], lw=0)
            ax.text(a + 0.8, y + h / 2 + 0.02, f"{a:.1f} ns  before", va="center", fontsize=9.5, color=t["fg2"])
            ax.text(b + 0.8, y - h / 2 - 0.02, f"{b:.1f} ns  after" + (f"  ({a / b:.1f}x faster)" if a / b > 1.3 else ""),
                    va="center", fontsize=9.5, color=t["fg"], fontweight="bold" if a / b > 1.3 else "normal")
        ax.set_yticks(range(len(rows))); ax.set_yticklabels([r[0] for r in reversed(rows)], fontsize=10, color=t["fg"])
        ax.set_xlim(0, 80); ax.set_xlabel("ns per try_push + try_pop (lower is better)", fontsize=9.5, color=t["fg2"])
        ax.set_title("MPMCQueue: 1.8x faster with GCC after our four-line fix, merged upstream (PR #58)",
                     loc="left", fontsize=12, fontweight="bold", color=t["fg"], pad=24)
        ax.grid(axis="x", color=t["grid"], lw=0.8); ax.set_axisbelow(True)
        for sp in ("top", "right", "left"): ax.spines[sp].set_visible(False)
        ax.spines["bottom"].set_color(t["grid"]); ax.tick_params(colors=t["fg2"], labelsize=9, length=0)
        ax.text(0, 1.03, "GCC divided by the queue's capacity twice per operation; the fix divides once",
                transform=ax.transAxes, ha="left", va="bottom", fontsize=9.5, color=t["fg2"])
        panel(axes[0], t, ab,
              "abseil: three refactorings in 2025, each adding 40 to 50 % to a hash-map lookup",
              "instructions per call", [(D(2025, 2, 18), D(2025, 3, 11)), (D(2025, 9, 2), D(2025, 9, 12))],
              [(D(2025, 2, 18), 105, "+40 %, undone in 3 weeks", "center", 5),
               (D(2025, 6, 20), 102, "+44 %, stays a year", "left", -6),
               (D(2025, 9, 2), 153, "+50 %, undone in 10 days", "left", 5)],
              (D(2024, 10, 1), D(2026, 12, 31)), (50, 175), [50, 100, 150])
        axes[0].xaxis.set_major_locator(mdates.MonthLocator(bymonth=(1, 7)))
        axes[0].xaxis.set_major_formatter(mdates.DateFormatter("%b %Y"))
        fig.text(0.0, -0.05, "Top: instructions per call at every measured library commit, replayed by hotpath-cost "
                 "under callgrind. Bottom: medians of runs on one idle core, -O2.", fontsize=9, color=t["fg2"])
        fig.savefig(HERE / f"history-{mode}.svg", bbox_inches="tight", transparent=True)
        if out_png:   # a preview on GitHub's page colour for the mode
            fig.patch.set_alpha(1); fig.patch.set_facecolor("#ffffff" if mode == "light" else "#0d1117")
            fig.savefig(f"{out_png}-{mode}.png", bbox_inches="tight", dpi=110, facecolor=fig.get_facecolor())
        plt.close(fig)

if __name__ == "__main__":
    import sys
    main(sys.argv[1] if len(sys.argv) > 1 else None)   # an optional PNG prefix, to look at the result
