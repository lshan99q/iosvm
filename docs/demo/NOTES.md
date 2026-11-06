# M3 acceptance capture — stage seventeen

- **Milestone**: M3 (first frame), stage 17 probe run
- **Commit**: 657f093 (the host binary the run used)
- **Guest firmware**: Darwin 22.5.0, 20F66, iPhone10,3 (`D22`); root.apfs built at 2026-11-06 13:47
- **Host**: Linux x86_64, Tesla V100-SXM2-16GB, NVIDIA 580.178.04, OpenGL 4.6 compatibility (EGL)
- **Command**: `HOST_ENV="GLWIRE_BACKDROP=1 GLWIRE_PANEL_PROBE=1 GLWIRE_PANEL_SHADOW=1 GLWIRE_SHADOW_APPLY=0" \
  tools/glwire/harness/boot-measure.sh s17probe4 /home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel/userland/root.apfs 800`
- **What it shows**: the home screen with the two widget boxes and the icon grid, taken while
  `GLWIRE_READ_PANEL=1` read the picture from the panel-sized target (the run's 298 frames never
  read back a sub-panel target, against 42.4% of s16tile's 3350 frames).
- **What it does not show**: the app labels are still absent, the widget boxes have no content, and
  the wallpaper is black. The run ended in a guest kernel panic after 298 frames (stage 17 §12).
- **Reproduced**: 1 run (the boot panicked in 5 of 6 attempts that evening, see stage 17 §12)
