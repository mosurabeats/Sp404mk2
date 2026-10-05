# Porting to DOOM OS / the SP-404MK2

DOOM OS 0.6.1 ships as a binary patch over Roland's 5.52 `SP404MKII_APP1.bin`,
applied in the browser. There is no source and no SDK yet. Its README lists
"an effects editor" and "an SDK so you can build your own screens" as coming
next. This library is written so that porting it is mostly glue once one of
those exists. This page lists what that glue needs, and what to check on real
hardware.

## What the library assumes

* **Memory:** the host allocates `fx->state_size` bytes per instance and
  calls `init` once. The effects need under 256 bytes each, except
  `junochorus` (about 8 KB, mostly its delay line); `juno106` needs about
  9 KB. `process` never allocates, locks
  or does I/O, so it is safe in an audio interrupt.
* **Audio format:** stereo, non-interleaved 32-bit float, processed in place.
  If the SP's effect chain uses fixed-point or interleaved buffers, convert
  at the edge, or port `src/dsp.h` to fixed-point (see below).
* **Block size:** any. Output is bit-identical whatever the block size (this
  is tested), because parameter smoothing and coefficient updates run on a
  sample counter, not per block.
* **Sample rate:** passed to `init`. Converter rates (26.04 kHz, 40 kHz,
  44.1 kHz) are expressed relative to it, so 44.1 or 48 kHz hosts both work.
* **Parameters:** `set_param` may be called at any time from the UI thread
  if writes to a `float` are atomic on the target (they are on ARM). Values
  are smoothed inside the effect, so knob steps don't zipper.

## Mapping to SP controls

* `params[0..2]` → CTRL 1, CTRL 2, CTRL 3. Use `dfx_param_from_knob()` to
  turn a 0..1 knob position into a value; it applies the exponential curve
  for frequencies and times (`DFX_PARAM_LOG`) and snaps stepped parameters.
* `params[3..]` → a SHIFT page (or the DOOM OS effect editor when it lands).
* `name` fields are 8 characters or fewer, to fit the SP screen.
* DOOM OS already records effect knob moves (automation) per CTRL knob for
  Roland's effects. If its effect hosting feeds custom effects through the
  same CTRL path, automation should carry over through this mapping.

## The Juno-106 synth

`doomfx_synth.h` uses the same rules as the effects, plus `note_on`,
`note_off` and `all_notes_off`. `render` overwrites its buffers instead of
processing them in place. For sample-accurate notes, render up to the
event's sample, send the note, then render the rest of the block.

* **Pads:** in chromatic mode, pad → MIDI note, and pad velocity → `note_on`
  velocity (only heard when Velo is above 0).
* **DOOM OS piano roll:** each note's start and end become note on and off
  at those ticks. Its notes are the same events the groove engine works on.
* **Controls:** CTRL 1-3 → Cutoff, Reso, Env amount. The other 22
  parameters need pages, ideally the DOOM OS effects editor once it exists.
  Presets are lists of parameter changes, loaded with
  `dfx_synth_load_preset()`.
* **Output:** stereo, with the chorus included. It can go to a pad's bus or
  straight into the SP's effect chain.

## Groove engine and the DOOM OS sequencer

DOOM OS already has quantise, swing (on 8ths or 16ths), humanise and nudge.
`doomfx_groove.h` adds the machine-specific feels on top:

* **MPC swing** at 96 PPQN: `dfx_groove_mpc(swing)`. Use
  `dfx_swing_offset()` for playback-time swing in a step sequencer, or
  `dfx_groove_apply()` to rewrite recorded notes (timing correct).
* **SP-1200 feel**: `dfx_groove_sp1200(swing)` for 24 PPQN eighth swing, and
  `dfx_convert_ticks(t, 96, 24)` followed by `dfx_convert_ticks(t, 24, 96)` to
  coarsen recorded timing to SP-1200 resolution.

The SP-404MK2's own pattern resolution has to be checked against the
firmware. Pass whatever PPQN it uses; the maths doesn't assume 96.

## CPU budget: what to optimise first

The current code favours clarity. It is light on a desktop, but the SP's
DSP budget is unknown and shared with Roland's own effects. Before a hardware
build, these are the hot spots:

| Where | Cost | Cheaper option |
|---|---|---|
| `dfx_quantize` with companding (`mpc60`) | `logf` + `expf` per sample per channel | 4096-entry lookup table for the expand step |
| `busglue` detector | `log10f` + `powf` per sample | detect in the linear domain, or run gain every 4–16 samples |
| `dfx_sat` everywhere | one divide | fine on ARM with an FPU |
| `juno106` voices | per voice per sample: a ladder (one divide, four one-poles), PolyBLEP; per voice every 16 samples: `exp2f` ×2, `tanf` | the obvious first cut is fewer voices (`VOICES` in `juno106.c`); then a table for `exp2f` |
| Coefficient updates | `tanf`/`powf` every 16 samples | already amortised; table for `tanf` if needed |

If the target has no FPU, or float is too slow in the audio path, `src/dsp.h`
is the only file with numerics shared between effects. A Q31 port of it,
plus the per-effect `process` loops, is the path.

## Testing on hardware

1. Start with `mpc3000` at default settings with Warmth at 0. It should be
   very close to a wire (the test suite checks this), so it is a safe first
   check that buffers, sample rate and gain staging are right.
2. Then `sp1200 output=7-8 pitch=12` with a 10 kHz sine. You should hear a
   strong 3.02 kHz alias. That checks the converter clock.
3. Compare against renders from `build/doomfx render` of the same input.
