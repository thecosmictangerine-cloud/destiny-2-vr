# Handoff: finish testing the barrel shot, polish it, then aiming and ADS (written 2026-10-15)

> **Status, later the same day: items 1–3 are done** — measured (every bullet hole within 0.47° of
> the fired ray), other weapons and two hands checked, muzzle height recalibrated. Results, method
> and what is left: `AIM.md` sections 4 and 5. Two premises below turned out wrong and are corrected
> there: the world is NOT rendered with the HMD's aspect (so the projection was never the problem;
> the flash of explosive rounds sitting ~20 cm above the hit was), and the layer was stretching the
> headset image (fixed, `RESEARCH.md`). Item 4 (ADS) is next, after the user's headset check.

**Next task, in order:**
1. Measure the barrel shot properly.
2. Test it across weapon types, including projectile weapons (rocket and grenade launchers).
3. Fix whatever breaks.
4. Only then continue with aiming/ADS (`HANDOFF-ADS.md`).

Read first: this file, then `AIM.md` (how the shot is redirected and why), then `CLAUDE.md` for the
build and test loop. `RESEARCH.md` and `WEAPON-6DOF.md` only when touching offsets or the gun's
placement.

## Where things stand

- **Done and proven on the mock, with a hand cannon (hitscan).**
  - The round leaves the VR muzzle along the barrel.
  - Exact proof: the round's hit test (`+0x1296E50`) receives the muzzle and barrel to four
    decimals.
  - Visual contrast: `build/shots/aim/contrast.png` shows left, right, and `shot off` back to the
    centre.
- **How it works.** Three pre-hooks in `hooks/vr/vr_aim.cpp`:
  - the launcher `+0x181E930`;
  - the round's aim ray `+0x12B0950`, as a post-call and only from `+0x12B06EA`;
  - the round trace `+0x181F680`, where two flags are cleared, recognised by content.
- **Not done.**
  - A degree-level measurement in the image.
  - Any weapon other than the hand cannon.
  - Two-handed.
  - The headset.
  - Nothing is committed (commit only when the user asks).
- **The user's goal:** "excellent, robust, not a hack". They test on a Quest 3 at milestones.

## 1. Make the measurement trustworthy first

`scripts/testing/impact_find.py` finds the spark burst reliably: a capture just before the shot, one
~90 ms after, and a null pair. It measures the miss against the barrel line, projected from
`ev=vr.aim line` (NDC origin and vanishing point).

That projection uses the **viewmodel** camera's aspect (1.778). The world is rendered with the
**HMD's** aspect, which `vr_camera.cpp` writes into the camera block. So the vertical scale is wrong
and the ~3° it reports is not real.

**Fix:**
- Take the world projection the GPU actually got. `vr_gpu.cpp` logs `ev=vr.gpu rewrite ... world_p=P00,P11`.
- Have `vr_aim.cpp` `report_line` emit NDC computed with those values, or pass them to `impact_find.py`.
- Check that the drawn magenta line (`SVR_Aim.txt`: `ray on`) and the Python projection coincide in
  one screenshot before trusting either.
- Cross-check with geometry: in `s3_pair.png` the ground is at z ≈ 40.09, from the trace's own args.
  The muzzle and barrel from `ev=vr.aim shot` therefore predict the impact pixel independently of
  any line drawing.

**Then run `scripts/testing/Test-Aim.ps1`:**
- Null; hand left, right and up; head left and down; roll; `near` (origin); `turn`.
- The `game_*` contrast, which must FAIL with `shot off`.

Pass bar to aim for: < 1° on hitscan, beyond the weapon's own spread. Fire the first round of a
burst (Destiny's first shot is accurate). Reload is handled: `R` was added to `GameIO.ps1`.

**Add a two-handed step**, using the setup from `Test-TwoHand.ps1`: hold taken, barrel steered by the
left hand. The impact must follow the left hand. It should work by construction, because
`weapon_sample` carries the final basis, but prove it.

## 2. Weapon types: the real risk

Only a hand cannon has been through the path. What may differ:

**Projectile weapons (rocket launcher, grenade launcher, fusion, sniper, bow):**
- They may launch through the same `+0x181E930`, but the round may not be a trace at all. A
  projectile entity may be spawned that flies from the fire point along its orientation, and possibly
  toward the camera's aim target.
- Test whether the launch rewrite alone is enough (look for `traces_kept` not increasing).
- If the projectile still flies toward the screen centre, find where it gets its target. Use the
  probe on `+0x181C8B0`'s callees, as was done for the hitscan:
  - `SVR_Probe.txt`: `hook <rvas> name`, then `args`, `scan on`, and fire.
  - The aim-match logs `ev=vr.trace aim ...`, flagging any argument that carries the character's aim
    or the eye.
- Watch the gating in `on_launch`. The first fire point must be within 2 m of the eye and within ~25°
  of the aim. A heavy weapon's fire point may sit further out; log it before widening anything.

**Shotguns:** many fire points. `on_launch` rotates the whole pattern rigidly by `D`. The trace gate
in `on_round_trace` only matches the barrel ray exactly, so pellets 2..N may not match and may keep
the camera aim. Check `traces_kept` against the pellet count, and fix the matching, probably by
remembering every rewritten point, not only the last one.

**Auto rifles and SMGs:**
- Each round re-enters the path. Check that every round is rewritten at high rate of fire.
- After a shot the rotational seat follows the recoil animation (the barrel line dips ~6°). Decide
  whether rounds 2..N should follow that kick (realistic) or not.

**Sword or melee:** must be untouched. Check that nothing logs `rewritten` for it.

**How to get the weapons:**
- The character's loadout shows a hand cannon plus two more slots. Swap with `Y` on the mock
  (`BtnY`, see `Test-TwoHand.ps1`) or the keys 1/2/3.
- Heavy ammo: Sunrise has infinite ammo (`hooks/infinite_ammo`). The flag lives in `player.json` in
  Sunrise's artifact directory (`player_settings_store.cpp`), probably
  `C:\Games\Sunrise\bin\x64\Sunrise\player.json`, or toggle it in the Insert menu. Verify where.
- If the loadout has no launcher, ask the user. Do not spend a session in menus.

**Capture timing for explosions:** an explosion is big and bright, so `impact_find.py` will find it.
Rockets travel, so take the "after" capture later (time of flight).

## 3. Polish, if the tests call for it

- **Muzzle offset** (`muzzle <fwd> <right> <up>`, default `0.20 0 0.07` from the palm).
  - With `ray on` the line should leave the drawn barrel's mouth.
  - Only the cross-bore components change where rounds go; the forward one is cosmetic.
  - It may need to be per weapon. If so, derive it the way the foregrip was learned: from the game's
    own animated hand/gun relation (`vr_arms.cpp`), not a table.
- **Spread:** stays the game's, applied after the rewrite. Fine for now; ADS will tighten it.
- **The once-a-frame aim-ray call** from `+0x12B0C28` is left on the camera. It likely feeds aim
  assist or "what am I looking at". Leave it unless a test shows it matters.
- **Optional laser** for the player: `ray on` already draws it into the headset image. Decide with
  the user whether to keep it as a comfort option, and if so shorten it to the hit point.

## 4. Then: aiming and ADS

Follow `HANDOFF-ADS.md`. Its "shots from the barrel" step is now done. What remains, as the user
decided:
- **Kill the ADS zoom first.** It is a nausea risk already live with the left trigger while
  two-handing.
- Hide the HUD reticle.
- Physical automatic ADS (sight to the eye), with thresholds live-tunable.
- Grenade to the left trigger.
- Magnified scopes last.
- The dominant-eye render switch.

The probe tooling below is how to find the zoom: watch `world_p` and the block FOV through an ADS
cycle.

## Tools this left you

- **`vr_prehook`:** pre-hooks on any entry, including VMProtect-virtualised ones (their entry is a
  five-byte `jmp` into the VM, retargeted). It gives you all argument registers and stack args, and
  post-calls via return-address swap.
- **`vr_probe`** (`SVR_Probe.txt`):
  - `hook <rva>[,...] [name]` logs distinct callers with full chains, through the unwind tables.
  - `args <rva> <n>` dumps registers and the floats behind pointers.
  - `scan on` flags the character's aim or eye in arguments while the left button is down.
  - **Never hook `+0x187CB30`** (`__chkstk`: takes `rax`, which the thunk clobbers).
- **`scripts/diagnostics/live_image.py`:**
  - `dump` once per session (decrypted image to `%TEMP%`, never the repo);
  - then `func`, `fdis`, `dis`, `xref`, `calls`, `find`.
  - The original x86 of virtualised functions is still there after their entry `jmp`. If a linear
    disassembly desyncs after a jump, start it a few bytes later.
- **`disp_refs.py <disp>`:** every instruction using a field offset. Finds a structure's users.
- **`aim_scan.py`:** heap scan for a value across states, plus poke/probe. It showed the character's
  pitch is only stored in derived copies.

## Traps

- Prologue bytes are checked before every hook. A typo there (`48` for `49`) costs a full reboot
  cycle (~7 min): copy them from `live_image.py dis`.
- Compare captures seconds apart. Io's lighting changes between sessions: day-like and night-like
  were both seen.
- The sparks are a red-shifted brightening. Swaying grass changes R and G together; that is why
  `impact_find.py` measures red gain over green gain.
- The game log rotates (`sunrise.log.old`) during long sessions. `Read-VrLog` only reads the current
  file.
- PowerShell 5.1: `$cases` and `$Cases` are the same variable. `mouse_event` takes `uint32`, so a
  negative delta must be passed two's-complement.
- Everything else in `CLAUDE.md` still applies (locale decimals, focus, the servo, `near`/`far`
  macros).
