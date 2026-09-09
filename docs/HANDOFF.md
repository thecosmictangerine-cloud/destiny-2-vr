# Handoff — the weapon's pivot is wrong, and why (written 2026-09-09, after the first Quest 3 run)

> **Superseded on 2026-09-09, later the same day.** The diagnosis below is correct and worth
> keeping — it is how the two symptoms were explained — but the fix has been implemented, and
> **`WEAPON-6DOF.md` is now the description of what the weapon's placement IS**. Read that for the
> current state. This file survives as the reasoning that got there, plus the sections at the end
> ("What is solid", "Real Quest 3 numbers", "Still unanswered from the headset") which are still
> current. Three robustness holes not diagnosed here — lost tracking, a two-frame sample tear, and
> an ungated detour — plus the fact that the module was using OpenXR's `aim` pose where it needed
> `grip`, are all in the new file. The claim under "Not in scope" that the arm and gun are one draw
> is **not measured**; see the new file's last section.
>
> **Measured after the fix, in Io:** artificial turning at 180 degrees now moves the weapon by
> **0 px** against a 0 px noise floor -- symptom 2 is gone. A wrist roll's translation went from
> 72.0 px to **0.0 px** with the pivot constant solved at **(-0.3300, -0.0915, 0.1090) m**, whose
> magnitude of 0.359 m agrees to 9% with the 0.33 m this file quotes from an unrelated route.
> Symptom 1 is nulled on the axis the user named; the yaw and pitch axes cannot be separated from a
> ~100 px foreshortening bias in the instrument, and the final seating needs the headset.
>
> **Validated on the Quest 3 through Link, 2026-09-09: the user's verdict is "el 6DOF va perfecto".**
> F3 is done. Two separate faults were found in that session and are written up in
> `BODY-SERVO-HANG.md`: the body servo's gain learner ran away and stalled the game, and switching
> the module off left the headset frozen.

Read `../CLAUDE.md` first (setup, build loop, gotchas), then `RESEARCH.md` — the sections "The
weapon's transform builder" and "Mono, and the double vision it was blamed for" are what this work
stands on — and `ZONES.md` for landing alone in a patrol zone.

**Where things stand.** The weapon moves in six degrees of freedom and it is genuinely decoupled
from the head; that much is measured, reproducible, and holds up (see "What is solid" below). But
in the headset it feels wrong, and the user's two symptoms both have exact arithmetic explanations.
Neither is a mystery and neither needs more reverse engineering. **Both are fixed by the
architecture the user asked for**, which is also simpler than what is there now.

## The two symptoms, and what causes them

### 1. Rolling the wrist swings the gun instead of spinning it in place

The user: *"al rotar la muñeca, tipo roll, esperarías que el centro de rotación fuera el arma... pero
el arma también rota pero moviéndose, como si el centro no estuviera ni en los brazos ni en el
arma."*

**The centre of rotation is the eye.** The engine draws the weapon at

```
weapon_world = camera_position + R(q) · d + offset_lanes
```

where `q` is the quaternion it derives from the (forward, up) pair the module hands to getter caller
`D5D832`, `d` is its own fixed viewmodel offset, and `offset_lanes` is the world-space offset the
module writes into lanes 4-6 of the transform buffer. `R(q) · d` is the term nobody is cancelling.

`|d|` is already measured at **0.33 m** (`RESEARCH.md`: 0.08 m of lane-4 travel subtended 13.5°).
So supplying a different orientation swings the weapon along a 0.33 m arc centred on the eye. A 90°
roll moves it about 0.47 m. That is precisely "the centre is neither in the arms nor in the gun" —
it is 33 cm behind both, at the eyeball.

It also explains an older observation in `RESEARCH.md` that was recorded without being understood:
`getter D5D832 head` made the gun "swing across the screen" rather than rotate in place. Same term.

### 2. Turning with the joystick translates the gun and arms

The user: *"si giro unos 180 grados vía joystick... sí que veo el corte abrupto de los brazos a nada,
o sea como si el arma+brazos se hubieran alejado."*

**The weapon's rest reference is stored in the wrong frame.** `xr_runtime.cpp` publishes the hand
offset already folded by the room anchor:

```
hand.offset(t) = R_z(fold(t)) · h_room        fold = roomAnchor − originYaw
```

`vr_weapon.cpp` captures `handRef` once, as a snapshot of that **already-folded** value, and then
uses `hand.offset(t) − handRef`. Expand it:

```
delta(t) = ( R_z(fold(t)) − R_z(fold(t0)) ) · h_room
```

Hold the controller perfectly still and turn the joystick, and `fold` changes while `h_room` does
not — so `delta` grows out of nothing. At 180° of artificial turn the two rotations oppose and the
spurious translation reaches `2 · |h_room|`. For a naturally held controller (roughly 0.35 m ahead,
0.25 m right, 0.3 m below the eyes, so `|h_room| ≈ 0.53 m`) that is **over a metre** of phantom
displacement. Which is exactly why the arms appear to recede far enough to expose the cut where the
torso would be.

Note this one is invisible in the mock tests as written: none of them turns the joystick while
holding the hand still, so nothing ever changed `fold` mid-test. That gap is a test to add, not just
a bug to fix.

## The fix: the architecture the user asked for

*"la posición del arma (el arma en sí, no los brazos) es justamente la posición del control tal y
como yo estoy físicamente, tipo donde tengo la cámara y donde tengo el arma respecto la cámara."*

That is the right model, and stating it as one equation makes both bugs disappear:

```
weapon_world  = camera_world + R_z(fold) · (hand_room − head_room) · unitsPerMetre
weapon_orient = R_z(fold) · hand_room_orientation
```

The controller's true position relative to the head, carried into the world. No rest reference, so
symptom 2 cannot happen — there is no stale snapshot left to go out of date. Solving for what the
module must write:

```
offset_lanes = R_z(fold) · (hand_room − head_room) · unitsPerMetre − R(q) · d
```

and the `− R(q) · d` term puts the pivot back on the gun, which fixes symptom 1.

The compensation is cheap to express, because `R(q)` is the rotation whose rows are the basis the
engine built from what we handed it — so

```
R(q) · d  =  d.x · hand.forward + d.y · hand.right + d.z · hand.up
```

and `HandPose` already carries all three vectors. It is one three-float constant in the hand's own
basis. (Convention caveat: it may be the transpose, i.e. the dot products rather than the
combination. Do not reason about it — make it live-tunable and measure, as with the lanes.)

### Steps

1. **Store the rest reference unfolded, or drop it entirely.** Dropping it is better and is what
   The user asked for: the gun then sits where their real controller is, not at the engine's tidy
   viewmodel spot. Keep `xform delta abs` (already implemented) as the switch, make it the default,
   and delete the relative path once the absolute one is confirmed. If a nudge is still wanted for
   comfort, it belongs as an explicit constant, not as a captured snapshot.
2. **Add a `viewmodel_offset x y z` command** that subtracts `d.x·forward + d.y·right + d.z·up`
   from the lanes, live-tunable through `SVR_Weapon.txt` like everything else in that module.
3. **Measure `d`** by tuning until pure rotation stops translating the gun. `|d|` should come out
   near 0.33 m; that is the check that the number is real and not a fudge. Then keep it as the
   default.
4. **Verify, then hand it back for a headset session.** The new tests below are the ones that would
   have caught these.

### Tests that must exist before this is believed again

The current suite passed while both bugs were present, so it is incomplete in two specific ways:

- **Pure rotation must not translate.** Hold the hand position fixed, sweep the orientation, and
  require the gun's screen position to stay put. `weapon_shift.py` measures it directly and reads
  `(0, +8)` at peak 0.952 on a null pair, so the instrument is good enough. **The mock cannot do
  roll**: `Set-MockInput` has `HandYaw` and `HandPitch` only, and roll is the axis the user noticed.
  Add a `HandRoll` field to `mockxr/mock_runtime.cpp` and to `Set-MockInput` — without it this
  cannot be tested at all.
- **Turning the joystick must not translate.** Hold the hand still, drive `TurnX` (or call
  `turn_room` directly), and require the gun's screen position to stay put relative to the world.
  This is the test that symptom 2 walked straight through.

## What is solid, and should not be re-derived

All measured in Io Giant's Scar, reproducibly, and none of it is in question:

- **The transform buffer's layout.** Lanes 0-3 a unit quaternion of the camera rotation, lanes 4-6 a
  world-space offset in the game's own axes, lane 7 the homogeneous 1. Identified by forcing one
  float at a time and template-matching the weapon: 166 px for 0.08 on lane 4, 172 px on lane 6, a
  depth change on lane 5. Full working in `RESEARCH.md`.
- **A game unit is a metre**, from 0.08 m subtending 13.5°.
- **The pose handed to `D5D832` carries orientation only** — its position field is never read by
  `+0xD5D7F0`. Do not try to move the weapon through it again.
- **The render view's position comes from getter caller `B363FA`, not from the camera block.**
- **Controller pose conversion**: 20/20 checks against hand-computed values, every axis and both
  rotations in both signs, both hands independent, head and hand independent.
- **Weapon decoupling in principle**: the same 0.09 m of travel gives +271 px by the hand and
  −286 px by the head. Equal and opposite, which is the decoupling. The *magnitude* is right; it is
  the pivot and the frame that are wrong.
- **Camera and body**: the character follows the gaze to 0.42° residual with a learned 579
  counts/rad, and the horizon does not move while it does — 4.70 mad against a 5.36 noise floor,
  i.e. quieter than doing nothing. Walking follows the gaze to 17.6°.
- **F9 alone configures everything.** No command file needed for a normal run.

## Real Quest 3 numbers, finally measured (2026-09-09)

From `ev=vr.xr eyes` in `evidence/2026-09-09-quest3-headset-session-pivot.log`. These retire two
guesses and correct the mock:

| | Left eye | Right eye |
| --- | --- | --- |
| position | `-0.0715, -0.0470, -0.2464` | `-0.0339, -0.0371, -0.1982` |
| orientation | `0.01031, -0.43299, 0.09492, 0.89633` | **identical** |
| FOV L/R/U/D (rad) | `-0.9425, 0.6981, 0.7679, -0.9599` | mirrored horizontally |

- **The Quest 3's panels are not canted.** Both eyes report the same orientation, so the cyclopean
  *orientation* change was harmless but unnecessary. The cyclopean *position* is the part that
  mattered: the eyes are 62 mm apart (measured from the position delta), and declaring one image at
  two positions is what could produce double vision.
- **The real FOV is asymmetric vertically too**: 44° up against 55° down, where the mock assumes
  48/-50. Horizontal is -54°/+40°. So `SVR_MockHmd.txt` should be `1824 1968 -54 40 44 -55` to match
  the hardware, and the module publishes a 108° symmetric horizontal frustum with aspect 0.964,
  declaring 110° vertically against the 99° the eye has. Covered.
- The Meta runtime triple-buffers the swapchain (`images=3`); the mock hands out two.

## Still unanswered from the headset

The user's session went on the weapon, so these are still open and cannot be answered from here:

- **Is the double vision gone?** The cyclopean pose is in and the eye data above says the cause was
  the position difference, but nobody has looked yet.
- **Are the black bands gone?** `aspect` is written and reads back as `1.0740` in the mock; on
  hardware it should be about 0.964.
- **Is 1920x1080 sharp enough?** 2560x1440 works and is 4× the pixels of 720p, but puts the
  Director's LAUNCH button out of the mouse's reach.

## Not in scope, unchanged

- **The arms.** Still visible, and they stretch when the controller travels far. The cheap avenues
  came up empty: Sunrise has nothing about first-person arms in its source, and the arm and gun are
  one draw. The user has deprioritised it twice. Note that fixing the pivot will change how the
  stretching looks, so judge it again afterwards rather than now.
- **Aim.** Bullets still leave the character. It got cheaper — the character already faces the gaze
  — but it needs a decision with the user first.
- **One frame of camera lag.** The OpenXR frame runs on the present thread.

## Housekeeping

- Branch `vr` holds the mod; `main` holds these docs.
- `Run-Patrol.ps1 -Runtime meta` launches against the Quest through Link. The registry's
  `ActiveRuntime` is SteamVR on this machine, so a launch that sets nothing lands there — the
  `XR_RUNTIME_JSON` override is not optional.
- `SVR_Destination.txt` is renamed to `.off`, so the Director is back.
- Window is 1920x1080 at `0,0`; the original cvars are beside them as `cvars.backup-vr.xml`.
- Measurement lesson that cost the most time: compare captures taken **seconds** apart against a
  null control pair, never minutes apart. Io's lighting drifts and the weapon idles, so a whole-frame
  metric across a session has a noise floor of tens of pixels — the same size as the effects being
  looked for. It produced two wrong conclusions before it was caught. `weapon_shift.py` template
  matches instead and reads `(0, +8)` at peak 0.952 on a control pair.
- Test amplitudes: the full matrix uses 0.3 m, which saturates (42° of swing, gun off frame).
  `-Scale 0.3` is what gives measurable, legible numbers.
