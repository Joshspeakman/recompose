# Frame interpolation

Frames shown between a title's own, so motion looks smoother than 60 while
the title itself still runs at 60. Written 1 October 2026 for xboxrecomp
developers. The code is `src/hle/hle_d3d8_interp.c`; its header comment is the
reference, and this note is the overview and the measurements.

## Why this, and not a higher frame rate

Games of this era step their simulation once a frame. TimeSplitters 2
uncapped runs its physics and timers fast (`resolution-and-framerate.md`), and
making each title's logic time-based is months of work per title that helps
no other. So the title stays at 60, and the toolkit draws the extra frames:
it keeps each frame's host calls and draws the newest frame again with the
matrices in its vertex constants blended part of the way back toward the
frame before. The title never learns these frames exist.

## Switching it on

| Setting | Values |
| --- | --- |
| `RECOMP_FRAME_INTERP` / `frame_interp` | `1` off (the default), `2`-`4` frames shown for each one the title draws. The launcher's "Frame interpolation" row |
| `RECOMP_INTERP_PROJECTION` / `interp_projection` | the first of the 4 projection registers, or `none`. Default: what the title said, else the Hor+ register |
| `RECOMP_INTERP_AFFINE` / `interp_affine` | `64-75`: a run of 4-register affine matrices (model-view, bones), or `none` |

A title project says which registers are which with
`xbox_D3D8SetInterpRegisters(projection, affine_first, affine_count)` (in
`d3d8_xbox.h`); the settings override it. TimeSplitters 2 calls it with
`(60, 64, 12)` at its first present (`src/recomp_manual.c` in split2-recomp).
Without it nothing but the projection is blended, which is right for no
title's moving objects: find the registers first (below).

Best with variable refresh (`vrr`, fullscreen), where each frame is shown the
moment it is presented. Windowed, the compositor shows the newest frame at
each refresh of the display, which is still smoother than 60 but uneven.

## How it works

**Where the time comes from.** The flip gate (`xbox_memory_layout.c`) holds a
finished frame until its vblank. TimeSplitters 2 finishes a frame in 2-5 ms
and then waits 11-14. The gate lends that wait to the module
(`xbox_Nv2aFlipGateSetIdle`): frame N is complete before the moment halfway
between N-1 and N, so the in-between frame is drawn from N-1 and N, and N is
still shown at its vblank as it always was. **At 2x this adds no latency.**
Nothing is extrapolated.

**What is kept** (`hle_d3d8_record.c`, while `hle_d3d8_interp_rec` is set):
every host call that changes state or draws, as an op in the deferred queue's
format, from the frame's start. The frame starts with a snapshot of the state
set before it (`hle_d3d8_interp_snapshot`, `capture_snapshot`'s list as ops),
so running the list draws the frame again. Vertex constants are kept as
src/hle gave them, before Hor+ scales its register, because they go through
the same setter again. Each draw is kept with a key (a hash of its vertex and
index bytes, its program, shape, textures and target) and the values of the
registers that are blended. Texture releases and program deletes wait until
no kept list can name them. Screen copies are wrapped
(`host_CopyBackBufferToTexture`) so an in-between frame copies its own screen.

**Drawing it.** `xbox_D3D8InterpBegin` points "the back buffer" at a second
image the size of the scene: the scene target still holds frame N, about to be
shown. The kept list runs through the same `host_*` wrappers with keeping off;
before each draw, its blended registers are set. Then the F9 overlay, the
present (`xbox_D3D8InterpPresent`, which does not count a frame), and
`xbox_D3D8InterpEnd`.

**Matching** frame N's draws to N-1's, in four tiers, tried in the order 1,
2, 4, 3:

1. the same bytes, program, shape and target; of several copies of one mesh,
   the one nearest in translation;
2. the same program, shape, textures and target (a mesh the CPU rewrites);
4. the same bytes, shape and target, drawn by another program that reads its
   matrices from the same registers (a mesh the title moved to another
   program); of several copies, the one nearest in translation;
3. the same program, stride, primitive type and target (a batch the CPU
   builds, whose count and textures change every frame: particles).

Unmatched draws are drawn as N drew them.

**Matching, tier 4** (28 November 2026, TimeSplitters 2: a corridor that went
dark and sharp for one in-between frame while walking in and out of
doorways). TimeSplitters 2 draws each room's pieces with one of two programs
and moves a room's pieces from one to the other as the player walks between
rooms. Without tier 4 those pieces had no partner either way: N's were moved
back with the camera and N-1's were filled in (below), so the in-between
frame drew the room twice. The level is drawn alpha blended, so the room
drawn twice was darker and more solid than in either game frame. On a walk
through Mexican Mission's corridors (16:9, FOV 80, interpolation 3), the
frame before's pieces drawn again over the same mesh in N went from 311 in
38 frames to none.

**Blending.** The projection term by term, when both frames have the same kind
(perspective or not). Each affine matrix is blended by its motion: D = B A^-1,
the step from frame N-1's matrix A to N's B, split by polar decomposition into
a rotation (slerped from the identity) and the rest (blended linearly), with
its translation scaled, and applied to A. A static object's D is the camera's
own step, the same for every piece of the level, so the pieces move together
however fast the camera turns; blending A and B themselves pulled each piece
toward the camera by its own amount part of the way through a turn. Only what
the draw's program reads is blended (`d3d8_vsh_bound_reads`): a register the
program never reads holds whatever was last put there. Only draws whose
program goes through the projection are blended, so screen-space draws (HUD,
menus) are N's to the pixel and do not shimmer. A motion that turns more than
12 degrees, or moves by more than half the distance, in one frame is a cut;
a camera turning that fast cuts most of the frame, so the frame is held.
(12, not more: with a real mouse, TimeSplitters 2's in-between frames lost
pieces of the level at the edge the camera was leaving in 13% of frames
turning 15 to 20 degrees, 2 to 3% below 15.) A cut piece, and a piece the
frame before has no partner for, is moved back with the camera: as N has it
in the world, seen from the in-between frame's camera.

**The camera's own motion** is the motion most matched pieces share, by
primitives, each weighed down by how much nearer the eye than most of the
frame it is, and agreeing in turn and move alike. What is carried in view
space (TimeSplitters 2's first-person weapon, half a unit from the eye)
holds still on the screen while the camera turns, and on a fast turn toward
a bare wall its two pistols outnumbered the level in view.

**Without the title's scissors** (`xbox_D3D8SetScissorsOff`). A title whose
scissors only spare drawing while it plays can be drawn without them, in the
in-between frames or in its own frames too. TimeSplitters 2 clips each room
it sees through a doorway, and the sky it shows through one (drawn flat, no
depth test), to the doorway's rectangle on screen; in an in-between frame
the rectangles are where the newer frame had them, and carrying them along
with the turn (`d3d8_SetScissorWarp`, still used for a title that keeps its
scissors) cannot follow a walk. Its frames in play are the same without
them, HUD and all, but for flickering hairlines of sky and sand the
whole-pixel rectangles left at higher resolutions; its menus need theirs, and
so does a split screen's halves, which it keeps to their rectangles with a
scissor too, so it asks only for a frame of play with every player's view
the whole frame. A band the viewport's whole width and not its height is
kept even then: a letterbox (its end of a match draws 3D streaks inside
one).

A frame is held as a cut when what was cut, weighed as the camera's vote is,
is more than half: with a bare wall in view TimeSplitters 2's pistols
outweighed a level that turned past the limit by primitives alone.

**Matching, tier 3** is for blended draws only: an opaque piece of the
level matched by program and layout alone could take the place of the one
the frame before drew there.

**What only one of the two frames drew** (November 2026, TimeSplitters 2,
whose in-between frames showed the sky through doorways on fast mouse turns).
A title draws what its newest camera sees, and may clip what it sees through
a doorway to the doorway's rectangle on screen (scissors). An in-between frame
looks part of the way back, so:

- the camera's step is taken from the motion most of the matched draws share,
  by primitives (the level's);
- every scissor rectangle inside the viewport is carried along with the
  camera's turn since N (`d3d8_SetScissorWarp`: its corners as directions
  from the eye, turned, projected again; a side on the viewport's edge stays
  there);
- N's 3D draws with no partner in N-1 are moved back with the camera;
- N-1's 3D draws with no partner in N, to the target the level is drawn to,
  are drawn too, after N's last 3D draw there (before its full-screen passes
  and 2D), moved forward with the camera: N-1's whole list is run for its
  state, nothing cleared, copied or retargeted (`hle_d3d8_op_sets_state_only`),
  and N's state put back. For a turn the two frames' views take in every
  view in between.

`RECOMP_INTERP_ACCOUNT=<from>-<to>` prints each frame's primitives by how
they matched and what was filled in, and of what was filled in, how much
N draws too: unpaired (drawn twice), paired with a second copy N-1 drew at
the same place, or a copy elsewhere. `RECOMP_INTERP_FILL=0` leaves the
filling out for comparison, `RECOMP_INTERP_TIER4=0` leaves tier 4 out, and
`RECOMP_INTERP_DRAWS=<tick>` or `<from>-<to>` lists each draw of those
frames: how it matched, the affine matrices its program reads, and the first
one's translation.

**Held frames.** A frame is not interpolated, and the display keeps N-1 until
N as at 60, when it has no 3D, when less than 60% of its 3D primitives
matched, when more than half were cut, when a movie went over it, while a
capture is being written, or when the redraw would not finish before the
vblank.

**Pacing.** In-between frame k of n is due k/n of the way from the last frame
shown to the vblank that shows this one. It is started early by what a redraw
has been costing (measured up to its present; a Vulkan present can then block
on the next image), and given up if it would not be done before the vblank.
Its blend is for the moment it is shown.

## Results: TimeSplitters 2, Siberia

Offline first (week 1). `RECOMP_D3D8_CAPTURE_EVERY=1` records consecutive
frames and `d3d8_replay --const-patch` redraws a capture with constants
replaced per draw. From three consecutive gameplay frames N-1, N, N+1 (walking
and turning), N-1 and N+1 blended at 0.5 were compared with the real N:

| | |
| --- | --- |
| 3D primitives matched | 94-100% |
| Blend closer to the real frame than repeating N-1 | 8 of 8 triples: 38.3-40.9 dB against 27.4-27.8 dB |

Live, 2x, D3D11, the same walk (five-second windows):

| | |
| --- | --- |
| Game frames | 60.0 a second, unchanged |
| In-between frames shown | 296-300 for each 300 frames in play |
| 3D primitives matched | 98-100% |
| Redraw | 0.2-0.9 ms average, under 2.7 ms worst |
| Shown from its moment | 0.15-0.37 ms average, about 1 ms worst |
| Blend | 0.51-0.52 average (0.5 is on time) |
| Held | menus with no 3D, the cut from the level's intro to play, empty frames at loads |

Vulkan submits more slowly than D3D11 (its own frames take about 1.5 ms
longer): at 2x the redraw is 1.3-3.4 ms and pacing is as good; at 3x most
slots fit and some are late, and are skipped.

## Diagnostics

- The five-second report: `[INTERP] 2x: 300 frames, 300 in-between shown, 0
  late; held ...; matched 100% of 3D primitives; redraw ... ms; shown ... ms
  from its moment; blend ...`. Frames that are neither shown nor held ran past
  their vblank, so there was no wait to use.
- `RECOMP_INTERP_DUMP=<prefix>` with `RECOMP_INTERP_DUMP_FROM=<swap>` writes
  in-between frames as BMPs. Beside `RECOMP_HLE_D3D8_DUMP_EVERY=1` from the same
  swap, they sit between the frames either side.
- `RECOMP_INTERP_DEBUG=1` prints the draws behind a frame held as a cut.
- F9's overlay shows the game's frame rate and, with interpolation, what is
  shown.

**Finding a title's registers.** Capture two consecutive frames
(`RECOMP_D3D8_CAPTURE_EVERY=1`) and look at the constants of a few draws:
a projection is `(sx,0,0,0) (0,sy,0,0) (0,0,a,b) (0,0,±1,0)` and set once or
twice a frame; a model-view changes from draw to draw, with `(0,0,0,1)` in its
fourth register.

## Limits and next steps

- **Particles the CPU positions stay at 60** (TS2's snow). Blending their
  vertex positions for tier-2 matches would fix it.
- **Fixed-refresh displays at a rate that is not a multiple of 60** (144 Hz)
  need VRR, or a render thread that shows the state at a fixed small delay
  (the design's "model B", 3-4 weeks).
- **Fixed-function titles** are not blended yet: their WORLD, VIEW and
  PROJECTION transforms are named by `SetTransform` and would need no hints.
- **Push-buffer draws** (Burnout 2 in two places, XGRA, Breakdown) never reach
  the host as draws and cannot be redrawn.
- **Deferred frames** (`RECOMP_HLE_D3D8_DEFER`) are not combined with this.
- A title that does not clear its depth buffer each frame is not handled: the
  in-between frame shares the device depth buffer.
