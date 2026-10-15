# Arms — each arm on its own controller, the weapon in the right hand (2026-10-15)

The goal, agreed with the user: full arms, each one independent and following its controller, the
gun held in the right hand, posed as well as the best VR games pose them. A two-handed foregrip and
what the left hand does during reloads are explicitly next steps.

The technique is the one dishonored-vr used, and that the user called "perfect" on the headset:
**rewrite the arms' bone palette on its way to the GPU**. The engine's skeleton and animation are
never touched; the arms are re-posed in the draw call that skins them. Nothing about it depends on
Tiger: `d3d11.dll` is not VMProtect's, and detours there raise no exception.

Code: `hooks/vr/vr_gpu.cpp` (D3D11 detours, capture, the palette and projection rewrites),
`hooks/vr/vr_arms.cpp` (rig, joint learning, the solve, the overlay), `hooks/vr/vr_math.h`.

## What the GPU shows (measured in Io, hand cannon, 2026-10-15)

All of it from `vr_gpu.cpp`'s whole-frame capture (`capture` in `SVR_Gpu.txt`), analysed with
`scripts/testing/gpu_capture.py`, `find_arms.py` and `arm_rig.py`, and confirmed by hiding draws
and comparing screenshots.

- **Every draw is on the immediate context** (0 deferred), about 3,600 per frame.
- **Shader reflection is stripped.** No resource names, no cbuffer layouts, no instruction counts:
  only the input signatures survive (`POSITION`, `BLENDWEIGHT`, `BLENDINDICES`). Everything below was
  found from the data, not from names.
- **The bone palette is constant buffer 11**, a 64 KB DYNAMIC buffer re-Mapped for every skinned
  object (one resource, new contents per object):
  - rows 4–5: the mesh's vertex dequantisation — row 4 is `(s, s, s, 0)`, row 5 the origin;
  - from row 8: one **3×4 row-major affine per bone**, 48 bytes each, `M = W · B⁻¹`.
- **The arms are three meshes on one 72-bone skeleton**: the gloves (72-bone palette, `s = 0.4406`)
  and two gauntlet layers (52-bone palettes, the first 52 bones of the same skeleton, `s = 0.5237`),
  which carry the forearm, the upper arm and the shoulder. Hiding all three removes the arms and
  leaves the gun floating. **The gun has its own 8-bone palette**, so arms and gun are separable.
- **The viewmodel is drawn camera-relative in world axes**: its per-view constants (cb12, 224 bytes,
  also re-Mapped per view) hold the same camera rotation as the world's with the camera position
  zeroed. A hand target in that space is simply `palm − head.offset`.
- **The viewmodel has its own projection** (cb12 rows 10–13; rows 0–3 hold the transposed
  view-projection): 77° horizontal against the world's 81.6° on a monitor — and with F9 on, the
  world goes to 104° while the viewmodel stays at 77°. **The gun and arms were being drawn about
  1.6× too large and pushed outwards in VR.** `vr_gpu.cpp` now widens the viewmodel's projection to
  the world's (`fov on|off`), keeping its depth compression.
- Vertex stream 0, 16 bytes: `R16G16B16A16_SNORM` position, `R8G8B8A8_UNORM` weights,
  `R8G8B8A8_UINT` indices. One gauntlet layer packs 12 bytes, without weights.

## The skeleton

Read off the skin-weight graph (which bones share vertices, and how much):

| | chest | clavicle | shoulder twist | upper arm | forearm | wrist twist | hand | fingers |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| left | 11 | 12 | 27 | 15 | 19 | 34 | 21 | 40, 42–45, 52–56, 62–66 |
| right | 11 | 14 | 28 | 17 | 20 | 39 | 22 | 46, 48–51, 57–61, 67–71 |

Left and right come from the bind pose (the game's +Y is left). Every armour piece indexes the same
palette, so the labels hold for any gauntlets; they are validated against the meshes before use.

**Joints**, two independent ways:

1. *Weight overlap*: the skin bends from one bone to the next where their weights overlap, so the
   weight-product centroid of a pair sits on their joint. Good at the elbow and wrist, biased at the
   shoulder by the deltoid.
2. *Animation*: a joint shared by parent and child satisfies `(R_p − R_c) J = t_c − t_p` every frame.
   Seven captures (idle ×2, ADS, sprint, two reload frames, idle) give, in bind space:

| joint | position (m) | residual |
| --- | --- | --- |
| shoulder | (−0.027, ±0.186, 1.485) | 0.1 mm |
| elbow | (0.077, ±0.324, 1.274) | 0 (hinge: undetermined along its axis, as expected) |
| wrist | (0.292, ±0.392, 1.156) | 0 |

Upper arm 0.27 m, forearm 0.25 m. The runtime does both: the overlap estimate as a prior the moment
the meshes are read, then the least squares, accumulated from every frame's palette and bounded
around the prior.

## The pose

Per arm, every draw, in camera-relative space:

- **Body.** Chest and clavicles in their bind pose, carried by a body frame hung from the **neck**
  (8 cm behind and 10 cm below the eye, in the head's frame — the point the head pivots about, so
  looking down does not drag the shoulders) and turned to the body's heading, which follows the head
  with a little lag and never more than 35° behind.
- **Hands.** Right: exactly where the game animated it — on the gun, which the weapon module already
  put on the controller (and which now seats itself there, below). Left: on the left controller,
  turned the way the right hand holds the gun, mirrored — see "The left hand".
- **Elbows.** Two-bone IK from the shoulder to the wrist, bent towards an anatomical pole (down,
  out, slightly back). Past full reach the arm stretches up to 12 %, then the shoulder slides — the
  hand on the controller is the one thing that must never be wrong.
- **Segments.** Each upper arm and forearm is placed from its *bind* geometry onto the solved
  joints, rolled by the elbow's plane. The forearm takes 45 % of the hand's roll and the wrist twist
  bone 90 %, the way the radius turns about the ulna.
- **Fingers.** The game's own pose, carried rigidly with the hand.

## The rewrite happens in place, at Unmap

The first version bound a rewritten copy of cb11/cb12 for each arm draw and restored the game's
buffer afterwards. It worked for the palette and never once matched for the view, and it had to
know which draws to touch. Both buffers are now **rewritten in the memory the game just Mapped**,
inside the Unmap detour, before the GPU ever sees them: every pass that binds them (G-buffer, depth
prepass, shadows) gets the posed arms and the corrected projection, and nothing has to be tracked
or put back. Draws are still watched, only to learn which vertex buffer an arm palette skins, so it
can be read into the rig once.

Two traps on the way: the mapped pointer is shared through a global table, not a per-thread one
(the game may Map and Unmap on different threads), and an arm palette is recognised by being
camera-relative with 40+ bones, not by its root sitting under the eye — once the weapon module
places the viewmodel on the controller it carries the whole skeleton with it.

## The weapon seats itself

The arm rewrite gives something the weapon work never had: **where the drawn right hand actually
is**, every frame, read out of the palette. Two slow loops close on it (`vr_weapon.cpp`
`seat_error` / `set_seat_rotation`):

- **Position.** The palm centre (the hand bone's skin centroid) is driven onto the controller's grip
  pose, through an offset in the controller's basis added to `pivot`. Converges to 0.5–2 mm. For the
  hand cannon it settles 4 cm from the hand-measured pivot; for the auto rifle, about 14 cm — the
  constant was only ever right for one weapon.
- **Orientation.** The right hand's rotation in the controller frame is driven to a canonical grip:
  the hand cannon's, at rest, measured once (`q = 0.0836, −0.3577, −0.0256, 0.9297`, in the
  controller's forward-left-up frame). The auto rifle used to point 90° to the left of the
  controller; it now points along it, as does every weapon, which is also the prerequisite for
  shooting where the controller aims.

Gain 0.04 per frame (about a third of a second): held offsets and slow animation (sprint) are
cancelled, recoil still reads. Both loops run only while the controller is still, the rotation only
while the palm is already within 15 cm (the weapon really held), big disagreements (a weapon swap)
are ignored, and if the palm stays more than 25 cm away for two seconds both corrections reset.
`seat on|off|reset` in `SVR_Weapon.txt`; `canon learn|on|off|reset` in `SVR_Arms.txt`.

## The left hand

Orientation: the right hand's grip, mirrored across the body's midplane — once the rotational seat
has converged, the canonical pistol grip, so it no longer changes with the weapon. Position: its
palm centre on the left controller's grip pose (which OpenXR defines as exactly that point).
Fingers: **open to closed with the left grip button** — open is the bind pose (the relaxed hand
the model was built in), closed is the right hand's fingers around the gun's handle, mirrored
(left and right finger bones paired by mirror symmetry of their bind centroids). Blended about each
bone's own centroid, never the model origin.

The left grip is contextual since the two-handed hold (`docs/TWO-HANDS.md`): near the weapon's
handguard it takes hold, away from it it is still the grenade. The curl itself is purely visual.

## Results on the mock (Io, 2026-10-15)

- Joints learned live match the offline solve to the millimetre (upper arm 0.273 m, forearm 0.255).
- Wrist error (the wrist as the rewritten forearm carries it, against the target): 0.0000 in every
  pose of `Test-Arms.ps1` — rest, left hand high, full reach (stretch, then shoulder slide), roll 80°,
  yaw 45°, pitch −50°, right hand across the body, right roll 60°, hands close, two-hand pose.
- No ghosting on a hand moving at 1.5 Hz; looking 55° down shows both arms whole to the shoulders.
- Weapon swap: the new weapon seats (0.5 mm, 0.00°) within about a second. F9 off gives the arms back
  to the game, F9 on takes them again.

## Verification

- `SVR_Arms.txt`: `overlay on` draws, with the frame's own viewmodel camera, each controller's frame
  (RGB = forward, left, up), the hand target frames, and the solved arm (yellow; orange when the
  shoulder had to slide). A yellow line outside the sleeve is a bug.
- `ev=vr.arms state` in the log: stretch, forearm twist, and the wrist error (the wrist as the
  rewritten forearm carries it against the target).
- `scripts/testing/Test-Arms.ps1`: the pose matrix on the mock.
- `ev=vr.gpu rewrite`: palettes seen, palettes and views swapped, the world projection used.

What only the headset can judge: whether the arms *feel* like your arms — shoulder width and height,
how the elbow drops, how the grip sits in the hand. Everything that shapes that is live-tunable from
`SVR_Arms.txt` (`neck`, `shoulder`, `pole`, `stretch`, `twist`) without a rebuild.

## Not done yet

- ~~Two-handed foregrip~~ — done, see `docs/TWO-HANDS.md`.
- **Reloads**: the left hand stays on its controller; the game's reload animation no longer reaches
  it.
- **Index finger on the trigger**, thumb on the face buttons: the curl is all-or-nothing per hand.
- **Shooting where the controller points**: the seat makes every weapon point along the controller,
  but bullets still leave from the camera.
- The **clavicles** do not shrug when a hand goes high; the shoulder slides instead past full reach.
- The player's own **third-person body** is visible directly below when looking straight down (a
  piece of armour at the bottom of the frame). Present with the arms off too; it is the game's.
