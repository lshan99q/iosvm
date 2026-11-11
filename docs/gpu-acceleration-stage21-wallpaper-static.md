# Wallpaper static fallback: not viable

The homepage wallpaper is a **PosterBoard-hosted live scene**, not a SpringBoard bitmap.
`userland/src/inject_sb.c:972` states it: PosterBoard draws it in another process and hosts it
into SpringBoard's layer tree; SpringBoard has no picture, and `/var/mobile/Library` has no
PaperBoard/PosterBoard/Wallpaper data after minutes.  `inject_sb.c:1976` only finds
`_SBWallpaperSecureWindow` as a root view for widget placeholders; it does not own the wallpaper
contents.  The P2 attempts (PosterBoard running, PBUI remote VC, layer tree) ended with
`contents=0` and extension check-in timeout (docs/gpu-acceleration-stage19-20-final-handoff.md:28).

Conclusion: **SpringBoard-side static image workaround is not成立**; a static image would sit under
the widgets but would not replace the hosted wallpaper window, and no provider exists to attach it.
The only real path is the PosterBoard/extension scene (P2), which is closed.
