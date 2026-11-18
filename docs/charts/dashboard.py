"""IOSVM history dashboard: commit pace, panic/stall commits, and fps by configuration.

Counts come from `git log` subjects on main (713 commits, 2026-10-31 .. 2026-11-18),
matched with: panic|spinlock|deadlock|stall|hang|freeze (case-insensitive).
fps values are the medians/figures reported in the stage docs and commit subjects.
"""
from datetime import date, timedelta
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from matplotlib.gridspec import GridSpec
from matplotlib.lines import Line2D

SURFACE = "#fcfcfb"
TEXT_P = "#0b0b0b"
TEXT_S = "#52514e"
GRID = "#e4e3df"
BLUE, ORANGE, AQUA, EXCL = "#2a78d6", "#eb6834", "#1baf7a", "#8a8984"

START = date(2026, 10, 31)
END = date(2026, 11, 18)
TOTAL = {  # commits per day on main
    "10-31": 84, "11-01": 39, "11-02": 22, "11-03": 26, "11-04": 17, "11-05": 14,
    "11-06": 45, "11-07": 26, "11-08": 40, "11-09": 95, "11-10": 42, "11-11": 91,
    "11-12": 37, "11-13": 10, "11-14": 45, "11-15": 40, "11-16": 1, "11-17": 25, "11-18": 14,
}
PANIC = {  # subset whose subject mentions panic/spinlock/deadlock/stall/hang/freeze
    "10-31": 4, "11-01": 5, "11-02": 3, "11-03": 5, "11-04": 2, "11-05": 1, "11-06": 2,
    "11-11": 5, "11-12": 1, "11-13": 1, "11-14": 1, "11-15": 3, "11-17": 6, "11-18": 2,
}
days = [START + timedelta(days=i) for i in range((END - START).days + 1)]
keys = [d.strftime("%m-%d") for d in days]
total = [TOTAL.get(k, 0) for k in keys]
panic = [PANIC.get(k, 0) for k in keys]
other = [t - p for t, p in zip(total, panic)]
assert sum(total) == 713, sum(total)

# fps by configuration (median or single reported figure; load differs, see labels)
CONFIGS = [
    ("1 vCPU pinned (control)", 9.9, "control", "load 6–7"),
    ("host-drawn baseline (11-17)", 9.09, "host", "—"),
    ("1 vCPU host-drawn (delivered)", 13.9, "host", "low load"),
    ("2 vCPU pinned, median", 15.8, "multi", "load 8–9"),
    ("2 vCPU pinned, 30 min", 15.9, "multi", "load 6–9"),
    ("4 vCPU pinned", 14.4, "multi", "load ~8"),
    ("1 vCPU + fb_hz=15", 20.0, "host", "load < 6"),
]
COLOR = {"control": BLUE, "host": ORANGE, "multi": AQUA}

fig = plt.figure(figsize=(13, 9.6), dpi=160)
fig.patch.set_facecolor(SURFACE)
gs = GridSpec(2, 2, figure=fig, height_ratios=[1, 1.05], hspace=0.42, wspace=0.18,
              left=0.07, right=0.97, top=0.80, bottom=0.07)


def style(ax):
    ax.set_facecolor(SURFACE)
    for s in ("top", "right", "left", "bottom"):
        ax.spines[s].set_visible(False)
    ax.tick_params(colors=TEXT_S, labelsize=8.5)
    ax.yaxis.grid(True, color=GRID, lw=0.8)
    ax.set_axisbelow(True)


# 1) commits per day, panic/stall subset highlighted
ax1 = fig.add_subplot(gs[0, 0])
style(ax1)
x = list(range(len(days)))
ax1.bar(x, other, color=BLUE, width=0.68, label="other commits")
ax1.bar(x, panic, bottom=other, color=ORANGE, width=0.68,
        label="panic / spinlock / stall commits")
ax1.set_xticks(x[::2])
ax1.set_xticklabels(keys[::2], rotation=0)
ax1.set_title("Commits per day", loc="left", fontsize=11.5, color=TEXT_P, fontweight="bold")
ax1.set_ylabel("commits", color=TEXT_S, fontsize=9)
ax1.set_ylim(0, 122)
ax1.legend(frameon=False, fontsize=8, loc="upper left", ncol=2, labelcolor=TEXT_P)
ax1.text(0, -0.2, "Subject-line match; a commit can be counted once.", transform=ax1.transAxes,
         fontsize=7.5, color=TEXT_S)

# 2) cumulative commits
ax2 = fig.add_subplot(gs[0, 1])
style(ax2)
cum = []
s = 0
for t in total:
    s += t
    cum.append(s)
ax2.fill_between(x, cum, color=BLUE, alpha=0.12, lw=0)
ax2.plot(x, cum, color=BLUE, lw=2.2)
ax2.scatter([x[-1]], [cum[-1]], color=BLUE, s=36, zorder=3)
ax2.annotate(f"{cum[-1]} commits", (x[-1], cum[-1]), xytext=(-6, 10), textcoords="offset points",
             ha="right", fontsize=9, color=TEXT_P)
ax2.set_xticks(x[::2])
ax2.set_xticklabels(keys[::2])
ax2.set_title("Cumulative commits on main", loc="left", fontsize=11.5, color=TEXT_P, fontweight="bold")
ax2.set_ylabel("commits", color=TEXT_S, fontsize=9)
ax2.set_ylim(0, 760)

# 3) fps by configuration (horizontal bars)
ax3 = fig.add_subplot(gs[1, :])
ax3.set_position([0.25, 0.07, 0.72, 0.30])
style(ax3)
ax3.yaxis.grid(False)
ax3.xaxis.grid(True, color=GRID, lw=0.8)
ax3.set_axisbelow(True)
names = [c[0] for c in CONFIGS][::-1]
vals = [c[1] for c in CONFIGS][::-1]
cols = [COLOR[c[2]] for c in CONFIGS][::-1]
notes = [c[3] for c in CONFIGS][::-1]
ypos = list(range(len(CONFIGS)))
ax3.barh(ypos, vals, color=cols, height=0.56, alpha=0.9)
for y, v, n in zip(ypos, vals, notes):
    ax3.text(v + 0.25, y, f"{v:g} fps   ·  {n}", va="center", fontsize=8.6, color=TEXT_P)
ax3.set_yticks(ypos)
ax3.set_yticklabels(names, fontsize=9, color=TEXT_P)
ax3.set_xlim(0, 27)
ax3.set_xlabel("frames per second", color=TEXT_S, fontsize=9)
ax3.set_title("Frame rate by configuration (values as reported; host load differs)",
              loc="left", fontsize=11.5, color=TEXT_P, fontweight="bold")
ax3.tick_params(axis="y", length=0)
legend = [Line2D([], [], marker="s", ls="", color=ORANGE, label="host draws the complete frame"),
          Line2D([], [], marker="s", ls="", color=AQUA, label="pinned multi-vCPU"),
          Line2D([], [], marker="s", ls="", color=BLUE, label="control")]
ax3.legend(handles=legend, frameon=False, fontsize=8.5, loc="upper right", labelcolor=TEXT_P)

fig.suptitle("IOSVM: development pace and frame rate", x=0.07, ha="left", fontsize=16,
             color=TEXT_P, fontweight="bold", y=0.955)
fig.text(0.07, 0.905,
         "Commit data from main (713 commits, 2026-10-31 to 2026-11-18). "
         "fps values are what the stage docs report; configurations and host load differ.",
         fontsize=9, color=TEXT_S, ha="left")

import os
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboard.png")
fig.savefig(out, facecolor=SURFACE)
print("wrote", out)
