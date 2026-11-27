# Recording from an analogue input

The capture module (`src/capture`, library `dh_capture`) is the offline core for recording a vinyl side (or any analogue
input), splitting it into tracks, cleaning it up and exporting it. It never opens an audio device: the caller pushes
sample blocks. The command line front end is `deckhandctl capture ...`.

## Recording: `CaptureSession`

- `start(CaptureConfig, err)` creates the file; `resume(path, err)` continues an earlier or interrupted recording.
- `push(interleaved, frames)` takes float blocks at the session's sample rate (any rate, 1 to 16 channels; no resampling,
  the file is written at the rate you configure). In async mode (default) it only fills a lock-free ring (about 10 s); a writer
  thread does the file I/O and the statistics, so it is safe from an audio callback. A full ring drops frames and counts them.
- File: WAV, 16 or 24-bit PCM or 32-bit float. Beyond 4 GB the header turns into RF64 (`ds64`). The header is refreshed every
  `flush_seconds` (1 s), so a crash loses at most that much. A file whose header is behind, or whose last frame is cut, is
  repaired from its length (`repair_wav`, or automatically by `resume`): every whole frame is kept.
- `stats()`: frames, seconds, peak per channel and overall, RMS, clip count (|x| at or above `clip_level`), DC offset per
  channel, dropped frames, write error. `take_meter()` gives peak/RMS/clips since the last call, for a live meter.
  `set_paused(true)` discards input (between sides). `flush()` asks for a header refresh now. `stop(err)` finishes the file.

## Splitting: `split.hpp`

1. `analyse_splits(source, settings)` reads the audio once and builds a click-robust level envelope (10 ms hops; each hop is
   the median of five 2 ms sub-blocks, after a 4th-order high-pass at 100 Hz, so clicks, crackle and rumble do not count).
2. `find_splits(analysis, settings)` (milliseconds, re-run freely): adaptive noise floor (quietest half second), programme level,
   gap threshold = min(floor + `adapt_db`, programme - `min_depth_db`). A gap is a stretch at or below the threshold that is at
   least `min_gap_s` long, stationary (inter-quartile spread within `max_gap_spread_db`, so a quiet musical passage is not one)
   and not touching the side's start or end (those are lead-in and run-out). Pieces shorter than `min_track_s` are merged
   (the weaker neighbouring gap goes). A side without enough depth (`split_levels().splittable == false`) gets no splits, so
   continuous recordings such as live albums are left whole. Each cut sits midway between the audible end of one track and the
   audible start of the next; `confidence` (0..1) combines depth, length and steadiness.
3. `audible_range` / `plan_pieces(analysis, settings, points, contiguous)` turn points into pieces. Lead-in and lead-out are
   trimmed with an onset threshold (`onset_db`), and the fade tolerance (`fade_db`, `fade_tolerance_s`) keeps quiet intros and
   fade tails that are still above the noise. `contiguous = true` tiles the audible range with no lost frame (cut at the points,
   only the first start and last end trimmed) so exports concatenate bit-exactly; `false` trims every piece on both sides.
4. `SplitPlan` is the editable list: `add`, `move`, `remove` (user points have confidence 1), `set_auto(find_splits(...))` to
   re-run. User points survive a re-run, and an automatic point the user removed or moved is not brought back (`vetoes`).
   `nearest_quiet_frame` snaps a clicked position to the quietest nearby spot.

## Clean-up: `cleanup.hpp` (all off by default)

Order: DC removal, rumble high-pass (zero-phase, `rumble_hz` 45), click repair (third-difference detector against a local
robust sigma, AR interpolation across the damage, only disturbances up to `click_max_ms`), loudness normalise (the project's
`measure_loudness` and `auto_gain_db`, to `target_lufs` with a `ceiling_dbfs` peak limit), and short raised-cosine fades.

## Export: `export.hpp`

- `export_pieces(source, pieces, tags, dir, settings)` writes each non-empty piece as `NN - Artist - Title.wav|aiff`, with tags
  (artist, title, album, genre, year, comment, track number and total). WAV carries a `LIST/INFO` chunk and an `id3 ` chunk;
  AIFF carries an `ID3 ` chunk. Files are written under a `.part` name and renamed; existing files are not replaced unless
  `overwrite`.
- With clean-up off a piece is the source samples untouched; contiguous pieces concatenate bit-exactly to the trimmed source.
- `export_edit(source, source_path, start_s, end_s, out, settings)` is "export an edited track": trim, fades, normalise a decoded
  track to a new file. It refuses to write over `source_path` and never opens it for writing.
- FLAC: the tree has no encoder, so `flac_available()` is false and a FLAC request returns an error. AIFF is 16 or 24-bit, 4 GB at most.

## Command line

```
deckhandctl capture info FILE
deckhandctl capture repair FILE
deckhandctl capture split FILE [--min-gap S] [--min-track S] [--min-depth DB] [--export DIR] [--contiguous 1]
        [--format wav|aiff] [--bits 16|24|32] [--artist A] [--album B] [--titles "T1;T2"] [--dc 1] [--rumble HZ] [--declick 1]
        [--normalise LUFS] [--fade MS]
deckhandctl capture edit IN OUT [--start S] [--end S] [--fade-in MS] [--fade-out MS] [--normalise LUFS] [--format wav|aiff]
```

## Limits

Thresholds are tuned on synthetic sides; real records need the settings exposed in a panel. Click detection can flag very
sharp percussion, so repair stays off unless asked. Mid-recording sample-rate changes are not supported.

## In the application

- **Input tap.** `IEngine::input_tap_enable / input_tap_read / input_tap_dropped` hand over the frames the capture device
  delivers (stereo, a mono input doubled) at `InputStatus::sample_rate`, before the microphone channel's resampling, gain
  and ducking. In the helper process the tap travels through its own shared ring (protocol 11); no new entitlement is
  needed, the helper already opens the input for the microphone. Recording uses the engine's one input, so starting a
  recording replaces the microphone input (and closes it again afterwards when it was not open before).
- **`CaptureService`** (`src/app/capture_service.*`) pumps the tap into a `CaptureSession`, keeps the meter (decaying peak
  hold, RMS, clip count), then holds the finished side: the envelope, the editable `SplitPlan`, track names and the export.
  Its `Options::open_device = false` uses the input that is already open, which is how tests drive it with a fake input.
- **Panel** `ui.capture` (default shortcut `mod+alt+shift+v`): pick the input and channel, record with the level meter and clip
  count, pause, stop. After the stop the side is analysed; its envelope is drawn with the split points. Drag a marker to move
  it, shift-click the side to add one, select a marker and press Remove. Sensitivity (shortest gap, depth) re-runs the
  detection and keeps your own points. Name the tracks, choose clean-up (DC, rumble, clicks, normalise, short fades), WAV or
  AIFF, 16 or 24 bits, and export to `<data folder>/Captures/<recording>-tracks`.
- **REST:** `/v1/capture/*` and `POST /v1/library/tracks/{id}/export-edit`, see docs/API.md.
- Recording continues while the panel is closed. The files live in `<data folder>/Captures`.
