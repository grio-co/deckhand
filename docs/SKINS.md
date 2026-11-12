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
`bind="deck[1].play"`: dotted lowercase segments, optional `[index]` (max 3 digits), at most 64 chars.
Controls and values the application provides (the default skin uses all of these):

    deck[N].  letter title artist bpm key tempo loop jog waveform pads load cue play sync
    mixer.    crossfader master.level headphone.volume headphone.mix
              ch[N].  trim eq.high eq.mid eq.low filter fader level cue
    browser.  tree tracks search count
    app.      controller audio import

`show="deck[1].loaded"` (prefix `!` to negate) hides a widget when false. `format` = `text int fixed0..3
time percent signed db`. A label with `text="@id"` and a `bind` formats the id with `value`/`count` = the bound
value; `args="deck=A"` adds fixed arguments (`parse_text_args`). `button text_on=` is shown while on.
`browser columns="title,artist,..."` header ids are `browser.col.<name>`.

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
`default.dhskin`: two decks, central mixer, waveforms and library, all procedural. `voidwave.dhskin`: hexagonal
window, free-form stack, orbiting/pulsing decorations. Original design; no third-party assets.
