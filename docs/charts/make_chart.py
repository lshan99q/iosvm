"""fps over commit history for IOSVM -- data transcribed from commit messages on main.

Each point is a fps value a commit explicitly reports. Configurations differ, so the
chart is grouped by measurement class and does NOT connect points into a single trend.
"""
from datetime import datetime
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates

SURFACE = "#fcfcfb"
TEXT_P = "#0b0b0b"
TEXT_S = "#52514e"
GRID = "#e4e3df"
BLUE, ORANGE, AQUA, EXCL = "#2a78d6", "#eb6834", "#1baf7a", "#8a8984"


def d(s):
    return datetime.strptime(s, "%Y-%m-%d")


# (date, fps, lo, hi, short-hash, label or None)
GUEST_PIPELINE = [  # guest-side rendering, single vCPU, no host drawing of the frame
    ("2026-11-09", 10.91, None, None, "5edef11", "10.9 · P4 local"),
    ("2026-11-10", 2.7, 2.0, 3.4, "35fc1cb", None),
    ("2026-11-10", 5.6, 5.3, 5.9, "df970e9", None),
    ("2026-11-11", 12.0, 10.0, 14.0, "f200ec2", "guest 10–14"),
    ("2026-11-14", 2.65, 2.0, 3.3, "ad8e811", None),
    ("2026-11-14", 1.80, None, None, "af3cca2", None),
    ("2026-11-14", 1.85, None, None, "af3cca2", None),
    ("2026-11-15", 7.25, None, None, "c26a61d", None),
]
HOST_DRAWN = [  # host replays the GL record and draws the complete frame
    ("2026-11-15", 7.69, None, None, "387c27f", None),
    ("2026-11-17", 9.09, None, None, "0127190", "9.1 · baseline"),
    ("2026-11-17", 13.9, None, None, "82190eb", "13.9 · delivered"),
    ("2026-11-18", 11.7, 11.7, 13.9, "d198744", None),
    ("2026-11-18", 20.0, None, None, "e65f4fa", "20 · fb_hz=15, load<6"),
]
PINNED_MULTI = [  # 2 vCPUs (or 1/4 for control) pinned to host cores
    ("2026-11-17", 9.9, None, None, "886bf99", "9.9 · 1 vCPU pinned"),
    ("2026-11-17", 14.4, None, None, "2052ddc", None),
    ("2026-11-17", 15.8, None, None, "8f81760", "15.8 · 2 vCPU"),
    ("2026-11-17", 15.6, None, None, "0e9126c", None),
    ("2026-11-17", 15.9, None, None, "cc33526", None),
    ("2026-11-17", 18.3, None, None, "0c346a5", None),
]
EXCLUDED = [  # regressions or broken pictures: shown, not trended
    ("2026-11-02", 0.26, None, None, "22fa4b8", "dead: cpus=2 spinner"),
    ("2026-11-15", 1.19, None, None, "00d7764", "async regression"),
    ("2026-11-15", 1.0, None, None, "dc3a503", None),
    ("2026-11-17", 18.3, None, None, "e5e73db", "broken picture"),
]


def plot_group(ax, rows, color, marker, hollow=False):
    xs = [d(r[0]) for r in rows]
    ys = [r[1] for r in rows]
    for r in rows:
        if r[2] is not None:
            ax.vlines(d(r[0]), r[2], r[3], color=color, lw=1.6, alpha=0.75, zorder=2)
    if marker == "x":  # unfilled marker: colour through edgecolors/linewidths only
        ax.scatter(xs, ys, s=60, marker="x", color=color, linewidths=1.8, zorder=3)
        return
    face = SURFACE if hollow else color
    ax.scatter(xs, ys, s=46, marker=marker, facecolors=face, edgecolors=color,
               linewidths=1.6, zorder=3)


fig, ax = plt.subplots(figsize=(12, 6.4), dpi=160)
fig.patch.set_facecolor(SURFACE)
ax.set_facecolor(SURFACE)

plot_group(ax, GUEST_PIPELINE, BLUE, "o")
plot_group(ax, HOST_DRAWN, ORANGE, "s")
plot_group(ax, PINNED_MULTI, AQUA, "D")
plot_group(ax, EXCLUDED, EXCL, "x", hollow=True)

# direct labels for key points (relief for the low-contrast aqua slot)
for group, color in ((GUEST_PIPELINE, BLUE), (HOST_DRAWN, ORANGE), (PINNED_MULTI, AQUA)):
    for date, fps, lo, hi, h, label in group:
        if label:
            ax.annotate(label, (d(date), (hi or fps)), xytext=(7, 6), textcoords="offset points",
                        fontsize=8.5, color=TEXT_P)

# milestone markers
for date, text in [("2026-11-17", "stage 24–25 (11-17)"), ("2026-11-18", "stage 26–27 (11-18)")]:
    ax.axvline(d(date), color=GRID, lw=1, zorder=0)

ax.set_ylim(0, 22)
ax.set_xlim(d("2026-11-01"), d("2026-11-19"))
ax.xaxis.set_major_locator(mdates.DayLocator(interval=2))
ax.xaxis.set_major_formatter(mdates.DateFormatter("%m-%d"))
ax.set_ylabel("frames per second (fps)", color=TEXT_S, fontsize=10)
ax.set_xlabel("commit date on main (2026)", color=TEXT_S, fontsize=10)
ax.tick_params(colors=TEXT_S, labelsize=9)
ax.yaxis.grid(True, color=GRID, lw=0.8)
ax.set_axisbelow(True)
for side in ("top", "right", "left", "bottom"):
    ax.spines[side].set_visible(False)

fig.suptitle("IOSVM: frame rate across the commit history", x=0.06, ha="left",
             fontsize=15, color=TEXT_P, fontweight="bold", y=0.975)
fig.text(0.06, 0.915,
         "Each point is an fps value a commit reports. Configurations differ, so points are not "
         "joined into one trend.\nVertical bars are ranges reported in the commit. Hollow × = "
         "regression or broken picture, shown but not trended.",
         fontsize=9, color=TEXT_S, ha="left", va="top")

from matplotlib.lines import Line2D
handles = [
    Line2D([], [], marker="o", ls="", color=BLUE, label="guest pipeline (single vCPU, no host draw)"),
    Line2D([], [], marker="s", ls="", color=ORANGE, label="host draws the complete frame"),
    Line2D([], [], marker="D", ls="", color=AQUA, label="pinned multi-vCPU (control and SMP=2/4)"),
    Line2D([], [], marker="x", ls="", color=EXCL, label="excluded: regression / broken"),
]
leg = ax.legend(handles=handles, loc="upper left", bbox_to_anchor=(0.0, 0.9), frameon=False,
                fontsize=9, labelcolor=TEXT_P)

fig.tight_layout(rect=(0, 0.02, 1, 0.86))
import os
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fps-history.png")
fig.savefig(out, facecolor=SURFACE)
print("wrote", out)
