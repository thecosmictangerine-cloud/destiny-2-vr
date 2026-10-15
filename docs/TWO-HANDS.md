# The two-handed hold (2026-10-15)

The left hand takes hold of the weapon. Agreed with the user, following the convention of the VR
shooters that do this well (Pavlov, Onward, H3VR, Contractors):

- **Grip plus proximity.** Squeezing the left grip on or near the handguard takes hold; opening the
  hand lets go. Proximity alone would grab the gun every time the hands crossed.
- **The left grip is contextual.** It was the grenade. Squeezed away from the gun it still is.
- **Long guns aim with both hands.** The barrel runs from the right hand through the left; the roll
  stays the right wrist's. The hand may hold anywhere along the handguard and slides along it.
- **Pistols and hand cannons: a support hand, drawn, never steering.** Five centimetres between the
  palms is no lever to aim by. Decided automatically from the weapon, with no per-weapon table.
- **A click in the left controller** on taking hold (and a lighter one when the gun is taken away).

ADS and shooting where the barrel points are the next step, not this one.

Code: `hooks/vr/vr_arms.cpp` (learning, state machine, drawing), `hooks/vr/vr_weapon.cpp`
(`apply_two_hand`), `hooks/vr/vr_two_hand.h` (the geometry both share), `hooks/vr/xr_runtime.cpp`
(haptics), `hooks/vr/vr_gamepad.cpp` (the contextual grenade).

## Where the foregrip is: learned from the game

Bungie animated a left hand on the foregrip of every weapon. That is the hold point, so nothing is
measured per weapon. But the hand is **learned**, never followed live: reload, inspect, sprint and
grenade animations take the animated left hand off the gun, and a held hand that flew off with it
would be absurd.

What is learned is the animated left hand's frame **in the animated right hand's frame**, read out
of the arm palette (`Foregrip::rel` in `vr_arms.cpp`). That quantity is rigid on the gun while the
left hand is on it, so it follows the gun through recoil, bob and the seat loops, and it involves no
thread timing at all -- both hands come from the same palette. It is taken only from a hand that is
**still** (4 mm and 2° frame to frame):

| measurement | treatment |
| --- | --- |
| within 2 cm and 10° of what is learned | blended in slowly; fingers recaptured; confirmed after 0.25 s still with the weapon seated |
| elsewhere, still for 1 s, where a foregrip can be | a different weapon: adopted outright (0.3 s for the first) |
| anything else | ignored: a reload moves the hand continuously |

The fingers around the foregrip are captured with it, so a reload's fingers never leak into the held
hand.

The hold steers by the foregrip in the **right controller's raw frame** (`fc`): the left palm on the
drawn gun, from the controller's palm, measured only while the controller is still (a moving one
puts the render latency into it) and the gun held one-handed. Its length decides the mode: under
0.18 m it is a pistol's support hand.

## The aim: an exact solution, not a blend

The weapon module's positional seat puts the drawn right palm on the controller's palm at any
orientation, so everything drawn on the gun pivots about that point. Turning the basis handed to the
engine by a world rotation `D` therefore moves the foregrip from `p + C fc` to `p + D C fc`, and
nothing else needs to be known.

A rotation about the right palm preserves distances from it, so of all the points on the handguard
line `fc + s x` only the one at the left palm's own distance can be brought onto the palm:

```
s = -fc.x + sqrt(fc.x^2 - |fc|^2 + |l - p|^2)        (the forward root, clamped to the handguard)
D = shortest rotation taking  C (fc + s x)  onto  l - p
```

After `D`, the handguard line passes **exactly** through the left palm, at any lever and any slide.
The shortest arc is also what keeps the roll the right wrist's: steering with the left hand turns the
gun about an axis perpendicular to the barrel, never about it.

`D` is applied in the weapon module, on the game thread, from the same OpenXR sample as the weapon's
position (`hand_pose_for`), after the rotational seat, and blended in by the hold's weight. The
transform detour's pivot term reads the rotated basis through the latched sample, so the gun turns
rigidly about the right palm.

The arm module draws the left hand **from the palette**: the learned foregrip carried by the drawn
right hand, slid along the drawn handguard to the point nearest the controller. Because that comes
from what was drawn, the hand is always on the gun, whichever frame the orientation was computed in.

## While the second hand holds the gun

The right hand on the gun is then no longer where the right controller alone would put it, so:

- the **rotational seat** and canon learning are frozen (they would turn the gun back);
- the left hand's **grip orientation learner** (`g_grip`) is frozen (it would learn the hold);
- the **positional seat** keeps running, measured in the drawn hand's basis instead of the raw
  controller's, because that is the basis the weapon module applies it in;
- the foregrip's `fc` is not re-measured.

All of these stay frozen 200 ms past the end of the release ramp, for palettes still in flight.

## The state machine (present thread, after the actions are synced)

| state | meaning |
| --- | --- |
| free | grip open |
| pending | squeezed near the gun (0.10–0.25 m): waits for the hand to arrive, never a grenade |
| held | holding; weight ramps to 1 in 0.12 s (eased, real time) |
| grenade | squeezed away from the gun or with nothing to hold: Q held while squeezed |
| suspended | the game took the gun away (swap, melee, super); comes back by itself while still squeezed |

A squeeze is classified once, where it starts. A grenade squeeze stays a grenade even if the hand
then passes the gun; a hold squeeze never throws one.

Letting go, other than by opening the hand:

- foregrip: the gun would have to turn more than 75° from the right hand's aim; the hands closer
  than 10 cm; the hand slid 12 cm past either end of the handguard (−0.12 … +0.10 m around the
  learned point);
- support hand: its controller 18 cm from the cup (taken at 10: hysteresis);
- a weapon swap (Y), a melee (right grip) or a super (right stick click): suspended, and the
  foregrip must be confirmed again before anything takes hold, because after a swap it belongs to
  another weapon.

Tracking loss: a lost left controller is held where it last was **relative to the right one**, in
both modules, so the gun stays rigid in both hands until the controller is seen again. Inside-out
tracking loses a controller exactly when two-handing puts it in front of the other.

## Tunables (`SVR_Arms.txt`)

`twohand on|off`, `twohand aim on|off`, `twohand engage <take> <grenade>`,
`twohand slide <back> <fwd>`, `twohand pistol <m>`, `twohand blend <s>`, `twohand relearn`,
`twohand dump`. All live, no rebuild.

## Verification

`scripts/testing/Test-TwoHand.ps1`, with the overlay on: the handguard on the drawn gun (cyan free,
amber taking hold, green held), the palm on it, and a magenta line from the left controller to that
palm. The decisive number is `line_err` in `ev=vr.arms hold`: the distance from the left controller
to the handguard line **of the drawn gun**, read back from the GPU palette.

## Results on the mock (Io, 2026-10-15)

`Test-TwoHand.ps1`: **24 of 24 steps pass**. `Test-Arms.ps1` unchanged (wrist error 0.0000 everywhere).

What the game's animation gave, with nothing measured by hand:

| weapon | foregrip `fc` (forward, left, up) | lever | mode |
| --- | --- | --- | --- |
| auto rifle | (0.381, −0.023, 0.045) m | 0.385 m | aim |
| hand cannon | (0.011, 0.070, −0.038) m | 0.080 m | support (the left hand cups the right) |

| check | result |
| --- | --- |
| take hold on the foregrip | held, aim, w 1.00, `line_err` 0.5–2.2 mm |
| left hand 8 cm left / up | swing 11.80° / 11.57° against atan(0.08/0.385) = 11.74°; `line_err` 0.5 / 0.7 mm |
| slide 6 cm forward along the handguard | slide 0.060, swing 0.13° (sliding does not turn the gun) |
| right wrist rolled 35° | `line_err` 0.1 mm |
| artificial turn 90° while holding | `line_err` 1.5 mm |
| head leaned 10 cm, yawed 20°, pitched −10° | `line_err` 0.6 mm |
| slide 30 cm off the front | released, `slide`; pending; back on the handguard takes hold again |
| firing while held | stays held; the hand rides the recoil (`line_err` 7–12 mm while the gun kicks) |
| squeeze 18 cm away | pending, no grenade; arrives, takes hold |
| squeeze at rest (48 cm) | grenade, thrown; passing the gun keeps it a grenade |
| swap (Y) while holding | suspended `swap`; the hand cannon's foregrip learned and confirmed; held again, support |
| support hand strays 14 cm / 24 cm | still held / released `range` |
| left trigger (ADS) while holding | held throughout; 12 mm during the ADS transition, back to 1 mm (the positional seat, running in the drawn basis, absorbs it) |
| haptics | one pulse per grab, one per forced release (7 in a run), `ev=vr.xr haptic` |

Two defects found on the way, both in the first version and both about weapon swaps:

- **A swap reconfirmed the old foregrip.** The swap animation LOWERS the old gun with both hands still
  rigidly on it, so the old foregrip read as perfectly still and confirmed itself within 250 ms; the
  suspended hold then re-engaged on it and the new gun was steered by the wrong foregrip (13 cm off).
  Fixed with an 800 ms window after a swap, melee or super in which nothing confirms (and the
  stillness streak restarts), plus a foregrip generation: a hold whose foregrip is replaced under
  it suspends and takes hold again on the new one.
- **`fc` was measured mid-swap**, against a right hand the animation was carrying away from the
  controller, and read 0.348 m for a hand cannon. It is now measured only from a confirmed, seated gun.

And two harness bugs, fixed: the mock opened its log exclusively (no test could read it while the
game ran), and its pose reader marked a file write as consumed before reading it, so a read that
collided with the harness's write lost that pose for good.

## What only the headset can say

- Whether 0.10 m to take hold and 0.25 m for the grenade feel right; whether a support hand on a
  hand cannon is welcome or in the way. All live-tunable (`twohand engage`, `twohand pistol`).
- The 0.12 s ramp: fast enough to feel like a snap, slow enough not to jolt.
- Whether the strength of the click is right on a Touch controller.
- Tracking loss with the controllers close together, which the mock cannot produce.

## Known, and left for the ADS step

- **ADS zooms the world.** Pulling the left trigger while two-handing (the index finger rests on it)
  puts the game into ADS, and the game's FOV zoom reaches the headset. The hold survives it; the
  zoom is the first thing the ADS work has to remove.
- **Bullets still leave from the camera**, so steering the barrel with the left hand does not yet
  steer the shots.
