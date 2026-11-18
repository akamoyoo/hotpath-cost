#!/usr/bin/env python3
"""The README's figure: two histories replayed by the tool, one panel each.

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
    "light": dict(fg="#1f2328", fg2="#59636e", grid="#d1d9e0", line="#2a78d6", band="#2a78d6", ours="#1baf7a"),
    "dark":  dict(fg="#e6edf3", fg2="#9198a1", grid="#3d444d", line="#3987e5", band="#3987e5", ours="#199e70"),
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
    mp = series("gcc", "msgpack-c", "unpack, reused zone", "alloc")
    ab = [p for p in series("gcc-instr", "abseil", "operator[] (hit)", "instr") if p[0] >= D(2024, 10, 1)]
    for mode, t in THEMES.items():
        fig, axes = plt.subplots(2, 1, figsize=(10, 5.4), gridspec_kw=dict(hspace=0.55))
        fig.patch.set_alpha(0)
        for ax in axes: ax.set_facecolor("none")
        def ours(ax, t):
            x = D(2026, 11, 17)
            ax.plot([x], [0], marker="o", ms=8, mfc=t["ours"], mec=t["ours"], ls="none")
            ax.annotate("our fix: 0 (msgpack-c PR #1223)", (x, 0), xytext=(-8, 0), textcoords="offset points",
                        ha="right", va="center", fontsize=9.5, color=t["fg"])
        panel(axes[1], t, mp,
              "msgpack-c: one more heap allocation per message since 2016, and still there",
              "allocations per call", [(D(2016, 5, 22), D(2026, 11, 17))],
              [(D(2016, 5, 22), 2.0, "d15d9220  visitor API", "left", 6)],
              (D(2015, 3, 1), D(2026, 12, 31)), (-0.3, 2.8), [0, 1, 2], extra=ours)
        panel(axes[0], t, ab,
              "abseil: three refactorings in 2025, each adding 40 to 50 % to a hash-map lookup",
              "instructions per call", [(D(2025, 2, 18), D(2025, 3, 11)), (D(2025, 9, 2), D(2025, 9, 12))],
              [(D(2025, 2, 18), 105, "+40 %, undone in 3 weeks", "center", 5),
               (D(2025, 6, 20), 102, "+44 %, stays a year", "left", -6),
               (D(2025, 9, 2), 153, "+50 %, undone in 10 days", "left", 5)],
              (D(2024, 10, 1), D(2026, 12, 31)), (50, 175), [50, 100, 150])
        axes[0].xaxis.set_major_locator(mdates.MonthLocator(bymonth=(1, 7)))
        axes[0].xaxis.set_major_formatter(mdates.DateFormatter("%b %Y"))
        fig.text(0.0, -0.01, "Exact counts per call at every measured library commit, replayed by hotpath-cost "
                 "(GCC 13, -O2); instructions under callgrind.", fontsize=9, color=t["fg2"])
        fig.savefig(HERE / f"history-{mode}.svg", bbox_inches="tight", transparent=True)
        if out_png:   # a preview on GitHub's page colour for the mode
            fig.patch.set_alpha(1); fig.patch.set_facecolor("#ffffff" if mode == "light" else "#0d1117")
            fig.savefig(f"{out_png}-{mode}.png", bbox_inches="tight", dpi=110, facecolor=fig.get_facecolor())
        plt.close(fig)

if __name__ == "__main__":
    import sys
    main(sys.argv[1] if len(sys.argv) > 1 else None)   # an optional PNG prefix, to look at the result
