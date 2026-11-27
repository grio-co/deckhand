# Step sequencer: pattern text format

The sampler plays patterns of 16 steps over its 16 slots; 8 patterns can be switched live (immediately, or from the next bar).
Run, record, tempo and swing are controls (`seq.*`, docs/API.md). The tempo follows the deck that leads (its beat grid) and otherwise runs
free at `seq.bpm`; every second step is delayed by `swing` x a step (0..0.75).

A pattern travels as one line of text (`GET`/`PUT /v1/sequencer/patterns/{n}`, the library settings `seq.pattern.<n>`):

    dhseq1;<step 1>;<step 2>; ... ;<step 16>

The text starts with `dhseq1` and has exactly 16 steps after it, separated by `;`. A step is

* `-` when nothing plays on it, or
* `<mask>:<velocities>`: `mask` is four hex digits, bit n set = slot n+1 plays (bit 0 = slot 1 ... bit 15 = slot 16); then two hex digits per
  slot in the mask, lowest slot first, the velocity `01`..`7f` (`7f` = full).

Example: slot 1 and slot 3 on step 1 (full and about half), a hit of slot 16 on step 5:

    dhseq1;0005:7f40;-;-;-;8000:7f;-;-;-;-;-;-;-;-;-;-;-

A velocity may be followed by `@<hex digit>`: that slot plays slice `0`..`f` (slice 1..16) of its sliced sample on that step instead of the whole
sample, once, ending where the slice ends. `0005:7f@340` = slot 1 plays slice 3 at full, slot 3 plays the whole sample at `40`. Without `@` the
text is the original format: old saved patterns read and write back unchanged (a text with `@` is not understood by an older build). A step whose
slot is not sliced, or has fewer slices than the number asked for (re-sliced since), plays the whole sample.

Letter case does not matter on input; a text that is not valid (wrong head, not 16 steps, a velocity of `00`, a mask without its velocities)
is refused whole and the pattern stays as it was. Empty patterns are `dhseq1` followed by sixteen `-`.

Engine API: `seq_step(pattern, step, slot, on, velocity, slice)` with slice -1 = keep the cell's slice, -2 = whole sample, 0..15 = that slice (the
command carries it in bits 8+ of `i`; protocol 10). Pads recorded while the sequencer records (`seq_record`) keep their slice: a slice trigger
writes the step with that slice, a whole-sample pad writes it with none. Clearing a slot's cells clears their slices; switching a cell off keeps the
slice for when it is switched on again.

Slicing: `sampler.slot.<n>.slices` cuts the slot's sample into 8 or 16 equal slices; `sampler.slot.<n>.slice.<j>` plays slice `j` once. A
controller can do both with the mapping actions `slice` and `seq_pattern` (docs/CONTROLLERS.md).

## Slices in the application

With a sliced slot in the grid, the sampler panel shows a slice row (Whole, 1..N): the chosen entry is the "brush". Clicking an
empty cell turns it on with the brush's slice, clicking a lit cell that has another slice changes its slice, clicking a lit cell
that already has the brush's slice turns it off. A lit cell shows its slice number. `POST /v1/sequencer/step` takes an optional
`slice` (0 = whole sample, 1..16).
