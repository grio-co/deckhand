# Plug-in hosting (src/plugins)

Hosts third-party audio effect plug-ins so they can sit in DeckHand's effect chains. Namespace `deckhand::plugins`,
option `DECKHAND_WITH_PLUGINS` (ON on macOS, otherwise a stub with the same interface). System frameworks only
(AudioToolbox, CoreFoundation, CoreAudio). This round: macOS Audio Units (effects: `aufx`, `aumf`). No VST3 code yet.

## Pieces
- `plugin.hpp`: `PluginInfo`, `ParamInfo`, `PluginInstance` (format neutral), `scan_installed()`, `open_plugin()`.
  A VST3 backend adds a `Format` value, a scan function and an open function behind those two calls (`plugin.cpp`).
- `catalog.hpp`: `PluginCatalog`: scan cache (text file, version stamp = cache version + OS build + installed unit count)
  and a persistent blocklist (`<cache>.blocklist`). `refresh()` loads the cache or rescans. Scanning only reads
  component metadata (name, maker, codes, version); nothing is instantiated.
- `probe.hpp`: `probe_plugin()` (open, render one block, close), `probe_out_of_process()` (runs the
  `deckhand-plugin-probe` helper with a deadline, reports crash / timeout), `probe_catalog()` (probe all, block failures).
- `effect.hpp`: `PluginEffect`, the chain-unit adapter.
- Backend: `au.cpp` (all Audio Unit code), `backend.hpp` (private).

## Instance
`open_plugin(info, sample_rate, max_block, &err)` gives stereo in / stereo out (non-interleaved float).
`process(in, out, frames)`: in and out may alias; any frame count (split internally at `max_block`); no allocation and
no locks; a render error passes the input through and bumps `render_errors()`. `set_bypass` is a hard bypass (copy; the
unit is not run). `tail_seconds()`, `latency_seconds()/latency_frames()` are reported only: the host does not delay-compensate.
`reset()` clears delay lines and tails (not while `process` runs).

## Parameters
`params()` lists id, name, unit, min/max/default, `steps` (discrete count, 0 = continuous), `value_strings`, read-only,
logarithmic flag. `set_plain/set_norm` are lock free from any thread: the value is queued and applied at the start of the
next `process()` (or `flush_params()`); `get_*` returns the queued value if pending. `value_string(id, plain)` asks the
unit for its display text. Not mirrored: parameter changes the unit makes itself (its own automation, its editor).
Skewed AU display curves other than logarithmic are treated as linear in the normalised mapping.

## State
`save_state` / `restore_state`: opaque blob = binary property list of the Audio Unit's ClassInfo. Only valid for the unit
that wrote it, so store it with `PluginInfo::id`. `presets()` / `load_preset()` use the unit's factory presets.
`PluginEffect::save_state` prepends on/wet/send mode.

## Using the adapter in a chain
`PluginEffect` matches a chain unit: interleaved stereo float, in place, <= 512 frames per call (longer calls are split),
`set_on`, `set_wet` (0..1), `set_param(index, 0..1)`, `busy()` (on, fading or ringing; skip `run` when false), `reset()`.
Insert mode crossfades dry/wet; send mode (`set_send_mode(true)`, for echo/reverb like units) adds the wet signal and
lets the tail ring out after switch-off (capped at 10 s). To integrate: add a plug-in unit kind to the chain model that
owns a `PluginEffect` (loaded off the audio thread, like any unit prepared before the chain swap), call
`run(buf, n)` where the built-in units run, and persist `{PluginInfo::id, PluginEffect::save_state()}` in chain presets.
`fx_unit_busy` maps to `busy()`. Load failure or a blocklisted id: leave the unit as a pass-through.

## What must cross the process boundary
The engine runs in a helper process, so plug-ins load there. The UI process needs: the catalogue (`PluginInfo` list),
per unit: plug-in id, on, wet, the `ParamInfo` list (once, at load), normalised parameter values (control), state blobs
(save / restore, can be large: use a side channel not the command ring), and load result / render error count.
Instances can live in any process; nothing is global. An engine crash from a plug-in takes the engine helper down, so
probe every plug-in out of process (`probe_catalog` with the `deckhand-plugin-probe` binary next to the app) when it is
first seen, block failures, and re-probe on a version change.

## Not done this round
VST3; plug-in editor windows (generic parameters only); MIDI input for `aumf`; sidechain / more than stereo; host tempo and
transport (`kAudioUnitProperty_HostCallbacks`) for tempo-synced units; delay compensation; latency-hiding; a
parameter-change listener. A UI would need: a native view (`kAudioUnitProperty_CocoaUI` / AUv3 view controller) embedded
in an NSView hosted by the app window, a main-thread run loop, and view lifetime tied to the instance. Until then render a
generic list from `params()` (sliders, stepped pickers from `value_strings`).

## Verification status
Tested only against Apple's built-in units (AUDelay, AULowpass, AUPeakLimiter) offline; no third-party plug-in has been
instantiated and nothing was played through an audio device. AUv3 app-extension plug-ins appear in the scan but are untested.

## In the application (effect chains)

Plug-ins are chain units of kind `EffectKind::Plugin` (protocol 9). The unit is opened in whichever process runs the audio (the engine
helper normally, the app in in-process mode), never on the audio thread, and the audio thread only calls `PluginEffect::run`.

- **Safety.** `plugin_host_vet(id)` (src/audio/plugin_host.cpp) resolves the id in the scan cache, refuses blocklisted ids, and before
  the first load of an id and version runs `deckhand-plugin-probe` in a throw-away process (open, render one block, close). A failure puts
  the plug-in on the blocklist (`<data>/plugins/plugins.cache.blocklist`) and comes back as the edit's error; a pass is remembered in
  `<data>/plugins/plugins.probed` so the app can probe ahead (the rescan does) and the engine does not repeat it. A missing probe helper
  refuses the load. A plug-in that crashes later still takes the engine helper down; the app restarts it and restores the chains (below).
- **Wire.** `FxChainEdit::text` carries the plug-in id for Insert / Replace / Load presets; `Set` with `field >= 16` sets plug-in parameter
  `field - 16` (normalised 0..1, lock free, no command ring). The side channel `kReqPluginOp` (`IEngine::plugin_op`) carries
  `Info` (JSON: id, name, loaded, error, render_errors, latency and every parameter with its current value and display text),
  `GetState` and `SetState` (opaque blob, at most 960 KiB). `kProtocolVersion` is 9.
- **Presets.** The engine's own chain preset names the plug-in (`"plugin": "<id>"`); `fx_chain_save_full` / `fx_chain_load_full` add the
  base64 state of every plug-in unit and restore it over the side channel. A preset naming a plug-in that is gone or blocked loads with a
  pass-through unit and the reason in the result. The app keeps named presets in `<data>/fx-presets/<name>.json`.
- **Restart.** `RemoteEngine` snapshots every edited chain (full preset, states included) a moment after the last edit and loads them
  into a fresh engine process after a restart.
- **Editor.** The chain panel (`ui.chain`, `mod+alt+shift+c`, the CHAIN button of a deck) edits the chain of deck 1..4, master, mic, sampler
  or a stem; plug-in parameters show as generic sliders with the plug-in's own value text, eight to a page. No plug-in windows.
- **Not done.** Host tempo and transport are not given to plug-ins (a tempo-synced plug-in runs at its own tempo; the unit's `beats` field
  does not apply to plug-ins); mix mode is insert (dry / wet crossfade), tails are not rung out after switch-off; the main app process does not
  carry the library-validation exception, so in in-process mode only Apple's own units load (see the signing notes in scripts/build-macos.sh).
- **Tests.** `test_plugin_chain` (Apple's delay / low-pass only: in process, through the helper, and a helper kill with restore),
  `test_chain_app`. No third-party plug-in is ever instantiated by a test; a rescan probes every installed plug-in (out of process), so tests
  do not run one.
