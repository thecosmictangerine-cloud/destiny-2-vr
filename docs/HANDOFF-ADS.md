# Handoff — physical ADS: done on the mock, next is the headset (2026-10-15, end of day)

Read first: `docs/ADS.md` (design, measurements, §7 the finishing pass), then `CLAUDE.md` (build/test
loop). **Nothing is committed** (rule: commit only when asked).

## State in one paragraph

Physical ADS works end to end on the mock, with the three loadout weapons learned. The ADS animation
is cancelled (hand cannon worst frame 4–5 mm, rifle 11–20 mm, all under 1 mm once in ADS). The shot is
zeroed onto the learned sight at 25 m. ADS happens by bringing the sight to the eye, with hysteresis
and a haptic tick. The zoom never reaches the image, and the ADS centre dot is patched out (verified in
the built DLL). This pass found and fixed two defects that would have shown at once in the headset:

- **the gun turned in the hand after every shot and reload**: the seats chased the game's animation.
  It's now gated on `ads::animation_at_rest()`.
- **the rifle pointed 13° off the controller**: the seat aimed a canonical HAND, not the weapon. Every
  learned weapon's SIGHT now lies along the controller's pointing ray.

The `Test-WeaponPivot` mystery is explained and it reads 17/17.

## Test status (mock, Io, final build)

| test | result |
| --- | --- |
| `Test-Ads.ps1` hand cannon | 14/14 (twice) |
| `Test-Ads.ps1` rifle / rifle `-TwoHand` | 13/14 / 15/16; the miss is the rifle's transition slip (known limit, ADS.md §7.4) |
| `Test-ShotStability.ps1` | 4/4 on the final run (rounds within 0.05°, seat 0.08°, drift 0.25°); one earlier run read 0.33° drift against a 0.3° bar, since raised to 0.5° (the idle-sway floor; the defect was 4°) |
| `Test-WeaponPivot.ps1 -Pivot -0.33 -0.0915 0.109` | 17/17, gun in the hand within 1.2 mm / 0.32° over 3816 frames |
| `Test-TwoHand.ps1` | 26/26 |
| `Test-Arms.ps1` | wrist_err 0 in all 10 poses |
| `Test-ServoAds.ps1` | 3/3 (settles in 203 ms in ADS, 109 at the hip) |
| centre dot | 0 px hidden, 26 shown |
| spread, rifle | hip and ADS both at the instrument floor (~0.2°) |

## Next: the headset session (the user is the tester)

Profiles learned on the mock carry over (`C:\Games\Sunrise\SVR_AdsProfiles.txt`, keep it). Launch with
`Run-Patrol.ps1 -Runtime meta`, click the game window, F9. What to ask:

1. **Does each weapon point where the hand points?** Hip-fire with the hand cannon, then the rifle.
   If the guns feel pitched wrong in the hand, that's the grip angle: `canon aim <deg>` in
   `SVR_Arms.txt` (positive raises the sight relative to the controller's ray). Pavlov and H3VR
   players tune this too; settle on a default.
2. **Does sighting feel natural?** Do the thresholds (4 cm / 12° in, 6 cm / 20° out) trigger when the
   user means to aim and not otherwise? Tune live with `enter` / `exit` in `SVR_Ads.txt`.
3. **Does the gun stay put in the hand when ADS starts and ends?** The rifle's transition has a
   one-frame 1–2 cm twitch on the mock: is it visible at 72–120 Hz?
4. **Which eye?** F6 cycles centre -> right -> left. The winner becomes the default (`g_renderEye`).
5. **The haptic tick on entering**: welcome or noise (`haptic off`)?
6. **Grenade on the left trigger**, ignored while holding the foregrip: right?
7. **Firing and reloading with the controller still**: does the gun stay where it is in the hand?
   (On the mock: yes, 0.02–0.10° between rounds.)

## Later, as agreed

- Magnified scopes (the frame behind the lens magnified into it).
- The grenade launcher's sight points 3.5° up from its gun axis; with the shot zeroed on the sight,
  check where grenades land against the sight at 15–30 m.
- Sustained-fire bloom in ADS vs hip (single spaced shots show no spread in either state).
- The rifle's transition jitter, if the headset shows it: the fix would be pairing the compensation
  with the animation sample the engine actually draws, not the FOV of the frame being placed.

## Known limits, honestly

- The transition residual is single frames of ±1 frame of animation-vs-FOV jitter (hand cannon ~5 mm,
  rifle up to 20 mm on the mock at ~64 fps); held ADS is under 1 mm.
- A weapon never seen before uses a guessed sight for its first ADS, and its first ADS is not
  compensated. It points by the canonical hand until its sight is learned, then turns gently, once,
  to lay its sight along the controller. The three loadout weapons are already learned.
- Profiles are told apart by palette bone count, hip pose (1.2 cm / 3°) and ADS zoom; two weapons
  identical in all three would share a profile.
- The hand re-grips the gun by about 4° in ADS (the game's animation); the gun doesn't move.
- The drawn gun follows the weapon's idle sway by up to ~0.3° relative to the controller.
- Aim assist needs enemies; untested.

## Tools added in the finishing pass

`Test-ShotStability.ps1`, `Test-ServoAds.ps1`, `in_hand.py`; `Test-Ads.ps1 -TwoHand`;
`Test-WeaponPivot.ps1` now rotates about the grip and has the `in_hand` trace check; `trace stop` and a
`body` column in the ADS trace; `canon aim <deg>` in `SVR_Arms.txt`.
