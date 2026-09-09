# The weapon's 6DOF placement — the robust version (2026-09-09)

Supersedes the "fix" section of `HANDOFF.md`. Read `../CLAUDE.md` first, then `RESEARCH.md`
("The weapon's transform builder"). This file is what the weapon's placement IS, why every term is
there, and what is measured about it.

## The one equation

```
weapon_world   = eye_world + (palm_room − head_room)
weapon_orient  = controller_orientation
```

The controller's true position relative to the head, carried into the world. Nothing else. Solving
for what the module has to write into the transform buffer's offset lanes:

```
lanes = (hand.palm − head.offset) + R(q) · pivot
```

- `hand.palm` and `head.offset` are both read from **this frame**, out of one lock acquisition.
- `R(q) · pivot` cancels the engine's own viewmodel offset. `R(q)`'s columns are the basis handed
  to the weapon's getter caller, so it needs no matrix:
  `R(q)·v = v.x·forward + v.y·right + v.z·up`.
- `pivot` is the only constant, three floats in the controller's basis, and it is **measured**.

**There is no captured state.** That is the whole property. It cannot depend on when F9 was
pressed, where the player was looking, or where the stick was — there is nothing stored that could
disagree with the present.

## Why this is the standard model

Native VR games compose one transform tree: a rig root in the world, head and hands as tracked
poses under it, the weapon under the hand by an authored grip offset. Four properties follow, and
they are the four that were missing here:

1. Nothing is relative to anything else. Transforms are *composed*, never differenced against
   remembered values, so no snapshot can go stale.
2. Artificial turning rotates the **root**, so head and hands turn together and turning is
   geometrically incapable of moving the weapon relative to the player.
3. Recentring also moves only the root, leaving relative geometry invariant by construction.
4. One temporal sample per frame: `xrLocateViews` and `xrLocateSpace` are called with the same
   `predictedDisplayTime`, and the result is latched once.

We cannot compose, because the engine's viewmodel model is 3DOF by design — `camera + R(q)·d`,
pivoting on the eye. So we **invert**: `R(q)·pivot` undoes the grip the engine assumes in order to
put ours in its place. The geometry is identical to a native game's; the cost is having to know
`pivot`.

## What was wrong before, exactly

Both of the user's symptoms had closed-form causes. Neither needed more reverse engineering.

### Rolling the wrist swung the gun instead of spinning it

The engine draws at `camera + R(q)·d + lanes` and nothing cancelled `R(q)·d`. So the centre of
rotation was **the eye**, and `|d| ≈ 0.33 m` — 33 cm behind both the gun and the hands. A 90° roll
moved the gun about 0.47 m along an arc. It also explains an older note in `RESEARCH.md` that
`getter D5D832 head` made the gun "swing across the screen": same term.

Fixed by the `R(q)·pivot` term.

### Turning with the stick translated the gun and arms

`vr_weapon.cpp` captured `handRef` as a snapshot of `hand.offset`, which is published **already
folded by the room anchor**. So the delta expanded to

```
( R_z(fold(t)) − R_z(fold(t₀)) ) · h_room
```

Hold the controller still, turn the stick, and `fold` changes while `h_room` does not — phantom
translation out of nothing, reaching `2·|h_room|` at 180°. For a naturally held controller that is
**over a metre**, which is exactly why the arms appeared to recede far enough to expose the cut
where the torso would be.

Fixed by deleting the snapshot. The fold is inside both `hand.palm` and `head.offset`, so it
cancels in the difference.

## Three robustness holes that were not in the handoff

Found by reading the code rather than from the headset, and all three are invisible in the mock as
it was. These are the likely other half of "en muchísimos casos se rompe".

### 1. Lost tracking made the weapon jump

The old code required `hand.valid && head.valid` and otherwise skipped the offset entirely, so the
gun **snapped** to the engine's default viewmodel spot and snapped back on recovery. A Quest 3
controller loses tracking several times a minute — resting against the body, out of the cameras'
cone, pointed at the player.

Now `xr_runtime.cpp` holds the last good pose. The part that matters as much as the holding: it
holds the **raw, unfolded** `XrPosef` and re-folds it with the current anchor every frame. Holding
a folded pose would have reintroduced symptom 2 in miniature every time tracking blinked.

### 2. The orientation and its correction came from different frames

`getter_replacement` and `xform_replacement` each called `xr::hand_pose(true)` separately. `R(q)`
comes from the getter's sample; the `R(q)·pivot` correction is computed in the xform. If the XR
frame published between the two, the cancellation was computed against a different orientation than
the one the engine was given, leaving a residue the size of one frame's hand movement — felt as
jitter, and only once the cancellation exists at all.

Now the getter latches the exact `(head, hand)` sample it used into a `thread_local`, and the xform
detour consumes it. The pairing is guaranteed rather than hoped for: the transform builder
`+0xD5D7F0` **calls the pose getter itself**, so on that thread the getter always runs first and
the xform detour immediately after. One writer, one reader, same thread, no lock. `xr::frame_sample`
also copies head and both hands under one lock, so the head and hand can never straddle a publish.

### 3. The offset was applied to every caller of the transform builder

`+0xD5D7F0` is detoured wholesale and the offset was applied unconditionally. `xform caller
all|<rva>[,...]` now gates it; the default is still all, because that is the configuration measured
to work, and the `report` counters say whether there is more than one caller to narrow to.

## `aim` versus `grip`: the pose was wrong

The module bound `/user/hand/right/input/aim/pose`. OpenXR defines two poses per hand and they are
not interchangeable:

- **`aim`** — the origin of a pointing ray, for pointers and menus. On Touch hardware it sits
  **in front of** the controller, in mid air.
- **`grip`** — the palm centroid, with a standardised orientation. Literally "where you are
  holding it".

Anchoring the weapon to `aim` puts the pivot at a virtual point ahead of the hand, so **the roll
would still feel wrong however well `pivot` were tuned** — a second, independent cause of symptom 1.

Now: **`grip` for position, `aim` for orientation.** `aim` is defined as the pointing ray of a
gun-like hold, which is exactly what a weapon's forward axis wants, and the grip pose's own
orientation is a different convention (thumb-up along its axis, not along the ray) — taking
`forward` from it would silently re-aim the weapon by about 90°.

A robustness bonus: palm→weapon-grip is a few centimetres and nearly weapon-independent, while the
engine's `d` (eye→viewmodel) varies a lot per weapon. Switching to `grip` shrinks the part that has
to be calibrated to the stable part.

`anchor palm|aim` switches it live, so the difference is measured rather than argued about. The
grip spaces are allowed to fail on their own: a runtime offering only aim poses still gives head
tracking and a pointable weapon, and `hand.palm` falls back to `hand.offset` so no consumer has to
check.

## What was measured, in Io, on 2026-09-09

The headline numbers, and what each one is worth.

| Result | Value | Instrument |
| --- | --- | --- |
| **Artificial turning no longer moves the weapon** | `turn_90` 0/-1 px, **`turn_180` 0/0 px**, `turn_back` 0/-1 px | template match, peaks 0.97-0.99 |
| Scene noise floor for those | **0 px** | two captures seconds apart, peak 0.99 |
| **A wrist roll no longer translates the weapon** | 72.0 px -> **0.0 px**, both signs exactly zero | roll-tolerant match, peaks 0.78/0.82 |
| The pivot constant | **(-0.3300, -0.0915, 0.1090) m**, \|pivot\| = **0.359 m** | least-squares solve on roll, two passes |
| Cross-check on that constant | 0.33 m in `RESEARCH.md`, by an unrelated route | agreement to 9% |
| The lanes the module writes | 0.1499 / 0.1500 / 0.1500 m for a 0.15 m pivot on each axis | transform dump |
| Head and hand sample pairing | `latch_missed=0` on every frame observed | log |
| Grip really is a different pose from aim | `r_off` and `r_palm` 7.7 cm apart | log |
| Callers of the transform builder | exactly one, `D5896F` | detour counters |
| The idle animation is not moving this weapon | 0 px over 20 s; 0 px across firing (ammo 13 -> 12) | template match, peaks 0.97-0.99 |

Turning is the one that mattered most, and it is the cleaner result: 180 degrees of artificial turn
with the hand held still moves the weapon by **nothing**, against a noise floor of nothing. That is
the bug that reached over a metre of phantom translation and exposed the cut where the torso would
be.

### What is NOT settled

- **Yaw and pitch of the wrist still measure 30-134 px** of apparent translation. Part of that is a
  known instrument bias rather than weapon travel -- see below -- and the two cannot be separated
  with this instrument. Roll is the axis that is clean, and roll is nulled.
- **The final seating** -- whether the grip falls where the hand actually is -- cannot be judged
  here. The mock's synthetic hand sits 0.3 m in front of the eye and 0.2 m below it, closer and
  lower than anyone holds a controller, and with the pivot correct the weapon's origin sits ON the
  palm: at that hold it lands below the field of view entirely. It looked like the pivot was
  throwing the gun out of frame; it was the fixture. This is a perceptual judgement and needs the
  headset, as flagged from the start.
- **Whether `d` is constant across weapons and animations.** The pivot cancels one fixed offset. If
  a different weapon or aiming down sights moves the engine's viewmodel, the cancellation is only
  exact for the pose it was measured in. Specific, testable prediction: the roll null holds at rest
  but returns in ADS or with another weapon. The user also reports a small periodic weapon animation
  even outside the idle pose, which would show up as occasional millimetre-scale noise.

## How `pivot` is measured

Not guessed, and not eyeballed. The quantity being driven to zero is an **affine** function of the
thing being tuned, which makes it a linear solve rather than a search.

Rotate the controller with its position held, from `R1` to `R2`. The weapon's world displacement is

```
D = (R2 − R1) · (pivot + d)
```

zero exactly at `pivot = −d`. The screen displacement is a projection of that, so it is affine
too, locally, and vanishes at the same point. So:

1. Measure the residual screen displacement at the current `pivot`.
2. Probe each of the three axes by 0.12 m and measure again — that is the Jacobian by finite
   differences.
3. Least-squares solve for the correction, apply, and iterate once.

**Roll, and only roll.** This is the part that took the longest to get right, and the reason is
worth keeping:

- A **roll** turns the weapon in the image plane and PRESERVES its silhouette, so cross-correlation
  (searching template rotations of 0 and plus/minus the roll angle) reports the translation and
  nothing else.
- A **yaw or pitch** foreshortens the gun. Correlation then answers with the offset that best fits a
  CHANGED shape, which on this scene is worth about **100 px with no translation behind it**. That
  is the same size as the effect being measured, it is constant in the pivot, and between `p = 0`
  and `p = -|d|` the true signal is linear in `p` -- so a fit over that range cannot separate the
  slope from the bias. Crossing the vertex would separate them, but that pulls the weapon towards
  the eye until it leaves the frame.

So yaw and pitch can say the pivot got better; only roll can say it is right.

Roll is blind to the component along its own axis, exactly as a spinning rod cannot reveal its own
length. So the forward component is taken from `RESEARCH.md`'s independent measurement and the two
perpendicular ones are solved from roll. `Solve-PivotRoll.ps1` does it: three measurements give the
2x2 Jacobian by finite differences, one linear solve gives the answer, and a second pass lands on
it. Both signs of the roll are averaged, because any residual shape bias is roughly odd in the
angle and averaging cancels most of what is left.

Two routes that did NOT work, recorded so they are not retried:

- **Least squares over all three axes in screen pixels.** The pivot's forward axis turns out to
  point along the camera's own view direction -- of course it does, the hand points where the player
  looks -- so moving it changes the weapon's DEPTH. Measured: 0.15 m forward moved the gun 88 px
  where the same 0.15 m sideways moved it 253. The Jacobian was near singular in exactly the axis
  being measured.
- **A one-dimensional scan of the yaw residual.** Defeated by the foreshortening bias above.

Iteration is expected rather than a fallback: pixels-per-metre depends on how far the gun is from
the eye, which changes as `pivot` changes, so the linearisation is only good near the current point
— but the error shrinks with the displacement.

**Small angles on purpose.** `weapon_shift.py` template-matches, and template matching is not
rotation invariant: a big roll turns the gun's image enough to spoil the correlation peak, and the
peak's position *is* the measurement. 15° on a 0.33 m lever arm is about 180 px — unmistakable —
while leaving the match trustworthy. The peak is reported for every measurement so a weak match is
visible rather than silent.

`pivot_solve.py` was validated against synthetic data before being pointed at the game: from
pixel-rounded measurements of a known ground truth it recovered the answer to **0.8 mm**, rank 3.
It is kept for the three-axis case; `Solve-PivotRoll.ps1` is what actually produced the number.

**The independent check that the number is real**: `|pivot|` should land near **0.33 m**, which was
measured by a completely different route (0.08 m of lane travel subtending 13.5°). Agreement means
the constant is the engine's actual viewmodel offset and not a fudge that happens to cancel one
test.

## The tests

`scripts\testing\Test-WeaponPivot.ps1`. The old suite passed with both bugs present, so it was
incomplete in two specific ways — both now covered:

| Check | What it holds still | What must not happen |
| --- | --- | --- |
| `roll_*`, `yaw_*`, `pitch_*` | hand position, head | the weapon must not translate |
| `turn_90`, `turn_180` | hand, head | the weapon must not move on screen |
| `turn_then_roll` | hand position, at 180° of turn | either bug alone would show here |
| `turn_back` | — | the anchor returns and so does the weapon |
| `hand_*`, `head_*` | — | the weapon must still MOVE (see below) |

The sanity section is not optional: with the pivot cancelled it is trivially easy to pass the two
regressions by breaking the response altogether, so the last block requires the weapon to move when
the hand really moves, and to stay in the **world** when the head moves.

The bar for "did not move" is three times the scene's own noise floor, measured in the run itself
from two captures taken seconds apart, floored at 25 px (~0.012 m at the weapon's distance).
**Compare captures taken seconds apart, never minutes** — Io's lighting drifts and the weapon idles,
so a whole-frame metric across a session has a noise floor the size of the effects being looked for.

### Three ways the instrument lied, and how each is now guarded

All three produced numbers that looked exactly like measurements. That is what makes them worth
recording: none of them announced itself.

- **Captures of the wrong window.** `Save-Shot` grabs the desktop, and when the game was not in
  front the shot was of the editor. The template matcher then compared the weapon against a code
  window and reported a 582 px shift at a correlation peak of 0.04. `Focus-Game` had always
  returned whether it succeeded and every call site discarded it with `[void]`. Now
  `Assert-GameFocus` retries and **throws**, naming the window that is in front instead. It also
  falls back to `Focus-GameHard`, which borrows the foreground thread's input queue -- Windows
  grants `SetForegroundWindow` only to a process that already holds the foreground or has recently
  received input, and a script host has neither, so the plain call works when the game happens to
  be in front already and is silently refused otherwise. Exactly the intermittent pattern seen.
- **A template box that no longer held the weapon.** The absolute placement moves the weapon's rest
  position, and `weapon_shift.py`'s box was authored around the engine's own viewmodel spot. A box
  containing only scenery returns a confident **0 px at peaks up to 0.97** -- which reads as a
  perfect PASS. Three points of one pivot scan were false zeros this way, and an earlier conclusion
  that "roll never translates the weapon" rested on them. The box is now settable through
  `SVR_TEMPLATE_BOX`, and any match below a minimum peak is refused rather than returned.
- **Foreshortening masquerading as translation.** Covered above: about 100 px of it under yaw, the
  same size as the effect. Roll is the axis that does not suffer from it.

Two harness gaps had to be closed before any of this could be measured:

- **The mock had no roll.** `handRoll`/`lhandRoll` are appended to `SVR_MockInput.txt` (and to
  `Set-MockInput`), and the mock now composes `q = yaw·pitch·roll` properly rather than by the
  two-axis shortcut. Roll is the axis the user noticed, so without this the main symptom was
  untestable.
- **The mock returned one pose for both spaces.** It now marks `palm_*` action spaces and offsets
  them 7 cm back and 3 cm down in the controller's own frame, as real hardware does. Otherwise
  confusing `aim` for `grip` is undetectable in the mock and only shows up in the headset.
- **Nothing could turn the room anchor exactly.** `turn <degrees>` in `SVR_Weapon.txt` calls
  `turn_room` directly, so the decisive regression runs to a known angle instead of holding a
  synthetic stick for a guessed length of time.
- **The weapon's idle pose.** After a while without input the weapon settles into an idle animation
  that points it upwards, and one click of fire returns it to the normal pose. Measuring in one
  pose and comparing against the other would be a large error correlated with how long the session
  had been sitting. `Clear-WeaponIdle` fires one shot before anything is measured. Measured on this
  build with the weapon already out of idle: 0 px over 20 seconds and 0 px across firing (ammo
  13 -> 12, so the shot happened), so this is insurance rather than a fix for a moving target.

### The scripts

| Script | What it does |
| --- | --- |
| `Test-WeaponPivot.ps1` | the two regressions, plus sanity checks that the weapon still moves |
| `Solve-PivotRoll.ps1` | **produced the pivot constant**: solves the two roll-visible axes |
| `Probe-PivotVisual.ps1` | blends a level and a rotated frame so the centre of rotation is visible |
| `Probe-PivotGain.ps1` | proves the lanes move by exactly the pivot, separating our write from the engine's use of it |
| `Scan-Pivot.ps1` | the one-dimensional scan; kept because its failure is instructive |
| `Probe-Pivot.ps1` | reconnaissance: is the weapon on screen and inside the template box |
| `pivot_solve.py`, `pivot_fit.py`, `pivot_sheet.py` | the three-axis solve, the V fit, the blend sheet |

## Commands (`SVR_Weapon.txt`)

New or changed:

| Command | Effect |
| --- | --- |
| `pivot <fwd> <right> <up>` | the pivot correction, in the controller's basis, metres |
| `anchor palm\|aim` | which controller pose anchors the weapon's position |
| `turn <degrees>` | turn the room anchor by an exact angle (test lever) |
| `xform caller all\|<rva>[,...]` | which callers of `+0xD5D7F0` get the offset |
| `xform delta on\|off` | absolute is the only mode now; `abs`/`rel` still parse |
| `xform ref` | no-op — there is no reference left to retake |

`F7` (recentre) no longer retakes anything, and that is the fix rather than an omission: the
placement is a pure function of this frame's poses, so a recentre moves the shared origin and head
and hand move with it.

## Still open

- **Is `d` a single constant?** Almost certainly not across weapons, and animations (ADS, reload,
  sprint) may move it. A native game has no `d` at all, so this risk is ours alone. It has a
  specific, testable prediction: if the roll stops translating at rest but starts again in ADS or
  with a different weapon, `d` is animated and has to be read live rather than baked. Measure it
  after the fix, not before.
- **The arms.** Deliberately out of scope for now. Note that `HANDOFF.md`'s claim that "the arm and
  gun are one draw" is **not measured** — there is no draw-call instrumentation in the codebase at
  all. And the arms *stretching* is evidence against it: a rigid transform would move them
  rigidly, so something is already anchoring the arm root near the camera while the hand follows
  the weapon. If they are ever worth attacking, the one probe that unlocks everything is the
  viewmodel draw's bone matrix palette (identifiable by searching per-draw constant buffers for the
  32 bytes this module writes) — that answers "can the arm be told from the gun", enables hiding
  them, and would be the only route to anything IK-like. Aim for invisibility, never for IK: the
  first-person arms are authored animation with no IK rig to feed.
