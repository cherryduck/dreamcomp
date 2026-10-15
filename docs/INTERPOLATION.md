# High frame rate: geometry interpolation

## Why not "matrix interpolation"

N64 ports interpolate matrices because the N64's RSP receives matrices and transforms vertices on
the GPU side, where RT64 can blend them. A Dreamcast game transforms every vertex on the SH-4 and
hands the PowerVR2 finished screen-space vertices: the display list holds no matrices. The
equivalent here is to blend the **geometry** of two consecutive game frames. The game's logic
keeps running at its own 60 Hz (series rule: never change the logic rate).

## Method

1. Each game render produces a decoded `Frame` (strips with screen-space vertices).
2. Strips of frame N+1 are paired with strips of frame N (`dream::render::match_frames`,
   `engine/render/include/dream/render/interpolate.h`): same parameter words and vertex count, in submission
   order, with a small lookahead for strips that appear or disappear.
3. An in-between frame is drawn with every paired strip's vertices blended at t = 0.5 (position
   and 1/w; texture coordinates and colours from the newer frame), unpaired strips as in the newer
   frame. HUD primitives do not move, so they blend to themselves.
4. Presentation: in-between frame, then the real frame half a game-frame later. One game frame of
   added latency (~16.7 ms). Only useful on displays above 60 Hz; off otherwise.
5. Camera cuts (median displacement far above normal) skip the in-between frame.

## Gates

| Gate | Measurement | Status |
|---|---|---|
| G1 Correspondence | `dreamcomp_ta_match` on consecutive fight frames: ≥ 95 % of vertices paired, displacement distribution plausible | **passed** 2026-10-05: 7 consecutive Soulcalibur fight pairs, 97.8-100 % of vertices paired, median 2-3 px / p90 5-13 px per frame; outliers are off-screen or behind-camera vertices (clipped by the game) and a few mispairs, so strips moving implausibly far are not blended |
| G2 Visual | in-between frames look like a frame of the game (no torn strips) on contact sheets of fights, menus, cuts | **passed for fights** 2026-10-05 (`DREAM_SHOT_INTERP=1` sequences: real, blended, real); menus/cuts not inspected frame by frame (23 cuts detected and skipped in 2700 frames). 2026-10-06: Daniel watched the `tools/slowmo.py` clips (pre-round camera sweep, fight action): the 120 fps side "much smoother", no glitches noticed |
| G3 Cost | per-frame matching + second render within budget on Iris Xe (`DREAM_PROFILE=1`) | **passed**: 0.03-0.10 ms per blend; unthrottled 3.9x real time with interpolation vs 4.1x without (scale 1) |
| G4 Pacing | 120 Hz output smooth (frame times even), audio unaffected | **partly measured** 2026-10-06: with `--present-mode immediate` the 60 Hz laptop panel no longer blocks the second present, and a fight runs at 1.0x real time with 1186 blended + 1192 real presents (~120/s), 0 audio underruns. Seeing the smoothness still needs a >60 Hz display. With vsync on a 60 Hz panel the second present blocks and the game runs at 0.5x, hence `fps = auto` (on only above 60 Hz). Before the pacing fix in `engine-changes.md` (2026-10-06) runs without vsync were not throttled at all |

## Pairing by texture coordinates (2026-10-15)

Ecco (240 Hz display) showed flickering seams in turns. Parameter words and vertex count do not
identify a strip when a game draws its terrain as many strips of one texture and culls some per
frame: in-order pairing matched strips with the wrong neighbours, and strips the game re-clips at
the near plane every frame never paired, so they stayed put while their neighbours moved. Now a
textured strip pairs only with one whose texture coordinates match (world geometry keeps them);
the rest get a second lookup by a texture-coordinate key out of list order; a UV-confirmed strip
may move up to 640 px (fast turns); and an in-between frame is dropped, like a cut, when more than
1 % of the drawn area would stay unblended. Ecco's `play` scenario: 8552 in-betweens drawn, 2145
dropped (mostly in turns), 0.19 ms per blend.

## Tools

- `dreamcomp_ta_match A.ta B.ta [--window N]` — pairing coverage and displacement.
- `--dump-ta FILE --dump-ta-at-frame N --dump-ta-count K` — K consecutive display lists.
- `tools/slowmo.py` — half-speed side-by-side clip, 60 fps vs 120 fps with blended frames, for
  judging the motion on a 60 Hz screen (2026-10-06: pre-round camera sweep and fight action,
  180 frames each, every frame blended, no torn strips seen).

## Using it

Setting `fps`: `auto` (default: blend only when the window's display refreshes above 60 Hz),
`120` (always; on a 60 Hz panel this halves the game's speed), `60` (off). Engine flags:
`--interpolate`, `--interpolate-auto`. The end-of-run report prints an `interpolation:` line
(blended frames, cuts, paired vertices, strips not blended, cost). `DREAM_SHOT_INTERP=1` makes
every screenshot also write the blended frame before it (`screenshot-NNN-interp.ppm`).

## Implementation

- `engine/render/src/interpolate.cpp`: `match_frames`, `blend_frames` (cut detection on the
  median motion of paired on-screen strips, per-strip jump limit, depth range covering both
  frames).
- `engine/runtime/boot/boot_main.cpp` (`Live`): keeps the previous decoded frame (after the HUD
  correction, so the HUD blends to itself), renders the blended frame into a second offscreen
  target before the real one, presents it ahead of the real frame, which follows half a guest
  frame later. The renderer rotates four vertex buffers so two targets in flight never share one.
