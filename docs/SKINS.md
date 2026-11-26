# DeckHand skins

Goal: Winamp-style total themeability, up to exotic layouts and effects, with no scripting.
Code: `src/skin/` (parser, validator, model, layout solver); shipped skins in `resources/skins/`.

## Package (`.dhskin`)
A `.dhskin` is a single text file (procedural skins: shapes, tokens, animations; no external files) or a
directory containing `skin.dhs` plus `img/` and `fonts/`. (A zip of such a directory is a distribution
format for the installer to unpack; the core only reads directories and files.) All referenced paths are
skin-relative: `..`, absolute paths, backslashes and drive letters are rejected, and `load_skin` also
refuses symlinks leaving the skin directory. Shaders and sounds are not implemented yet.

## Syntax
Line-oriented, brace blocks, `//` comments: `name arg... key=value... { children }`. Strings in quotes.
Colours `#rgb #rrggbb #rrggbbaa` or a token name. Lengths: `120` (logical points), `50%` (of parent
content), `fill`, `auto`. No expressions, loops or functions. Templates give reuse:

    template deck n color { column { label bind="deck[${n}].title" color=${color} } }
    ... use deck n=1 color=deck1

Templates expand textually; cycles, unknown/missing parameters and expansion bombs are errors.

## Sections (after `dhskin 1`)
    meta name=... author=... license=... min_version=... description=...
    tokens { color NAME #hex | number NAME 4 | font NAME family=.. file=.. size=12 bold=true }
    shape NAME { polygon 0,0 1,0 1,1 0,1 | ellipse | roundrect radius=24 | chamfer size=16 }
    atlas NAME src="img/a.png" { region NAME x y w h [slice=l,t,r,b] }   // slice = nine-slice insets
    animation NAME property=opacity|scale|offset_x|offset_y|rotate|glow from= to= duration=ms delay=ms
                   ease=linear|in|out|inout loop=once|repeat|pingpong trigger=always|hover|pressed|on|beat
    strings [lang=de] { skin.id "text" }                                // per-skin strings
    window ID size=WxH min=WxH shape=NAME title="@app.title" { <one root node> }

## Layout nodes
Containers: `row`, `column` (flow), `stack` (children anchored, default centre), `free` (anchored, default
top-left), `grid cols= rows=` (equal cells, `col row colspan rowspan` per child), `panel` (stack).
Widgets: `waveform jog button knob fader meter label browser tree pads image spacer`. Unknown widgets and
unknown properties are **warnings** (forward compatibility); the node stays in the layout as `Kind::Unknown`.

Properties: size `w h min_w min_h max_w max_h flex aspect`; spacing `pad margin` (1, 2 = h,v, or 4 values
l,t,r,b) `gap`; placement `align=stretch|start|center|end` `justify=start|center|end|between`
`anchor=fill|tl|t|tr|l|c|r|bl|b|br` `x y`; `rotate`; style `color bg border border_w radius opacity scale
font font_size text_align sprite=atlas:region glow glow_r`; behaviour `id bind bind2 text format show
tooltip anim=a,b transition=ms shape`; widget extras `orientation bipolar cols rows count mode columns row_h
text_on args editable ...`. `state hover|pressed|on|disabled|normal { style props }` overrides style per state.
Flow rule: fixed/percent children first (shrunk proportionally if they exceed the space), remainder split by
`flex` (default 1) honouring min/max. Everything is clamped to the window and to its parent.

## Bindings
`bind="deck[1].play"`: dotted lowercase segments, optional `[index]` (1-based, max 3 digits), at most 64 chars.
Skin names are the public API; the application translates them to its control table (`deck1.play`,
`deck2.eq_high`, `mixer.head_level`...) or to its own actions in ONE place, `map_binding()` in
`src/app/skin_view.cpp`. Unknown bindings are harmless: the widget draws but does nothing.

    deck[N].  letter title artist bpm key time elapsed remaining loop loaded   (labels, `show`)
              play cue sync key_lock quantize slip reverse eject               (buttons; cue acts on press and release)
              tempo (fader, -1..1)  tempo_range (button: steps 6/10/16/100 %, label shows the range)
              key_shift (label)  key_shift.minus .plus .reset (buttons)
              fx[S].select (button: steps through the effects, shows the name)  fx[S].on  fx[S].wet (fader)
              jog  waveform (mode=zoomed|overview; overview seeks on click/drag)  pads (hot cues, cols= rows=)
              load (button: loads the selected library track)
              metronome (button: click on every grid beat, headphones only)
              grid.edit (button + `show=`: edit mode, the waveform draws the grid heavily)  grid.bpm (label)  grid.lock (button)
              grid.shift_m10 .shift_m1 .shift_p1 .shift_p10 (ms)  grid.first_beat .downbeat .cycle  grid.bpm_m01 .bpm_m001 .bpm_p001 .bpm_p01
              grid.half .double .tap .reanalyse .undo .add_section .remove_section (buttons; they edit the loaded track's grid)
              cue.memory_set .memory_delete .memory_prev .memory_next .loop_save .loop_recall (buttons)
    mixer.    crossfader master.level (meter) master.volume headphone.volume headphone.mix sampler.level
              ch[N].  trim eq.high eq.mid eq.low filter fader (knobs, faders)  level (meter)  cue (headphone cue)
              metronome.level (fader)
    browser.  tree tracks search (editable label) count add_to remove edit can_remove (show)
              columns (button: the column editor)  compatible (button: only tracks that mix with the loaded one)
              playlist.add_list .add_folder .add_smart .rename .delete
    preview.  play stop (buttons)  wave (waveform: click or drag seeks)  title time (labels)  level (fader)
    sampler.  pads (16 slots: click plays, shift-click or the corner square stops)  level  stop
    app.      controller audio status (labels)  import rec export language skin layout settings sampler learn (buttons)
              has_import multi_language multi_skin has_four (for `show=`)

`show="deck[1].loaded"` (prefix `!` to negate) hides a widget and its children when false (the layout space
is given back in rows, columns and grids; elsewhere it is just not drawn). `format` = `text int fixed0..3 time percent signed db`. A label with `text="@id"` and a
`bind` formats the id with `value`/`count` = the bound value; `args="deck=A"` adds fixed arguments
(`parse_text_args`). `button text_on=` is shown while on. A button or label with a `bind` but no `text` shows
the application's own text for it (REC with its clock, the language tag, the skin name, an effect name).
`browser columns="title,artist,..."` header ids are `browser.col.<name>`; known columns are title, artist, album,
genre, bpm, key, time, rating, comment, added, plays, played, color, format, bitrate. The list in the skin is the default:
the user's own choice, order and widths (the browser's `Columns...` panel and dragging a column edge) are saved in the
library settings `browser.columns` and `browser.widths`. A skin whose layout has no `grid.edit` button still works; edit
mode can be switched on through the API. The scrolling waveform zooms with the mouse wheel and its corner buttons (the
application draws those); the zoom is remembered per deck (`ui.wave_zoom.N`).

## Windows
A skin may declare up to twelve windows. The application uses `main` (the first) and, when present, `four`: the
`app.layout` button (shown with `show="app.has_four"`) toggles between them, persisted as setting `ui.layout`.
`four` is expected to bind decks 1-4.

## Free-form windows and modules
A skin can draw a window of any shape (a skull, two discs ...) with every empty pixel transparent to the desktop, and put the
big parts (library, sampler, settings ...) in separate windows that float independently. Everything is optional: a skin
without any of it (default, voidwave) is one opaque rectangular window exactly as before. Example: `orbit.dhskin/`
(art by `tools/make_orbit_art.py`). Shells: `docs/SHELL-WINDOWS.md`.

    window main size=1000x560 chrome=none background=transparent mask="img/main.png" mask_threshold=128
           scale=fit scale_min=50 scale_max=200 snap=true {
      resize { ellipse cx=880 cy=455 rx=24 ry=24 }          // grip: drag = uniform scale
      snap_point 500,556
      free sprite="main:full" { ... }
    }
    window browser role=browser size=760x300 chrome=none background=transparent shape=capsule_shape scale=fit
           snap=true visible=true place="main below 120 -12" { snap_point 380,2  free { ... } }
    window sampler role=sampler size=300x300 ... floating=true { drag { ellipse ... ellipse ... op=sub }  ... }
    button action="window.toggle:browser" text="@window.browser"

Window properties: `role` (main, browser, sampler, effects, mixer, settings, deck3_4 or a free name; default = the id; one window
per role; `four` is a layout of main, not a module); `chrome=none|system` (default system); `background=transparent|opaque`;
`mask="img/x.png"` (opaque where alpha >= `mask_threshold`, 1..255, default 128; may be the art itself) or `shape=NAME`;
`scale=fit` with `scale_min/scale_max/scale_default` in percent (50..300 typical; the layout is solved at the declared size and drawn
uniformly scaled) or `scale=none` with `min= max=` for free resizing; `aspect=lock|free`; `resizable`; `visible` (at start; main is
always shown, modules default hidden); `floating` (always on top); `snap`; `shadow=auto|none`; `place="REF over|below|above|left|right [dx [dy]]"`
(where a module first opens, relative to a window id or role). Window children besides the one root node: `drag { regions }`
(only these areas move the window; default: any opaque pixel that is not a control; opaque pixels elsewhere are inert),
`resize { regions }` or `resize handle=tl|tr|bl|br size=24`, `snap_point x,y` (up to 32).

Vector regions (`shape NAME { ... }`, `drag`, `resize`), in points of the declared window size, applied in order, `op=sub` cuts a hole:
`rect x= y= w= h= [radius=]`, `ellipse cx= cy= rx= ry=`, `polygon x,y x,y ... units=px`, `path d="M x y L x y H x V y Q cx cy x y C ... Z"`
(absolute upper-case commands, curves flattened). The classic single whole-window `polygon 0..1 / ellipse / roundrect / chamfer` form still works.

Hit testing (what a click does): control > resize region > drag. Outside the shape (mask alpha below threshold, or a hole) the click falls
through to whatever is behind the app. A transparent window with neither mask nor shape counts what is painted (fills, sprite pixels) as
opaque. Controls overhanging the art still work.

Modules: `action="window.toggle:ROLE"` / `window.show:ROLE` / `window.hide:ROLE` on any button (on-state = shown), `window.reset` (arrangement back
to the skin's defaults). `show="app.window.ROLE"` is true while that module is shown, `show="app.multi_window"` while the shell can show several windows
(the default skin's "Pop out" button and the docked library use them: the library moves into its own window and the main layout reflows).
Which role hosts what: the widgets in a window's layout (`browser`, `tree`, `pads` ...). The app's panels (roles `settings`, `sampler`, `learn`,
`tags`, `relink`, `columns`, `smart`, `cue`, `export`, `add_to`) open as their own default window; a skin window of the same role hosts the
panel instead, drawn into its node with `id=content` (or the whole window). A skin that declares a `sampler` window gets it from `app.sampler`.
Menus (`export`, `add_to`) open as pop-ups hanging from the button that opened them and close when they lose focus; they are never clipped by a shape.
Position, visibility, scale, size, stacking and docked-to are saved per skin (settings `ui.win.<skin>.<window>`) and restored; Window menu / `window.reset` resets.

Snapping: a `snap=true` window dragged within 12 points of another's edge (docking or lining up) or `snap_point` snaps; docked windows move as
a group when the one they are docked to is dragged; shift-drag moves one window alone and detaches it.

Curved controls: `path bind=... d="M ..." viewbox=WxH thickness= handle=` (a slider whose handle travels along the curve; the value is the
nearest arc-length position, pressing anywhere on the track jumps there) and `arc bind=... start=135 end=405 thickness=` (a curved meter, degrees
clockwise from +x; `mode=db|linear`). `rotate` turns sprites and the hit area of any widget; `hit=alpha [hit_threshold=16]` makes an image
button hit only where its sprite is visible.

Single-window fallback: a shell without multi-window support gets the visible modules docked beside the main window (per `place`) over the solid
theme backdrop, shrunk uniformly to fit; shapes are ignored. Images are bled into transparent texels at load so anti-aliased edges never show dark fringes.
Limits: 12 windows, 64 region primitives per list, 32 snap points, 1024 path points, masks up to 4096 px per side / 16 Mpx.
Debug: `GET /v1/debug/windows` and `GET /v1/debug/hit?window=&x=&y=` (admin).

## Interaction
Knobs: vertical drag, shift = fine, double-click = default. Faders: drag (a press off the thumb jumps there,
then the drag is relative), shift = fine, double-click = default. With the shell's `touch` flag buttons, knobs and
faders get at least 44 pt hit targets. Safe-area insets given to `dh_app_set_display` shrink the solved window.
`transition=ms` tweens colours between states; `anim=` animations run (`opacity scale offset_x offset_y glow`;
triggers `always hover pressed on beat`, where `beat` is the time since the last beat of the bound deck).
`sprite=atlas:region` is drawn on any widget (stretched, or nine-slice when the region has `slice=`), and an
`image` widget is just a sprite; atlas PNGs (8-bit, non-interlaced, RGBA/RGB/grey/palette) load from skin
directories only. `rotate` (the property and `rotate` animations, degrees about the centre) turns sprites, not
other widgets; an `image` with a `bind` follows that toggle, so `trigger=on` spins it while the deck plays
(see `voidwave.dhskin/`, art generated by `tools/make_voidwave_atlas.py`). The legacy window `shape=` (polygon / ellipse /
roundrect / chamfer) only clips hit testing inside the classic rectangular window; free-form windows are below. Panels the application draws itself
(settings, sampler, MIDI learn, track details, export destinations, add-to-playlist) use the skin's colour tokens
by name: `bg panel panel2 line text dim accent bad good hover deck1..deck4` (with fallbacks, e.g. voidwave's
`void glass haze rim ink violet cyan magenta`). A skin that cannot be loaded is reported in the status line and
the previous skin stays; if even the built-in one fails the app falls back to a minimal code-drawn UI.
Skins are chosen with the `app.skin` button (cycles built-in skins and those in `<data>/skins`), or in
Settings; the choice is setting `ui.skin` and applies without a restart.

## Text and i18n
Skins never hardcode UI text: `text="@deck.sync"` resolves via `resolve_text()` - the skin's own `strings`
(per language, then default) first, else the i18n catalog. Plain text without `@` is literal (discouraged).

## Layout solver
`solve_layout(skin, window_index, width_px, height_px, scale)` -> flat `Layout`: widgets in paint order with
rect (physical px), binding, text spec, merged per-state style (lengths pre-multiplied by scale), animation
indices, parent index and `parent_is_flow`; plus the window outline polygon (`Layout::shape`, empty =
rectangle). `hit_test()` returns the topmost interactive widget and honours the outline. The app draws
through its gfx draw list; animations are evaluated with `animation_value()`.

## Safety
Hard limits (`skin::limits`): 1 MiB skin file, 8 MiB per asset, 24 nesting levels, 20000 statements, 4096
layout nodes, 512 tokens, 16 atlases, 1024 regions, 64 animations, 4 windows, 256 shape points, window
16384 px max. No scripting, no network, no file access outside the skin directory. Any failure yields no
skin (`ParseResult::skin == nullptr` plus messages); the app then keeps the built-in skin. The skin runs in
the UI process only and can never affect audio.

## Built-in skins
`default.dhskin`: `main` = two decks (transport, key lock/quantise/slip/reverse, key shift, tempo, three effect
slots, hot-cue pads, overview) around a central mixer, zoomed waveforms, playlist tree and track table with
every library action; `four` = four compact decks with a four-strip mixer. All procedural. `voidwave.dhskin`:
hexagonal window, free-form stack, orbiting/pulsing decorations; exposes fewer controls (plus the skin and settings
buttons). Original design; no third-party assets.
