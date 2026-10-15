# Aim — the shot leaves the barrel (2026-10-15)

Goal agreed with the user: the shot goes where the gun points, exactly and without lag, and that is
verified by measurement rather than by eye. `HANDOFF-ADS.md` is where this started; ADS, the zoom
and the reticle come after it.

## 1. What the shot followed before (measured)

Io (Giant's Scar), mock runtime, hand cannon, one round per raw click. The impact is a burst of
sparks visible for a few hundred milliseconds; a capture 80–150 ms after the click finds it
(`scripts/testing/impact_find.py`), a decal a second later is too faint to difference.

| Head | Controller | Character (mouse) | Impact |
| --- | --- | --- | --- |
| level | level | level | view centre |
| 30° down | level | level | out of view: the round left level |
| level | 30° down | level | not where the controller points |
| level | level | **40° down** | **on the ground, low in the frame** |
| level | 15° down, 20° right | level | view centre, even with **every** caller of the pose getter and pointer handed the controller |

**The shot followed the character's own aim** — yaw from the body servo (so roughly the gaze), pitch
left wherever it was (level, since vertical stick look is dropped). The camera-pose paths the weapon
module detours carry nothing to it.

## 2. Where the shot is made (found 2026-10-15)

Large parts of the weapon code are **virtualised by VMProtect**: the function entry is a five-byte
`jmp` into the VM. Two facts made it tractable anyway:

- the original x86 of a virtualised function is still in the image, after the entry `jmp`, and is
  readable (`scripts/diagnostics/live_image.py`: snapshot of the decrypted image, `.pdata`-aware
  `func`/`fdis`/`xref`/`calls`, and `disp_refs.py` for field-offset cross references);
- the entry `jmp` is itself a perfect hook point — exactly five bytes — so a virtualised function
  can be hooked by retargeting its displacement (`hooks/vr/vr_prehook.cpp`).

`hooks/vr/vr_probe.cpp` is a live tracer built on that: any function, hooked from a command file,
logs its distinct callers with full call chains (unwind tables only, bounded by the stack limits —
nothing that raises an exception), its argument registers and the floats behind them.

The path, from the ammo getters a shot reads (`+0xCBCD00`, `+0xCBCF30`) upwards and then down:

```
+0xB5F4EE ... +0xD0A320  weapon subsystem scheduler (flags at +0xE0)
  +0xCB7680              FIRE: consumes ammo (+0xCCA120 -> magazine setter), counts the shot
                          (+0x1710), and queues a fire event: +0xCC6010 links it at weapon+0x1720
+0xCCF720                plays queued fire events out when their time comes:
  +0xCB48C0 (VM)          resolves one event into fire points:
    +0xA1F4C0               named sockets of the weapon (FNV hashes), up to 64
    +0xCB53E0               their muzzle points in the world
    +0x181E930              LAUNCH(weapon, flags, FirePoint* points, Vec4* muzzles, int count, ...)
      +0x181C8B0 (VM)        one round per fire point:
        +0x12B0390             the round's aim: asks
          +0x12B0950             AIM_RAY(&player, ..., Vec3* origin, Vec3* dir, Vec3* eye, int*)
                                   from the camera block (+0x12D50F0) or a protected view-ray cache
                                   (+0x1285110 -> +0x1284CC0: obfuscated singleton, eye at +0x3E0,
                                   forward at +0x3F0). Also called once a frame from +0x12B0C28.
        +0x181F680             ROUND TRACE(&player, Vec4 ray[2]{start, dir}, ..., stack flags):
                                 arg7 -> dir := actor's stored aim (+0xB20 / +0xB14)
                                 arg6 -> start := projected onto the eye's line
                                 then spread, then
          +0x1296E50             the hit test (eye passed too, for a wall check eye -> start)
            +0xA33180 -> +0xA33490   the raycast itself (124 call sites)
```

The shape is the classic one: the round is aimed at what the CAMERA's ray hits and sent there from
the muzzle. Rewriting only the fire points moved the muzzle flash and tracer but not the impact.

`FirePoint` is 0x50 bytes: `+0x10` orientation quaternion (x, y, z, w), `+0x20` position
(x, y, z, 1). `muzzles` is one world point per fire point, 0x10 apart. All world space, game axes.
The orientation **is the character's aim** (a hand cannon at rest: `(0.00007, 0.00005, 0.7936,
-0.6084)`, i.e. yaw 4.449 rad = the body yaw the camera logged).

## 3. The levers (three, all in `hooks/vr/vr_aim.cpp`)

1. **Launch** (`on_launch`, pre-hook on `+0x181E930`): the fire points become the barrel —
   muzzle flash, tracer, the round's start and direction.
2. **Aim ray** (`on_aim_ray` + `after_aim_ray`, pre-hook on `+0x12B0950` with a post-call): when
   the round asks for its aim (return address `+0x12B06EA`), its origin, direction and eye outputs
   are rewritten after the call with the muzzle and the barrel. The once-a-frame call from
   `+0x12B0C28` is left alone. The post-call is `prehook::request_post`: the call's return address
   is swapped for a trampoline that runs the rewrite and jumps on.
3. **Round trace** (`on_round_trace`, pre-hook on `+0x181F680`): recognised by content — its ray
   must equal the muzzle and barrel this thread's launch rewrite just wrote — and its two flags
   that would put the camera's aim back (arg 6, arg 7) are cleared. Spread and the eye-to-start
   wall check stay the game's.

How the fire points are rewritten (lever 1):

- **orientation**: `D · q_i`, with `D = barrel · aim⁻¹` — the rotation from the character's aim to
  the gun's, so a multi-point pattern keeps its shape;
- **position and muzzle**: the VR gun's muzzle, `eye_engine + palm + R · muzzle`, plus each point's
  rotated offset from the first — the same composition the gun is drawn with.

`barrel` is the orientation the weapon module handed the engine this frame (controller, rotational
seat and two-handed swing included), read from `weapon::weapon_sample`. Spread and recoil are still
the game's, applied downstream relative to the orientation it is given.

Only the local player's own shots are touched: the first fire point must be within 2 m of this
player's eye and its orientation within ~25° of this player's aim. Anything else — another player,
an AI, a turret — launches exactly as the game made it.

`SVR_Aim.txt`: `shot aim|observe|off` (default `aim`), `muzzle <fwd> <right> <up>`, `ray on|off`,
`length <m>`, `dump`. Every shot logs `ev=vr.aim shot` with what the game aimed and what was fired.

## 4. Verification (mock, Io, 2026-10-15)

**Plumbing, exact.** The round's hit test (`+0x1296E50`, traced live with the probe) receives:

| | start | direction |
| --- | --- | --- |
| before | `625.3, 299.4, 41.8` (eye + 0.67·aim) | `-0.2538, -0.9673, 0.0001` (the character's aim) |
| after  | `625.061, 299.469, 41.736` (the VR muzzle) | `-0.4777, -0.8599, -0.1797` (the barrel, to 4 decimals) |

Every shot logs `ev=vr.aim shot` and `ev=vr.aim ray ... rays_rewritten=N traces_kept=N`; all three
levers fired on every shot of the session.

**End to end, by eye.** `build/shots/aim/contrast.png`: controller 12° left and down → the impact
lands left on the ground; the same right-hand pose with `shot off` → back to the view centre (the
old behaviour); with `shot aim` → right, beside the gun. `s3_pair.png`: the dust burst on the ground
along the barrel line, where before it was always at the centre of the view.

**Measured to the degree (2026-10-15, second session).** `scripts/testing/Test-Aim.ps1`, hand cannon,
against the ray each round was really fired along (`new_pos`/`new_fwd` of its own `ev=vr.aim shot`,
projected with the frame's camera):

| kind | steps | result |
| --- | --- | --- |
| barrel (`shot aim`) | null, hand left/right/up, far, head left/down, roll, near (2 m), turn | every mark found **0.01–0.47°** off the fired ray (8–10 of 11 per run; the rest inconclusive, see below) |
| contrast (`shot observe`) | the game's own round, barrel pointed elsewhere | 4–46° off the barrel line: the test can tell the two apart |
| self (`shot observe`) | the game's own round against its OWN ray | 0.45–1.76°: the hand cannon's hip-fire spread, i.e. the instrument's floor |

How it measures, and the four things that made the first numbers wrong:

- **The mark, not the flash.** This hand cannon's rounds explode, and the flash is centred ~20 cm
  ABOVE the point hit: it read as a steady 1–1.5° (4° at 2 m) "miss" upwards, always on the same
  side of the line, whatever the pose. The bullet hole a round leaves, found in a capture 2.5 s
  later (`impact_find.py --decal`), is exactly at the hit point: one shot measured 0.28° by its
  mark and 1.51° by its flash.
- **The gun out of frame.** The controller is held low on the right, out of view, aiming back into
  the scene (where the gun is changes nothing about the claim); otherwise recoil is the biggest
  change between the captures and an impact on nearby ground lands beside the gun.
  `Show-Aim.ps1` is the in-frame picture for a person.
- **Motion blur off.** The game's motion blur smeared every frame after a shot (the recoil kick of
  its own camera, which the module overrides). `motion_blur_mode=0` in `cvars.xml` -- it should be
  off for VR anyway. Separately, an explosion near the player throws a RADIAL blur over the whole
  frame for ~100 ms; that is the flat game's own effect (the game's own round at nearby ground does
  it too), which is why the flash detector uses only the flash's saturated core.
- **The projection was right all along.** The handoff suspected the viewmodel camera's 16:9 aspect;
  a GPU capture shows the WORLD is rendered at 16:9 too (P11/P00 = 1.778): the engine takes the
  FOV from the pose block but ignores the aspect written there (see `RESEARCH.md`).

A mark is not always visible: Io's lighting drifts, and on sand lit by day, or where an earlier
step already left a hole, there is nothing new to find. Those steps are reported INCONCLUSIVE and
never counted as passes; a small random offset per run keeps marks from landing in old ones.

**Other weapons.**

- **Auto rifle** (slot 2, 600 RPM): a 1.5 s burst was 15 launches, 15 rewritten, 15 aim rays, 15
  traces -- every round goes through all three levers. Its rounds do not explode and its marks are
  tiny (only the 2 m one was found: 0.32°), but its impact sparks sit 0.15–0.96° off the fired ray
  in all 7 steps, the contrast 24°. In the side view its tracer is drawn from the real barrel's
  mouth to the point hit, converging onto the fired line.
  Recoil climbs the CHARACTER's pitch during a burst (12–14°) and the game recovers it on its own
  (0.9° a few seconds later): nothing accumulates, so nothing has to correct it.
- **Grenade launcher** (slot 3): one launch, one aim ray, one trace per grenade -- the same path.
  It explodes where the controller points (left and right follow it, ~100–250 ms flight), and in the
  side view the grenade is seen leaving along the line. Not measured to the degree: its "mark" is
  a diffuse scorch and smoke cloud, and a ballistic arc is not a straight line anyway.
- **Two hands** (rifle, hold taken, left hand 8 cm low): the hold swung the barrel 12.0° and the
  fired ray points 11.2° away from the right controller (the logged `ctrl_fwd` has two decimals) --
  the round follows the barrel the left hand steers, as it must since `weapon_sample` carries the
  final basis.
- **Multi-point patterns** (shotguns, twin barrels -- none in this loadout): the trace gate used to
  recognise only the first fire point, so rounds 2..N would have gone back to the camera's aim. It
  now remembers every point the launch rewrote.

**Muzzle.** Side views with the line on (`Show-Aim.ps1`, pose `side`): the line ran parallel to
the barrel but 2.3 cm (hand cannon) and 3.7 cm (rifle) below the bore. `up` is 0.10 now (was
0.07): through the middle of the hand cannon's barrel, the lower edge of the rifle's. The game draws
the muzzle flash at the viewmodel's own muzzle socket, the real mouth, which is ~0.33 m beyond the
line's start on the hand cannon. Forward is left INSIDE the gun on purpose: the gun hides where the
round starts, so the tracer reads as leaving the barrel on any length of gun, and the game's
eye-to-start wall check stays conservative. The original fire points cannot calibrate this: they
are the third-person model's, 0.7 m in front of the eye.

## 5. What is left

1. **Headset**: the user judges whether the line, flash and tracer look like they come out of the
   gun, and whether 0.10 up is right for their weapons. Note the layer change in `RESEARCH.md`: the
   image is no longer stretched vertically, so the black bands above and below are back.
2. **Comfort, out of this scope**: the radial blur of nearby explosions; the grenade launcher's
   recoil animation, which pulls the gun to the face; the hand cannon's flip after a shot (the next
   round follows the controller, not the animation -- intended).
3. Then the rest of `HANDOFF-ADS.md`: remove the ADS zoom, hide the HUD reticle, physical ADS,
   grenade to the left trigger, scopes, the dominant-eye switch.

Tools this work left behind, all reusable: `vr_prehook` (pre-hooks on any entry, virtualised or
not, plus post-calls), `vr_probe` (`SVR_Probe.txt`: hook, caller chains, args, aim matching),
`scripts/diagnostics/live_image.py`, `disp_refs.py`, `aim_scan.py`; for aim, `Test-Aim.ps1`
(`-TwoHand`, `-Prefix`, `-Magazine`, `-Only`), `Show-Aim.ps1` and `impact_find.py` (`--decal`, `--ray`).
