# ADS — aiming down the sights, physically (2026-10-15)

**Goal agreed with the user**: aim the way a native VR shooter does (Pavlov, Onward, Contractors,
H3VR, Alyx) — the sight is used by bringing it to the eye, the world never zooms, the sight tells the
truth about where the round goes, and the gun never moves in the hand on its own — while keeping what
Destiny's ADS gives the shot (accuracy, handling, aim assist).

Decisions (user, 2026-10-15, "go with your recommendations"):

1. **The sight rules the shot**, in and out of ADS: the round leaves the muzzle along the learned sight
   line, zeroed at a fixed distance (25 m, configurable).
2. **The sight is learned from the game's own ADS pose**, per weapon, persisted.
3. **Drawn = fired through the ADS transition**: the game's ADS animation is cancelled, so the gun
   does not slide in the hand when ADS starts or ends.
4. **Dominant-eye render switch** (`eye center|left|right`, F6), for the headset to decide.
5. **A light haptic tick on entering ADS**, switchable.
6. **The grenade moves to the left trigger**, ignored while the left hand holds the gun; the left grip
   is only the hold. A hidden `mode button` keeps the old mapping as a fallback.

## 1. What the reconnaissance found (mock, Io, hand cannon)

- **The zoom never reached the image.** The handoff feared it did. ADS narrows the engine's FOV
  (`in_fov` 1.4242 -> 1.0173 rad, x1.55 zoom), but the camera hook writes the VR FOV after the engine
  every frame, and a GPU capture held in ADS shows the world at P00 = 0.7813 = 1/tan(104°/2), exactly
  as at the hip. What the earlier session took for a zoom was the next two items.
- **ADS blurs the periphery** (`dof_mode=2` in `cvars.xml`): edge sharpness dropped to a third with
  the centre untouched — a head-fixed blur in a headset. `dof_mode=0` now (backup
  `cvars.backup-dof.xml`).
- **A red dot appears at the exact centre of the frame in ADS.** It is not the HUD (hiding the whole
  UI composite leaves it) — a head-locked dot that points where the gun does not. Hidden; see §5.
- **The ADS animation throws the gun at the face.** 300 ms into ADS the gun was drawn a hand's
  length up and towards the camera; the self-seating loop dragged it back over about a second. This
  is the "gun moves on its own" the decisions forbid, and the main thing this work had to remove.
- **The engine's FOV is the ADS progress.** It narrows in step with the ADS blend, and the camera hook
  reads it before overwriting it — the clock the animation compensation runs on.
- Also: **the game returns an idle player to orbit** ("You have been returned to orbit because you were
  inactive") after several minutes with no input; a long scripted session on the mock with a still
  head hits it.

(Measurements of the animation, the compensation and the detector: §4.)

## 2. The model: two poses per weapon

Destiny animates the hand (and the gun in it) in the viewmodel's LOCAL frame — the frame the weapon
module hands the engine: rotation = the basis given to the weapon's pose getter, origin = the transform
buffer's offset lanes. In that frame each weapon has:

- a **hip pose** `H` of the hand, and
- an **ADS pose** `A`, in which Bungie's animation has put the sight exactly on the flat camera's axis:
  the line through the flat camera (`eyeLocal`, the local origin, since the engine's own lanes are 0)
  along local +X.

`vr_ads.cpp` learns both while the pose is still: the hip pose at the hip FOV, the ADS pose at the zoom
plateau. From them:

- **the ADS move** `D = A * H^-1` — what the animation does to the gun;
- **the sight line in the gun frame**: `H * A^-1 * (eyeLocal, +X)`. The gun frame is the one in which
  the gun holds its hip pose, i.e. where the weapon module means the gun to be.

**What stands for the gun: the weapon's own palette, not the hand.** The first version used the drawn
right hand. Measured through an ADS cycle of the hand cannon, the weapon palette's root bone moved
106 mm and turned 1.9° in the local frame while the hand moved 110 mm and turned 4.1–4.4°: the game
re-grips the hand by about 2° on the way to ADS. A sight learned from the hand carries that error, and
a compensation that holds the hand lets the gun turn in it. `proxy auto` (the default) uses the
weapon's root whenever its palette is seen (a skinned palette of a few bones next to the hand, read at
Unmap like the arms'; its root sits exactly on the camera axis in ADS, y = 0.000), and the hand only
for a weapon without one. The re-grip is left to the hand: it moves 2° on the gun, which is what a hand
does when it settles into a sight picture.

**The ADS curve.** The FOV and the animation are not eased alike. On the hand cannon the animation is
77% of the way to ADS when the FOV is 54% of the way; compensating by the FOV fraction left a 25 mm,
1.1° slip at the middle of the transition. But the relation is the SAME going in and coming out
(f = 0.535 -> 77% in, f = 0.557 -> 80% out), so it is a fixed curve, learned per weapon: 16 bins of
animation progress against FOV fraction, filled from every frame in mid-transition (the pose's
projection on its own hip->ADS path), kept monotonic, saved with the profile.

No table, no hand-measured number per weapon: the same move that found the foregrip
(`docs/TWO-HANDS.md`) — let the game's own animation say where things are.

Weapons are told apart by their hip pose (within 2.5 cm and 8°). A steady hip pose that matches no
profile starts a new one after a second; it is written to `SVR_AdsProfiles.txt` (game directory) only
once it has been seen in ADS, so a sprint pose never reaches the file. A weapon swap (Y) distrusts the
profile in hand until its hip pose is matched again; nothing is compensated meanwhile.

## 3. The three uses

**Cancelling the ADS animation** (`ads::compensation`, applied in `vr_weapon.cpp hand_pose_for`). The
engine's ADS fraction `f` comes from its FOV: `zoom = tan(hip/2)/tan(fov/2)`, `f = (zoom-1)/(zoomAds-1)`.
The gun's pose at the animation's progress `curve(f)` is interpolated the way an animation blend does
(lerp the position, slerp the rotation), `D(f) = pose(curve(f)) * H^-1`, and the frame handed to the
engine is turned by its inverse:

```
B' = B * D(f).m^T                      basis handed to the getter
lanes = (palm - head) + B * pivot - B' * D(f).t      offset lanes
```

so the engine draws `lanes + B' * (D(f) * H) = (palm - head) + B * pivot + B * H` — exactly the hip
placement. Same frame, no lag: the FOV is read in the camera hook, which runs before the weapon is
placed. The gun frame everything else reads (the shot, the detector, the arms) stays `B`.

While it compensates, the arm module's learning loops are frozen (`vr_arms.cpp`, `inAds`): the
positional and rotational seats, the grip and the foregrip learn from the drawn hand, and in ADS the
hand re-grips; chasing that would turn the gun the compensation is holding. A weapon not yet learned
is left to the seats as before (they drag it back into the hand over about a second).

**The shot** (`vr_aim.cpp shot_geometry`). With a learned sight, the round leaves the muzzle towards
the point of the sight line at the zero distance (25 m), and the fire basis is the gun's turned by the
smallest rotation that does it. Sight 2–4 cm above the bore: the error is under 0.1° beyond a few
metres and zero at 25 m — below the weapon's own spread. `sight off` fires along the bare barrel.

**Physical ADS** (`ads::present`, every frame). In the frame the image is rendered in, whose origin IS
the eye: `r` = distance from the eye to the sight line, `gaze` = angle between the line and the head's
forward, `reach` = how far ahead the hand is along the line. Enter when `r < 4 cm`, `gaze < 12°`,
`reach > 8 cm` for 60 ms; leave when `r > 6 cm` or `gaze > 20°` for 150 ms. While in, right mouse is
held (`vr_gamepad.cpp`), so the game's ADS — spread, handling, aim assist — applies. Every enter and
exit logs `zoom_check`: the world projection the GPU received against the FOV written (`ok` / `LEAK`).

Until a weapon's sight is learned the detector uses a guess (parallel to the barrel, 9 cm above the
palm) — enough for the first ADS to happen and teach the real one.

## 4. Verification (mock, Io, 2026-10-15)

**Learned profiles** (`SVR_AdsProfiles.txt`, all three loadout weapons, taught with `Teach-Weapons.ps1`):

| weapon | palette bones | ADS zoom | ADS move / turn (local) | sight vs gun forward |
| --- | --- | --- | --- | --- |
| hand cannon | 8 | 1.548 | 106 mm / 1.9° | 1.2° left, 0.5° up |
| auto rifle | 3 | 1.810 | 130 mm / 0.2° | 0.06° |
| grenade launcher | 6 | 1.588 | 161 mm / 4.8° | 3.1° left, 3.5° up |

Identity needed both the bone count and a tight pose match (1.2 cm, 3°): the rifle and the grenade
launcher hold the gun 1.6 cm / 4.7° apart, and a looser match merged them (the rifle's sight drifted
4° before it was caught). The weapon's palette is the one with the MOST bones near the hand: "nearest"
alternated between the launcher's 1- and 6-bone palettes and split it in two.

**The ADS animation** (hand cannon, controller still, `ads_trace.py`), gun relative to controller:

| compensation | worst frame |
| --- | --- |
| none | 83 mm |
| FOV fraction, hand stand-in | 25 mm, 1.1° |
| learned curve, gun stand-in | **4.8–5.4 mm, 0.25–0.29°**; under 1 mm held in ADS |

The residual is single frames inside a 220 ms transition, where the curve is steepest.

**`Test-Ads.ps1`: 14/14.** Aligned -> engine in ADS (zoom 1.548); 3 cm off stays (band); 7 cm off
leaves; 5 cm from the hip does not enter; 3 cm from the hip enters; head 15° off stays, 25° leaves,
15° from the hip does not enter; one right-hand haptic pulse on entering; slip 4.8 mm / 0.25°; every
enter/exit `zoom_check=ok` (world P00 0.7813 = the VR FOV); the fired ray meets the sight line 25.2 m
down it, 1.5 mm apart.

**The sight is right, by eye**: with the eye on the learned sight line, the hand cannon is seen from
behind with the rear sight's glowing brackets framing the front post — the flat game's sight picture
(`build/shots/adstest/enter_zoom.png`).

**Regressions**: `Test-TwoHand.ps1` 26/26 (new steps: grip away waits and takes hold on arrival, left
trigger ignored while holding, grenade when free). `Test-Arms.ps1` wrist_err 0 in all 10 poses.
`Test-Aim.ps1`: 8 barrel steps 0.02–0.26° (was 0.01–0.47°), contrast and self as before; two flagged
steps are the instrument's (head_dn: the detector picked the left hand's fingers while the mark sits
visibly on the line; near: 1.13° on the centroid of a large near mark that is on the line).
**`Test-WeaponPivot.ps1`: 9/16 at first** — explained and fixed in §7 (17/17 now).

## 5. The centre dot

The red dot at the exact centre of the frame in ADS is the hand cannon's iron-sight dot: in the flat
game it sits on the front post (`build/shots/dot/novr_pair.png`). It is in the back buffer (`snap`),
so the headset sees it. No draw paints it — hiding every vertex shader, every pixel shader, every
ADS-only compute shader, each in groups and with ADS re-entered under the rule, left it — because it
is painted by Destiny's FINAL COMPOSITE (vs 7BDCE58C / ps BDF29159, the full-screen quad onto the
back buffer). Diffing that draw's pixel constants between hip and ADS (`cb_diff.py`, captures now
record `pcb`/`pt`): constant buffer 0 row 3 goes (0,0,0,0) -> (1,1,1,1). Zeroed in place at Unmap
(`patch BDF29159 3 0 0 0 0`), the dot is gone from screen and back buffer, and outside it the frame
differs from before by exactly a null pair's noise. `vr_gpu.cpp` now holds that row at zero while VR
is on (`reticle hide|show`). Verified in the built DLL: `centre_dot.py` reads 0 px held in ADS, 26 with
`reticle show`, 0 again after `reticle hide`.

Two traps learned on the way: a skipped draw leaves its render target as the last frame left it (hiding
everything freezes the frame rather than blanking it, so bisection over draws proves nothing); and the
game returns an idle player to orbit after a few minutes on the mock.

## 6. Commands and files

`SVR_Ads.txt` (game directory), one command per line:

| command | effect |
| --- | --- |
| `mode auto\|button\|off` | physical ADS (default), left trigger = ADS (old mapping), none |
| `enter <cm> <deg>` / `exit <cm> <deg>` | the hysteresis band (4 12 / 6 20) |
| `dwell <ms_in> <ms_out>` | 60 150 |
| `reach <m>` | 0.08 |
| `zero <m>` | 25 |
| `comp on\|off` | cancel the ADS animation |
| `learn on\|off`, `forget [all]` | the profiles |
| `haptic on\|off` | the entry tick |
| `force on\|off` | hold ADS regardless (tests; teaching a weapon) |
| `proxy auto\|hand\|gun` | what stands for the gun: the weapon palette's root whenever seen (`auto`, the default), the drawn hand, or the root always |
| `eye center\|left\|right` | the eye the mono image is rendered from (also F6, cycling) |
| `trace <s>` / `trace stop` | per-frame ADS progress, local hand pose, visible slip and the body servo's error, to `SVR_AdsTrace.csv`; `stop` ends it early and keeps the rows |
| `dump` | profiles and detector state |

`SVR_Aim.txt` gains `sight on|off`; `ray on` now draws the sight line in cyan beside the shot line in
magenta. `SVR_Gpu.txt` gains `hide pchk <8 hex>` (pixel shader checksum, stable across launches),
`hide range <from> <to>` (immediate-context draw ordinals, for bisection), `hide auto on|off`.

`SVR_Arms.txt` gains `canon aim <deg>`: the sight's pitch relative to the controller's pointing ray (§7.2),
0 by default.

Tools: `scripts/testing/Test-Ads.ps1` (the detector matrix, the slip, the zoom check, the convergence;
`-TwoHand` runs it holding the foregrip), `Test-ShotStability.ps1` (§7.1), `Test-ServoAds.ps1` (§7.4),
`ads_trace.py` (reads a trace), `in_hand.py` (the gun vs the controller over a trace, §7.3),
`Find-CentreDot.ps1` + `centre_dot.py` (the bisection).

## 7. Finishing pass (2026-10-15, evening): two defects found and fixed, the rest verified

Everything here was measured on the mock in Io with the build that contains it.

### 7.1 The seats chased the game's animation: the gun turned in the hand after every shot

With the controller held perfectly still, ten hip shots from the hand cannon did not leave along one
line: the first went level, the rest 3.5–4° low, one 8° off; and through a reload the detector saw the
sight swing up to **72° off the gaze**. Cause: the arm module's self-seating (`vr_arms.cpp`) learns the
weapon's static hold from the drawn hand, and its only gate was "controller steady", which is exactly
when a player fires or reloads. It chased the recoil (accepting rotations up to 60° and offsets up to
25 cm), turned the gun frame, and the shot, the sight and the detector all read the gun frame. The same
chase of the hand cannon's idle fidgets is what made `Test-WeaponPivot` read 27–69 px under pure rolls.

Fix: `ads::animation_at_rest()`. The drawn hand's pose in the viewmodel's local frame is pure
animation (the controller and every correction live in the frame handed to the engine, not in the
pose), so "unchanged by more than 1.5 mm / 0.4° per frame for 150 ms" means the game is holding the
weapon still. The positional seat, the rotational seat, the canon and the left hand's learned grip now
learn only then.

`Test-ShotStability.ps1` (10 shots and a reload, controller still), after the fix:

| check | before | after |
| --- | --- | --- |
| fired directions, worst from their mean | 3.5–8° | **0.02–0.10°** |
| sight vs gaze through the burst | 1.2–72° | **0.00–0.08°**, no ADS entered |
| gun vs controller at rest, after vs before | (4° off) | **0.2–1.3 mm, 0.02–0.33°** (the weapon's idle sway) |
| rotational seat, after vs before | — | 0.18° |

### 7.2 The canonical HAND did not point every weapon where the controller points

The rifle's aligned sight sat **12° off the gaze** with the controller level (hand cannon 1.2°): the
rotational seat turned each weapon until the right HAND matched a canonical pistol grip learned on the
hand cannon, and the rifle's animation holds its gun at a different angle in that hand, so the rifle
pointed 9° sideways and 7° up of the controller (seat 13.3°). Starting a session with the rifle would
have put the hand cannon off instead.

VR shooters do the opposite: the weapon is laid along the controller's pointing ray and the hand fits
the weapon (Pavlov, H3VR, and both let players adjust the "grip angle"). Once a weapon's sight is learned
the seat's target is now the canonical-hand target swung by the smallest rotation that lays the SIGHT
along the controller's aim (`vr_arms.cpp`; `ads::sight_direction`); the canonical hand only decides the
roll about it. The fixed point is a constant per weapon (simulated for 200 random holds and grip angles:
exact). While the weapon in hand is not identified yet but a learned profile has its palette, the seat
holds rather than drift towards the canonical hand and back. `canon aim <deg>` pitches the sight
relative to the pointing ray, for the headset. Measured after: aligned sight at **0.00° from the gaze for
the hand cannon and 0° for the rifle**, one- and two-handed.

Same pass, the detector's fallback: it borrowed another weapon's learned sight whenever the one in hand
had no learned sight, by palette bone count. That is right only while the weapon in hand is not
identified; a NEW weapon (identified, never held in ADS) now uses the honest guess.

### 7.3 `Test-WeaponPivot`: explained, 17/17

After 7.1 it read 13/16. The three left were the instrument: the mock turns a controller about its
AIM origin and its grip pose sits 7 cm behind it, so a 15° yaw really moves the grip 1.8 cm and the gun
anchored there rightly follows (the logged palm positions predicted 30 / 32 / 30 px of travel; the test
measured 22 / 27 / 34); and leaning 15 cm towards a gun 0.43 m away draws it 1.5× bigger, which template
matching cannot follow. The test now turns the controller about its grip, as a wrist does, and checks
head parallax with a vertical move. It also gained a check with no camera in it: the ADS trace records
the drawn gun relative to the controller every frame, which must be constant if the gun stays in the
hand. Result: rotations 0–3 px, turns 0–1 px, and over **3816 settled frames** of every rotation and
two 90° anchor turns the gun stayed within **1.2 mm and 0.32°** of one pose in the hand.

### 7.4 The rest of the handoff

- **Rifle + ADS** (`Test-Ads.ps1` with slot 2): 13/14; **rifle held in both hands + ADS** (`-TwoHand`):
  15/16 — the hold stays on throughout (barrel through the left palm within 0.5 mm), detection, zoom
  and the 25 m zero all hold. The one failure in each is the transition slip, next item.
- **The rifle's ADS transition**: worst single frame 11–20 mm (hand cannon 4–5 mm), rotation under
  0.45°; under 1 mm once in ADS. Not the curve: the rifle's animation is fast (about 28 mm per frame at
  its peak on the mock's ~64 fps), and the drawn gun's progress along its own path, against an evenly
  advancing FOV, reads 0.22 → 0.56 → 0.39 → 0.89 — which animation sample the engine pairs with which
  FOV wobbles by a frame. A learned curve cannot remove ±1 frame of jitter. Known limit; on a 90 Hz
  headset each frame carries 30% less of it. It was there before this pass (12.7 mm), unmeasured.
- **Spread**: the rifle's spaced single shots land on their fired ray within the instrument's floor
  in both states (hip 0.02–0.17°, ADS 0.07–0.21°, bullet holes): there is no first-shot spread for
  ADS to remove. The engine is really in ADS (its zoom), and the spread is added downstream of the
  rewrite, so its ADS spread applies — but sustained-fire bloom, where the difference would show, was
  not measured. Aim assist needs enemies; untested.
- **The body servo in ADS** (`Test-ServoAds.ps1`): a 30° head turn settles under 2° in 203 ms in ADS
  against 109 ms at the hip (the game's lower ADS sensitivity halves the loop's gain, as expected); the
  stuck guard stays quiet. The view never depends on it, so this costs only the legs.
- Instrument fixes worth knowing: log lines are fresh by TIMESTAMP, not by counting matches in the
  log's tail (the count stops growing once old matches scroll out, and a stale line read as a seat that
  had "moved 1.08°"); and quaternions printed to four decimals must be normalised before comparing
  (two prints of one rotation read 1.1° apart).
