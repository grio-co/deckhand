# Shells and free-form windows

Contract: `src/app/include/deckhand/app/capi.h` (section "Free-form and multi-window skins"). The macOS shell
(`platform/macos/Shell/main.swift`) is the reference. Linux and Windows do **not** implement this yet; until they call
`dh_app_set_capabilities` the core draws everything into the one rectangular window via `dh_app_frame()` (visible modules
docked beside the main window over a solid backdrop, shapes ignored). Nothing else in those shells changes.

## Frame loop
`dh_app_set_capabilities(DH_CAP_MULTI_WINDOW | DH_CAP_TRANSPARENT | DH_CAP_CLICK_THROUGH)` once; then per tick:
`dh_app_begin_frame` -> `dh_app_windows` (create / destroy / show / hide / move native windows; a hidden module is listed so a Window
menu can offer it) -> per visible, unoccluded window `dh_app_window_display(px size, backing scale)` + `dh_app_window_frame` ->
`dh_app_end_frame` (apply its uploads/releases once) -> render each window's frame. One texture namespace (id 1 glyph atlas, 2+ skin
images): create each texture once, bind it from any window; do not copy atlases per window. Per-window display scale is supported
(windows on monitors with different scale factors). Skip rendering of hidden/occluded windows. Vertices are straight alpha; with the blend
(src.a, 1-src.a colour; 1, 1-src.a alpha) the target is premultiplied, which non-opaque surfaces expect. Clear to `clear_rgba` (alpha 0
for `DH_WIN_TRANSPARENT`). Window positions are desktop points, origin top-left of the primary display, y down.

## Input and hit testing
Pointer and scroll go to `dh_app_window_pointer/scroll` in window pixels. Keys, text and modifiers stay global
(`dh_app_key/text/modifiers`). Drops: `dh_app_window_open_file`. For `DH_WIN_SHAPED` windows call `dh_app_hit_test` on every mouse move:
`NONE` -> the click must reach whatever is behind; `DRAG`/`RESIZE` -> do not send a pointer event, run `dh_app_window_drag`
(BEGIN with the pointer's desktop position, UPDATE on moves, END on release; the core snaps, moves docked groups, scales, and you apply the
new frames from `dh_app_windows` at once); `CONTROL` -> pointer events; `SOLID` -> swallow. Report `dh_app_window_event`: MOVED, RESIZED,
FOCUS (a pop-up closes when it loses focus), CLOSE (user closed: the core hides the module, main quits), MINIMIZED. Menu: `dh_app_window_set_visible`,
`dh_app_windows_reset`. Apply `move_seq` changes (position/size), clamp to existing screens, re-clamp when a monitor disappears.

## macOS (done)
Borderless `NSWindow` (`isOpaque=false`, clear background, `CAMetalLayer.isOpaque=false`), shadow follows the alpha (`invalidateShadow`) or
`DH_WIN_NO_SHADOW`; click-through by toggling `ignoresMouseEvents` from the hit test on mouse-moved via both a local and a global event monitor
(works while the app is inactive; never flips while a button is down); `acceptsFirstMouse`; `mouseDownCanMoveWindow=false`; main window in
Dock / window list, modules `fullScreenAuxiliary`, level `.floating` for `floating`; a hidden window is ordered out only after the mouse button is up.
`DECKHAND_SINGLE_WINDOW=1` runs the classic one-window path; `DECKHAND_FPS=1` logs frame rate.

## Windows (to do)
Layered or DirectComposition windows with per-pixel alpha (`WS_EX_NOREDIRECTIONBITMAP` + DXGI flip swap chain with `DXGI_ALPHA_MODE_PREMULTIPLIED`,
or `UpdateLayeredWindow` with a premultiplied bitmap); `WM_NCHITTEST` answers `HTTRANSPARENT` for `NONE`, `HTCAPTION` for `DRAG` (then the OS drags, but
snapping/groups need the core drag: prefer `HTCLIENT` and run `dh_app_window_drag` from mouse messages with `SetCapture`), `HTBOTTOMRIGHT`-style for RESIZE;
`WS_EX_TOOLWINDOW`/owner windows for modules (no taskbar entry), `WS_EX_TOPMOST` for floating; per-monitor DPI via `WM_DPICHANGED`; `DwmExtendFrameIntoClientArea` is
not needed for borderless popups; clamp with `MonitorFromRect`.

## Linux (to do)
SDL3: `SDL_WINDOW_TRANSPARENT | SDL_WINDOW_BORDERLESS`, one `SDL_Window` per core window with a shared GL/Vulkan context (shared textures) and
premultiplied blending; click-through: `SDL_SetWindowShape` where supported, else X11 XShape input region (`XShapeCombineMask(ShapeInput)`) built from sampling
`dh_app_hit_test` into a bitmap when the size changes, Wayland `wl_surface.set_input_region`; dragging via the core (do not use compositor move). Needs a compositing window manager
(X11: an ARGB visual + compositor; Wayland: always). Without one, do not announce `DH_CAP_TRANSPARENT` (the core still composes one window). Always-on-top is
a hint on Wayland (not guaranteed); window positions are not settable on Wayland, so announce `DH_CAP_MULTI_WINDOW` there only with xdg-positioner/XWayland.
