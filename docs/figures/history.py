#!/usr/bin/env python3
"""The README's figure: abseil's flat_hash_map lookups across its history, replayed by the tool.

  docs/figures/history.py        reads out/ (replay.py results), writes history.svg
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
    "light": dict(fg="#1f2328", fg2="#59636e", grid="#d1d9e0", line="#2a78d6", band="#2a78d6", before="#9aa3ad"),
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
        fig, ax0 = plt.subplots(1, 1, figsize=(10, 3.4))
        axes = [ax0]
        fig.patch.set_facecolor("#ffffff")   # opaque: the same image reads on GitHub's light and dark pages
        for ax in axes: ax.set_facecolor("#ffffff")
        panel(axes[0], t, ab,
              "abseil: three refactorings in 2025, each adding 40 to 50 % to a hash-map lookup",
              "instructions per call", [(D(2025, 2, 18), D(2025, 3, 11)), (D(2025, 9, 2), D(2025, 9, 12))],
              [(D(2025, 2, 18), 105, "+40 %, undone in 3 weeks", "center", 5),
               (D(2025, 6, 20), 102, "+44 %, stays a year", "left", -6),
               (D(2025, 9, 2), 153, "+50 %, undone in 10 days", "left", 5)],
              (D(2024, 10, 1), D(2026, 12, 31)), (50, 175), [50, 100, 150])
        axes[0].xaxis.set_major_locator(mdates.MonthLocator(bymonth=(1, 7)))
        axes[0].xaxis.set_major_formatter(mdates.DateFormatter("%b %Y"))
        fig.text(0.0, -0.02, "operator[] on a map of 100,000 keys, instructions per call under callgrind at every measured "
                 "library commit (GCC 13, -O2). Each step is one commit.", fontsize=9, color=t["fg2"])
        fig.savefig(HERE / "history.svg", bbox_inches="tight", facecolor="#ffffff", pad_inches=0.25)
        if out_png: fig.savefig(f"{out_png}.png", bbox_inches="tight", dpi=110, facecolor="#ffffff", pad_inches=0.25)
        plt.close(fig)

if __name__ == "__main__":
    import sys
    main(sys.argv[1] if len(sys.argv) > 1 else None)   # an optional PNG prefix, to look at the result
