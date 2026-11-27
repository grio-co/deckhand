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

Letter case does not matter on input; a text that is not valid (wrong head, not 16 steps, a velocity of `00`, a mask without its velocities)
is refused whole and the pattern stays as it was. Empty patterns are `dhseq1` followed by sixteen `-`.

Slicing: `sampler.slot.<n>.slices` cuts the slot's sample into 8 or 16 equal slices; `sampler.slot.<n>.slice.<j>` plays slice `j` once. A
controller can do both with the mapping actions `slice` and `seq_pattern` (docs/CONTROLLERS.md).
