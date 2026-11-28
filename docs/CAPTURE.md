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

- `export_pieces(source, pieces, tags, dir, settings)` writes each non-empty piece as `NN - Artist - Title.wav|aiff|flac`, with tags
  (artist, title, album, genre, year, comment, track number and total). WAV carries a `LIST/INFO` chunk and an `id3 ` chunk;
  AIFF carries an `ID3 ` chunk. Files are written under a `.part` name and renamed; existing files are not replaced unless
  `overwrite`.
- With clean-up off a piece is the source samples untouched; contiguous pieces concatenate bit-exactly to the trimmed source.
- `export_edit(source, source_path, start_s, end_s, out, settings)` is "export an edited track": trim, fades, normalise a decoded
  track to a new file. It refuses to write over `source_path` and never opens it for writing.
- FLAC: `flac_available()` is true. `ExportSettings::container = Container::Flac` writes 16-bit for `Pcm16` and 24-bit for `Pcm24` or
  `Float32`, 1 to 8 channels, with the same float-to-integer rounding as the WAV writer, so FLAC pieces decode to exactly the
  samples the WAV pieces hold and contiguous pieces still concatenate bit-exactly. Tags go in a VORBIS_COMMENT block (TITLE, ARTIST,
  ALBUM, GENRE, DATE, COMMENT, TRACKNUMBER, TRACKTOTAL). More than 8 channels is an error. AIFF is 16 or 24-bit, 4 GB at most.

## FLAC encoder: `flac.hpp`

Written from the public format specification (RFC 9639); no third-party code. `FlacWriter` streams: `create(path, params, tags)`,
`write_int` / `write_float` in any chunk size (the output does not depend on the chunking), `close()` encodes the short final block
and rewrites STREAMINFO and the SEEKTABLE, so a long side is read and written once and never held in memory twice.

- Frames: fixed block size (default 4096), CRC-8 header, CRC-16 frame, UTF-8 frame numbers. Stereo picks independent, left/side,
  right/side or mid/side per frame by encoded size.
- Subframes: constant, verbatim, fixed predictors 0 to 4, LPC (Tukey-windowed autocorrelation, Levinson-Durbin, error-feedback
  quantised coefficients, orders up to 12) with Rice-coded residuals and a partition-order search (up to 8); wasted bits are removed.
  `level` 0 uses fixed predictors only, 1 to 4 LPC up to order 8, 5 to 8 (default 5) tries several orders up to 12.
- Metadata: STREAMINFO with the MD5 of the unencoded samples (own MD5 in `Md5`), a SEEKTABLE with a point about every 10 s (sized from
  `total_frames_hint`; unused points stay as placeholders), VORBIS_COMMENT, and 2 KiB of PADDING.
- Range: 4 to 24 bit, 1 to 8 channels, sample rates 1 to 1048575 Hz, block sizes 16 to 65535.
- Measured (release build, Apple silicon, one core, 60 s synthetic music-like stereo at 44.1 kHz): 16-bit 53.6 % of the WAV size
  (ratio 1.86) at about 96x real time; 24-bit 69.1 % (ratio 1.45) at about 80x. Real music with a noise floor compresses less.

Tests (`tests/test_flac.cpp`): an independent decoder written for the tests (`tests/flac_ref_decoder.hpp`: its own bitwise CRCs, MD5,
all subframe types) must reproduce silence, DC, full-scale sine, noise, music-like, wasted-bit, square, extreme-value and impulse
signals bit-exactly for 1/2/3/4/6/8 channels, 8 to 24 bit, 8 kHz to 1 MHz rates, lengths 0 to 12288 frames and block sizes 16 to 65535
at levels 0, 3 and 8; the project's own decoder (`decode_file`) must read 16/24-bit files at 44.1/48/96 kHz bit-exactly (required on
macOS, skipped where the platform decoder cannot read FLAC); and `ffmpeg`, only if it is on PATH, must decode 1/2/6-channel files
with CRC checking turned on to the same samples.

## Command line

```
deckhandctl capture info FILE
deckhandctl capture repair FILE
deckhandctl capture split FILE [--min-gap S] [--min-track S] [--min-depth DB] [--export DIR] [--contiguous 1]
        [--format wav|aiff|flac] [--bits 16|24|32] [--artist A] [--album B] [--titles "T1;T2"] [--dc 1] [--rumble HZ] [--declick 1]
        [--normalise LUFS] [--fade MS]
deckhandctl capture edit IN OUT [--start S] [--end S] [--fade-in MS] [--fade-out MS] [--normalise LUFS] [--format wav|aiff|flac]
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
  detection and keeps your own points. Name the tracks, choose clean-up (DC, rumble, clicks, normalise, short fades), WAV,
  AIFF or FLAC (FLAC only where `flac_available()`), 16 or 24 bits, and export to `<data folder>/Captures/<recording>-tracks`.
- **REST:** `/v1/capture/*` and `POST /v1/library/tracks/{id}/export-edit`, see docs/API.md.
- Recording continues while the panel is closed. The files live in `<data folder>/Captures`.
