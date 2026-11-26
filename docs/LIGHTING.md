# Lighting: DMX lights that follow the music

DeckHand can drive DMX fixtures (pars, strobes, UV bars, simple moving heads) from the music that is playing, with
no show to program per track. The show reads the master deck's beat grid and phrase analysis (intro, build, drop,
breakdown, ...) and decides what the lights do. **Nothing here has been tried on real fixtures or a real node yet**;
see "What is untested" at the end.

Everything is off by default. No packet leaves the machine until you enable an output.

## Setup

### 1. Outputs

| Output | Wire | Notes |
| --- | --- | --- |
| Art-Net | ArtDmx over UDP port 6454 | Set the node's IPv4 address as `host`. Answers ArtPoll (reply goes to the poller only, not broadcast) so consoles and nodes list DeckHand as a source. |
| sACN (E1.31) | UDP port 5568, multicast `239.255.x.y` or unicast | Priority 0..200, per-universe sequence numbers, optional synchronisation universe, stream termination on stop. |
| Serial widget | USB serial | "Pro" style framed widgets (macOS, Linux). "Open DMX" 250 kbaud break timing: macOS only, best effort. Not implemented on Windows. |

Up to 8 universes. Patch universe numbers are 1-based. Art-Net sends Port-Address `universe - 1 + first_universe`;
sACN sends universe `universe + universe_offset`.

The frame rate is 40 Hz by default. A frame identical to the previous one is repeated once a second (keep-alive)
instead of 40 times a second. `refresh_hz` is 1..60.

Every output has its own `enabled` flag (default `false`) and there is a master `enabled` switch above them. With
the master off, the show still renders (the preview works) but no output is opened. Art-Net refuses to open without
a destination: DeckHand never guesses a broadcast address. If you want broadcast, type a broadcast address yourself.

USB serial design: the "Pro" widgets frame a whole DMX packet (`0x7E`, label 6, length, start code, 512 slots,
`0xE7`) and generate the DMX break timing in hardware, so plain `termios` writes are enough. "Open DMX" widgets
leave the break to the host: DeckHand sets 250000 baud 8N2 (macOS accepts that rate through `cfsetspeed`), asserts
BREAK for about 120 us with `TIOCSBRK`, waits about 16 us and writes start code + 512 slots. Operating system timing
makes this jittery; prefer a framed widget or a network node. On Linux the 250 kbaud rate needs `termios2`
(`BOTHER`), which is not done, so "open" is refused there with a message. Windows serial is a stub that returns an
error.

### 2. Fixture profiles (`.dhfix`)

A profile is JSON. Channel `i` of the fixture is the `i`-th entry of `channels`.

```json
{
  "id": "my-par",
  "name": "My 8 channel par",
  "channels": [
    {"role": "dimmer"},
    {"role": "red"}, {"role": "green"}, {"role": "blue"}, {"role": "white"},
    {"role": "strobe", "off": 0, "slow": 16, "fast": 240, "hz_min": 1, "hz_max": 20},
    {"role": "macro", "default": 0},
    {"role": "other", "default": 0}
  ]
}
```

Roles: `dimmer red green blue white amber uv strobe pan pan_fine tilt tilt_fine colorwheel gobo macro speed other`.
Per channel: `default` (sent when nothing drives it), `min`/`max` (DMX range of a continuous control). `strobe`
takes `off` (no strobe), `closed` (shutter closed, used for blackout on fixtures without a dimmer), `slow`/`fast`
(DMX values at `hz_min`/`hz_max`). `colorwheel` takes `slots`: `[{"value": 10, "color": "#ff0000", "name": "red"}]`.
`pan`/`tilt` followed by their `_fine` channel are driven in 16 bit. A role (other than macro, gobo, speed, other)
may appear once per profile.

Built-in profiles: `generic-dimmer`, `generic-rgb`, `generic-rgbw`, `generic-rgba-par` (dimmer, R, G, B, A,
strobe), `generic-moving-head` (pan, pan fine, tilt, tilt fine, colour wheel, gobo, strobe, dimmer, macro),
`generic-strobe`, `generic-uv`. Your own go in the config as `profiles` (inline) or `profile_files` (paths).

How the abstract look reaches the channels: colours go to R/G/B; when the fixture has a white emitter the common
part of R, G and B moves there (RGBW), amber takes the orange part (RGBA); a fixture with a dimmer channel gets
intensity on the dimmer and the colour at full scale, one without scales the colours; a dimmer-only fixture follows
the brightest colour component; a UV fixture follows the blue part; a colour wheel picks the nearest slot.

### 3. Patch

```json
"patch": [
  {"id": "front-l", "profile": "generic-rgbw", "universe": 1, "address": 1,  "tags": ["front", "left"],  "position": 0},
  {"id": "front-r", "profile": "generic-rgbw", "universe": 1, "address": 5,  "tags": ["front", "right"], "position": 1},
  {"id": "head-1",  "profile": "generic-moving-head", "universe": 1, "address": 20, "tags": ["back"], "position": 2}
]
```

`position` orders fixtures for chases and fans (low to high, left to right is a good habit). Tags `front back left
right floor` are used by the scenes (fixtures tagged `back` pulse on the off-beat); other tags are kept. Validation
rejects unknown profiles, duplicate ids, universes outside 1..32767, addresses outside 1..512, fixtures running past
channel 512, overlapping channels in a universe and more than 8 universes. A rejected patch leaves the previous
configuration running.

## How the automatic show decides

Input per frame: beat position (so beat phase and bar position), tempo, the section the master deck is in (label and
energy 1-10 from the phrase analysis), time to the next section boundary, playing or stopped, and optionally the
master level and low/mid/high levels.

**Scenes** (each maps abstract colour and intensity to whatever channels a fixture has):

| Scene | What it does |
| --- | --- |
| wash | static colour |
| pulse | brightest exactly on the beat, decaying over the beat; accent on the bar's downbeat; `back` fixtures on the off-beat |
| chase | a lit spot runs across the fixtures in `position` order once per bar |
| colour_cycle | colour glides through the palette, one palette step per 8 bars, offset across fixtures |
| build_ramp | chase that speeds up toward the boundary, brighter as it goes, strobe in the last fifth (capped) |
| calm | slow fades in cool colours, ignores the beat |
| blackout | off |

Movement patterns for pan/tilt fixtures run beside the scene: `circle`, `tilt_sweep`, `pan_fan` (phase locked to the
bar). A blinder hit (white, one beat, short strobe burst) lands on drops and high-energy choruses.

**The director** chooses on every section change: intro = wash or soft pulse; build = build_ramp; drop/chorus =
pulse, chase or colour_cycle plus the blinder hit; verse = pulse or colour_cycle; breakdown/bridge = calm; outro =
wash. Intensity follows the section energy (0.37 at energy 1 up to 1.0 at 10). Variety is a pure function of the
`seed`, the section type and the running count of sections: the same seed on the same track gives the same show
every time, a different seed gives a different one.

**Timing.** Scene changes happen on section boundaries, early enough to land on them. Hard hits (drops) cut exactly
on the boundary; soft changes start half a crossfade (default 1.5 s) before it so the new look is in place at the
boundary. `latency_ms` shifts the entire show earlier to compensate for network, node and fixture delay: set it
until a drop looks in time, typical values 20-120 ms. States arrive at the application's tick rate; the show
extrapolates from the last one with the tempo, so beats stay sharp between ticks.

**Palettes.** `user`: your list of up to 8 colours. `track`: from the track's colour tag (hue, plus its neighbour,
complement and other neighbour); when the track has no usable colour, from its key. `key`: Camelot number to hue
(8 = 210 degrees, one step = 30 degrees), minor keys deeper than major. All deterministic. Breakdowns pull
whatever palette is active toward blue.

## Safety

* **Master dimmer** (0..1), **blackout** (immediate), **hold look** (freezes colour and position; strobe stops).
* **Strobe**: hard cap, default 10 Hz, configurable 0..25 and never above refresh/4 (so a 40 Hz refresh cannot
  flash faster than 10 Hz even in software). `strobe_enabled: false` removes every strobe from every scene and
  override: use it for photosensitive audiences. The cap and the switch are the last stage of the render path,
  nothing bypasses them, including manual overrides and fixtures' own strobe channels (those are driven at the
  capped rate; a rate below the fixture's slowest means "no strobe"). Fixtures without a strobe channel are flashed
  by gating their output at the same capped rate.
  Note that photosensitivity guidance for public events is usually stricter than 10 Hz (3 flashes per second is a
  common limit): lower `strobe_max_hz` or disable strobes if in doubt.
* **Stop = dark**: when the master deck stops the lights fade to black (0.6 s). If no music state arrives for 2 s
  (the application froze) the show treats it as stopped. Stopping the engine sends one dark frame and, for sACN,
  stream-termination packets.
* **Manual override**: set a scene, movement, colour, intensity or strobe by hand; the automatic show keeps running
  underneath and takes over again on release.

## Configuration file

`LightingConfig` is JSON (`config_to_json` / `config_from_json` / `save_config_file`, atomic replace). Sections:
`enabled`, `refresh_hz`, `keepalive_s`, `show` (`master strobe_enabled strobe_max_hz latency_ms crossfade_s
stop_fade_s stale_s seed auto_movement palette{mode,colours}`), `outputs{artnet,sacn,serial}`, `patch`, `profiles`,
`profile_files`. Out-of-range numbers are clamped.

## In the application

`src/app/lighting_bridge.*` owns the `LightingEngine`, reads and writes `lighting.json` in the data folder and feeds the
show from the deck on air (the same pick as the suggestions: the loudest playing deck): beat position and phrase
sections from the track's stored grid and phrase analysis (loaded into a `TrackTimeline` when the track loads), the
tempo including pitch, the track colour tag and key, and the deck's output level (the engine reports one level per deck,
not bands: low, mid and high all follow it). Nothing runs until `enabled` is true, and nothing leaves the machine until
an output is enabled with an address. Where to find it:

* Tools panel, tab "Lights": run the show, blackout, hold, "No strobes", master, Art-Net and sACN switches with the node
  address and universe, and a row of preview swatches. Everything else (patch, profiles, palette, latency, strobe cap)
  is in the configuration: `GET`/`PUT /v1/lighting/config` (admin) or `lighting.json`.
* Controls and shortcuts: `lighting.blackout`, `lighting.hold`, `lighting.master`, `lighting.strobe` (0 = no strobes),
  `lighting.enable`. They have no default key.
* REST: `GET /v1/lighting/status`, `GET`/`PUT /v1/lighting/config`, `POST /v1/lighting/command`, `GET /v1/lighting/preview`.
* A build with `-DDECKHAND_WITH_LIGHTING=OFF` compiles the same bridge as a stub: the routes answer `available: false`
  (config and command: 404 / 400) and the controls refuse.

## Tests

`test_lighting` (packet encoders byte for byte against the specification layouts, profiles, patch, mapping, config,
pacing with an injected clock), `test_lighting_net` (UDP on 127.0.0.1 with ephemeral ports: Art-Net, ArtPoll reply,
sACN unicast, sequence wrap, outputs off by default), `test_lighting_show` (scripted track: beats, boundary timing
with latency, strobe cap, stop/blackout/hold/master/override, seed determinism, palettes, and a counting-`new`
check that the steady-state render path allocates nothing).

## What is untested

No real fixture, DMX node, console or USB widget has received a packet from this code. Everything above was checked
against the protocol layouts and over loopback. Specifically unverified: that a given node accepts the Art-Net and
sACN frames and shows the universes where expected (universe numbering conventions differ between products: use
`first_universe` / `universe_offset`), the ArtPollReply fields as seen by real consoles, sACN multicast routing and
interface choice on a multi-homed machine, serial widgets of any kind, the colour mapping on real LEDs (the amber
and white extraction is generic), strobe channel ranges of real fixtures (profiles are generic), and how the scenes
look in a room. Windows and Linux builds are only compiled in CI; the Winsock path has not been run.
