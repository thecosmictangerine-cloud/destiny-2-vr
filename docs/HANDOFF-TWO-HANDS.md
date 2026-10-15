# Handoff — the two-handed grip (written 2026-10-15, end of the arms session)

**Next task: the left hand takes hold of the weapon.** The user chose this over reloads. Read
`docs/ARMS.md` first: it is the current description of the arm system this builds on. This file is
what that one does not say: where things stand, what is decided and what is not, and a concrete
plan with the traps already visible.

## Where things stand

- **Arms (F4) work on the mock and have never been on the headset.** Each arm follows its own
  controller, the right hand holds the weapon, the left hand is free on its controller and closes
  with the left grip. The weapon seats itself in the right hand (palm on the controller's grip pose
  to 1–2 mm, every weapon turned to the hand cannon's canonical grip, so it points along the
  controller). Verified with `scripts/testing/Test-Arms.ps1`; wrist error 0.0000 across the matrix.
- **Nothing is committed.** The `sunrise-vr` working tree (branch checked out: `vr-public`) holds:
  new `vr_gpu.cpp/.h`, `vr_d3d_slots.cpp/.h`, `vr_arms.cpp/.h`, `vr_math.h`; edits to `vr_camera.cpp`
  (one call), `vr_weapon.cpp/.h` (seat loops), `Sunrise.vcxproj` (+7 lines); new scripts
  `lib/VrCommands.ps1`, `testing/Test-Arms.ps1`, `gpu_capture.py`, `find_arms.py`, `arm_rig.py`. The
  docs repo has `docs/ARMS.md`, this file, and an updated `CLAUDE.md`, also uncommitted. Commit only
  when the user asks.
- The deployed DLL in `C:\Games\Sunrise\bin\x64` is the latest build.
- The user's headset verdict on the arms is still pending. If it arrives first, it takes priority:
  shoulder/elbow feel is tunable live from `SVR_Arms.txt` (`neck`, `shoulder`, `pole`, `stretch`,
  `twist`) without a rebuild.

## What the user decided, and what is still open

Decided:

- Two-handed grip is the next thing to build. Reloads come later.

Proposed to the user and **not yet answered** — ask, or build it switchable and say so:

1. **Two-handed AIM, on top of the visual snap.** The recommendation was to do both, with the aim
   part switchable: while the left hand holds the foregrip, the barrel runs from the right hand
   towards the left (Pavlov-style), and roll still comes from the right controller.
2. **How the snap is triggered.** Proximity alone, or proximity plus squeezing the left grip
   (the convention in most VR shooters)? **Squeezing the left grip currently throws a grenade**
   (`vr_gamepad.cpp` maps it), so "grip to hold" needs that binding moved first. That is a decision
   about F2's mapping, so it is the user's.
3. Reloads (for later): when the game's animation takes the left hand off the gun, should the
   player's left arm follow it — always, only when the hand was near the weapon (recommended), or
   never?

## The plan

Everything needed is already computed inside `arms::rewrite()` (`vr_arms.cpp`) every draw.

### 1. The foregrip comes free from the game's animation

The game's own animated LEFT hand is on the weapon's foregrip, for every weapon, as Bungie authored
it. In `rewrite()` it is `animated[left]` (a frame at the bind wrist, axes of the hand bone), and the
drawn left palm is `palmDrawn[left]`. That is the snap target. No per-weapon measurement.

But do **not** snap to the live animation. During reload, inspect, melee and sprint the animated
left hand leaves the gun, and a snapped hand would fly off with it. Instead, **learn the foregrip
relative to the right hand**, the way `g_grip` learns the mirrored grip:

```
foregrip = inverse_rigid(animated[right]) * animated[left]    // left hand in the right hand's frame
```

Update it slowly (EMA), and only while it is steady. A jump in it is exactly the signal that the
game's animation has taken the left hand away (useful later for reloads). Reset it on a sustained
change, which is a weapon swap, reusing the 800 ms re-seat logic `g_grip` already has. Then:

```
snapTarget = animated[right] * foregrip
```

That follows the gun through the seat loops and recoil, and ignores the left hand's own animation.

### 2. Snap with hysteresis and a blend

- Engage when `|left palm target − snapTarget palm| < 0.10 m` (plus the grip button, if the user
  wants it). Release above 0.15 m.
- Blend weight `w` towards 1 or 0 over about 0.12 s (per-frame step from real time, not a frame
  count: the mock runs at about 190 fps, the headset at 90).
- Hand frame: `blend_about(controllerTarget, snapTarget, w, …)`. The helper exists; it blends
  rotation and moves a pivot linearly. Pivot about the palm, not the wrist, or the palm swims.
- Fingers: blend from the grip-curl fingers (`left_finger()`) to the game's animated left fingers
  (`inverse_rigid(animated[left]) * M_a(b)`, which hold the foregrip) by `w`, with `blend_about` at
  each bone's centroid (`rig.cen`).
- While snapped, the left arm's IK simply solves to the snapped wrist. The elbow follows; nothing
  else changes.

### 3. Two-handed aim (if the user agrees)

The weapon's orientation is set in `vr_weapon.cpp` (`hand_pose_for`, then `apply_seat_rotation`) on
the GAME thread, from `xr::frame_sample`. Compute it there, not in the arm module, so it uses the
same sample as the position:

- The arm module publishes, once learned, the foregrip's palm point in the RIGHT CONTROLLER's
  frame. That is a constant per weapon, from `foregrip` above plus the right hand's seat.
- When engaged (a published flag and `w`), the weapon module builds the basis from the two hands:
  `forward = normalize(leftPalm − rightPalm)`, corrected by the fixed angle between that line and
  the barrel in the authored hold. Learn it as the rotation that maps the hand-to-hand direction
  onto the controller forward at the moment of engagement, so engaging causes no jump. Keep roll
  from the right controller's up, made orthogonal.
- Blend that orientation in by `w`. Keep it consistent with the latched sample (`t_sample`): the
  pivot term `R(q) * pivot` uses the basis handed to the engine.
- The rotational seat must not fight it. Freeze `g_seatQ` integration while `w > 0`, or the loop will
  try to turn the right hand back to canonical against the two-hand basis.

### 4. Verify on the mock

- Add a log line with the snap state (`w`, distance to the foregrip, engaged) next to `ev=vr.arms
  state`, and draw the snap target frame in the overlay.
- Put the left controller on the foregrip. The easiest route is a temporary command that logs the
  learned foregrip in the right controller's frame. The mock's controllers are in stage
  coordinates, with the right hand at `(0.2, 1.4, −0.3) + (HandX, HandY, HandZ)` and the left at
  `(−0.2, 1.4, −0.3) + (LHandX…)` (OpenXR: +X right, +Y up, −Z forward), both in neutral
  orientation when yaw, pitch and roll are 0. With the right hand neutral, a foregrip offset of
  `(f, l, u)` in the controller frame (forward, LEFT, up) means
  `LHandX = HandX + 0.4 − l`, `LHandY = HandY + u`, `LHandZ = HandZ − f`, approximately (the palm
  vs aim offset of 7 cm back and 3 cm down applies to both hands alike).
- Cases: engage, hold, release; hysteresis at 0.12 m (no flicker); weapon swap while snapped;
  recoil (fire once) while snapped; sprint while snapped; and, with two-hand aim, moving the left
  hand left and right should swing the barrel while the right hand stays put.
- `Test-Arms.ps1`'s `j_two_hand` is close to a foregrip pose already; extend the matrix rather than
  writing a new script.

## Traps already known

- **Build–test cycle is about 7 minutes**: `build.ps1` and `deploy.ps1` kill the game, then
  `Run-Patrol.ps1` takes about 6 minutes to reach Io and turn VR on. Batch changes, and use the
  running session with command files for anything tunable.
- **The left grip throws a grenade on the mock** (`Set-MockInput -GripL 1`). The screenshot then
  shows the right arm throwing and the grenade icon spent. It is not an arms bug.
- **`near`/`far` are Windows macros.** Do not name a variable `near`.
- **The mock's hands sit low.** With the FOV fixed they are at the bottom edge. Use `Test-Arms.ps1`'s
  raised baseline (`HandY 0.10, HandZ −0.15`).
- **Seat loops can talk themselves into a wrong pose** if they integrate before the gun is on the
  controller. They are gated (palm within 15 cm, steady controller) and reset when stuck for 2 s. Any
  new loop needs the same guards.
- **Bones are rewritten in place at Unmap, not at draw time** (`vr_gpu.cpp` `examine_palette`). The
  draw-time swap was abandoned because it never matched for the view constant buffer. Keep new
  rewriting in the same place.
- **Body piece when looking straight down**: the player's own third-person body, present with the
  arms off. Not ours.
- **Pixel-level verification**: compare captures taken seconds apart, never minutes (lighting drifts
  through Io's day; colours changed visibly within this session).

## Files to read, in order

1. `docs/ARMS.md` — the system.
2. `vr_arms.cpp`: `rewrite()` (targets, IK, the per-bone write), `left_finger()`, `blend_about()`,
   the `g_grip` learning block (the pattern to copy for `foregrip`), `draw_overlay()`.
3. `vr_weapon.cpp`: `hand_pose_for()`, `apply_seat_rotation()`, `xform_replacement()`, `seat_error()`.
4. `vr_gpu.cpp`: only if something about palettes or views looks wrong (`examine_palette`,
   `examine_view`).
