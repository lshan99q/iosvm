# SPDX-License-Identifier: MIT
"""Metal -> Vulkan translation: frame rate by implementation, stage 29 (experiment branch).

Top: the translated client's frame rate as the picture's route to the host and the guest's checksum
changed. The points are the runs in docs/stage29-metal-translation.md section 5.4, one synthetic
Metal client (750x1624 BGRA8, 4.87 MB a frame), one host, one vCPU, Vulkan device 1.

Bottom: the pair with the Replay control measured in the same session (section 5.6), same host,
same picture size. The Replay path's rate depends on what the screen is doing (a static home
screen submits no frames at all), so it is drawn as the measured windows plus the delivered 13.9 fps
reference line. The two rows are not the same workload and the caption says so.

Regenerate: python3 docs/charts/metal-translation-history.py
"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SURFACE = "#fcfcfb"
TEXT_P = "#0b0b0b"
TEXT_S = "#52514e"
GRID = "#e4e3df"
BLUE, ORANGE, AQUA, EXCL = "#2a78d6", "#eb6834", "#1baf7a", "#8a8984"

# (label, fps, low, high, colour, note)
ROWS = [
    ("picture inline on the socket,\nfull byte-wise CRC", 1.94, None, None, EXCL,
     "4.87 MB a frame through TCP/TCG; 490 ms a frame"),
    ("by-reference upload,\nfull byte-wise CRC", 16.4, None, None, ORANGE,
     "hakios_vtop + the host maps QEMU's RAM; the checksum is now 48 ms a frame"),
    ("by-reference,\nslice-by-8 CRC", 27.0, None, None, ORANGE,
     "same CRC, an eighth of the loop"),
    ("by-reference,\nsampled CRC (1 row in 8)", 58.3, 57.4, 60.1, AQUA,
     "five runs, 2,000 frames, 2,000/2,000 crc_ok; the reported median"),
]
REPLAY_DELIVERED = 13.9
REPLAY_WINDOWS = [  # (fps, what the screen was doing)
    (12.4, "boot animations settling"),
    (15.6, "host-driven swipes, window 1"),
    (33.6, "host-driven swipes, window 2"),
    (65.2, "host-driven swipes, window 3"),
    (63.9, "host-driven swipes, window 4"),
]


def top(ax):
    labels = [row[0] for row in ROWS]
    values = [row[1] for row in ROWS]
    colors = [row[4] for row in ROWS]
    positions = list(range(len(ROWS)))
    ax.barh(positions, values, color=colors, height=0.55, zorder=3)
    for row, value, position in zip(ROWS, values, positions):
        _, fps, low, high, _, note = row
        text = "%.1f fps" % fps if low is None else "%.1f fps (%.1f–%.1f)" % (fps, low, high)
        ax.text(value + 1.2, position, text, va="center", fontsize=9.5, color=TEXT_P, zorder=4)
        ax.text(1.0, position - 0.36, note, va="center", fontsize=7.4, color=TEXT_S, zorder=4)
    ax.set_yticks(positions)
    ax.set_yticklabels(labels, fontsize=9, color=TEXT_P)
    ax.invert_yaxis()
    ax.set_xlim(0, 74)
    ax.tick_params(axis="x", colors=TEXT_S, labelsize=7.5)
    ax.tick_params(axis="y", length=0)
    ax.grid(axis="x", color=GRID, linewidth=0.8, zorder=0)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(GRID)
    ax.set_title("translated client — what changed and what it cost", fontsize=9.5,
                 color=TEXT_P, loc="left")


def bottom(ax):
    ax.set_xlim(0, 74)
    ax.set_ylim(-1.15, 1.45)
    # translated client: bar + range
    ax.barh(0.75, 58.3, color=AQUA, height=0.28, zorder=3)
    ax.plot([57.4, 60.1], [0.75, 0.75], color=TEXT_P, linewidth=1.0, zorder=4)
    ax.text(59.6, 0.75, "58.3 fps median (57.4–60.1)", va="center", fontsize=8.5, color=TEXT_P)
    # replay windows: scatter on a row, labelled
    for fps, what in REPLAY_WINDOWS:
        ax.plot(fps, 0.05, marker="o", markersize=5, color=ORANGE, zorder=4)
    ax.plot([0], [0.05], marker="x", markersize=5, color=EXCL, zorder=4)
    ax.text(2.0, -0.72, "Replay control: 0 fps idle, 12.4–65.2 fps while the screen is drawing "
            "(host-driven swipes;\nthe rate depends on what is animating; the delivered 13.9 fps "
            "is a home-screen figure)",
            fontsize=7.4, color=TEXT_S)
    for fps, what in REPLAY_WINDOWS:
        ax.text(fps, 0.22, "%.0f" % fps, fontsize=6.8, color=ORANGE, ha="center")
    ax.axvline(REPLAY_DELIVERED, color=BLUE, linestyle="--", linewidth=1.2, zorder=2)
    ax.text(REPLAY_DELIVERED + 0.8, 1.2,
            "delivered Replay baseline: 13.9 fps (home screen, 2026-11-17)", fontsize=7.8,
            color=BLUE)
    ax.set_yticks([0.75, 0.05])
    ax.set_yticklabels(["translated\nclient", "Replay\ncontrol"], fontsize=8.5, color=TEXT_P)
    ax.tick_params(axis="x", colors=TEXT_S, labelsize=7.5)
    ax.tick_params(axis="y", length=0)
    ax.grid(axis="x", color=GRID, linewidth=0.8, zorder=0)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(GRID)
    ax.set_title("the pair — same host, same 750×1624 picture size, not the same workload",
                 fontsize=9.5, color=TEXT_P, loc="left")
    ax.set_xlabel("frames per second", fontsize=8.5, color=TEXT_S)


def main():
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10.0, 7.4), facecolor=SURFACE,
                                   gridspec_kw={"height_ratios": [2.2, 1.0]})
    for ax in (ax1, ax2):
        ax.set_facecolor(SURFACE)
    top(ax1)
    bottom(ax2)
    fig.suptitle("Metal → Vulkan translation: the cost was the transport and the checksum, not the translation",
                 fontsize=11, color=TEXT_P, x=0.012, ha="left")
    fig.text(0.012, 0.005,
             "one vCPU · Vulkan device 1 (Tesla V100, PCI 0000:05:00.0) · QEMU TCG · 2026-11-21/22 · "
             "stage 29, branch experiment/metal-translation · docs/stage29-metal-translation.md",
             fontsize=6.8, color=TEXT_S)
    fig.tight_layout(rect=(0, 0.025, 1, 0.955))
    out = __file__.replace(".py", ".png")
    fig.savefig(out, dpi=160, facecolor=SURFACE)
    print("wrote %s" % out)


if __name__ == "__main__":
    main()
