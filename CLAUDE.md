# Destiny 2 Sunrise VR mod — project context

Monoscopic 6DOF VR mod for **Project Sunrise** (offline Destiny 2, Season of Arrivals build), as a
proof of concept. Engine internals live in `docs/RESEARCH.md`; read it before touching offsets.

## Layout

- `sunrise-vr/` — git clone of `stanuwu/Sunrise`, branch `vr`, `upstream` remote, pinned to master
  commit `7e3875d`. All mod code goes in `Sunrise/src/client/hooks/vr/`.
- `tools/` — the Sunrise installer and the official 0.3.2 DLL, kept for comparison and rollback.
- `docs/` — research and status. Project-level docs live HERE, not inside the fork, so nothing of
  ours can ever conflict with upstream.
- Game install: `C:\Games\Sunrise` (~93 GB, separate from any live Destiny 2).

## Scope (agreed with the user)

PoC, **no stereo**. Everything is monoscopic: one view rendered, same image to both eyes. Head
tracking 6DOF carries most of the sense of presence; stereo disparity matters mainly within a few
metres and these are big open spaces. The user knows the world will read as flat/infinitely far.

Phases: **F0.a** build from source · **F0.b** prove the camera pose can be written · **F1** OpenXR
+ 6DOF mono head tracking · **F2** controllers as a gamepad · **F3** weapons 6DOF.

F3 is in scope even though Sunrise has no enemies: it retires the hardest remaining risk
(decoupling the weapon viewmodel from the camera). Validate aim with a `world_lines` impact
marker, not with something to shoot.

## Fork discipline (option C)

All VR code in its own directory; upstream files get only additive lines. Current footprint into
theirs: **48 insertions, 5 files** (`Sunrise.vcxproj`, `graphics_renderer_lifecycle.cpp`, and three
in `hooks/teleport/`). The only non-addition is the `AdditionalIncludeDirectories` line, extended
with `vendor\openxr\include`.
Keep it that way — it is what makes `git pull upstream` merge cleanly. `Sunrise.vcxproj` lists all
931 sources explicitly with no wildcards, so new files must be added to it by hand.

The target build is pinned to a **fixed Steam depot manifest**, so reverse-engineered offsets can
never be broken by a game patch. Only Sunrise's own internals can drift.

## Build & test loop (Claude runs this solo)

1. `sunrise-vr\scripts\build.ps1` → `sunrise-vr\build\x64\Release\steam_api64.dll`
2. `sunrise-vr\scripts\deploy.ps1` → swaps it into `C:\Games\Sunrise\bin\x64\`
   (`-Restore` puts the stock DLL back)
3. `sunrise-vr\scripts\lib\GameIO.ps1` → dot-source for `Start-Game`, `Wait-GameWindow`,
   `Focus-Game`, `Send-GameKey`, `Send-GameText`, `Send-GameClick`, `Save-Shot`,
   `Invoke-GameSteps`, `Get-GameStats`.
4. Read the PNG for visual verification; read `C:\Games\Sunrise\bin\x64\Sunrise\logs\sunrise.log`
   for the structured log (note the `logs\` subdirectory). The game window appears on the user's
   screen — that is fine and expected.
5. Verification scripts, all in `scripts\testing\`: `Test-HandPose.ps1` (controller pose, every
   axis and rotation against hand-computed values, orbit is enough), `Test-WeaponPivot.ps1` (**the two
   regressions the rest of the suite walks straight through**: pure rotation must not translate the
   weapon, and artificial turning must not either; `-Solve` also measures the pivot constant by a
   least-squares solve rather than a search, and `Probe-Pivot.ps1` is the visual reconnaissance to
   run before believing it — the absolute placement moves the gun's rest position, and if it has
   left `weapon_shift.py`'s template box the test would measure a beautiful zero for the wrong
   reason), `Test-Weapon6DOF.ps1` (the F3
   matrix: hand moving with the head still, then head moving with the hand still; `-Quick` for just
   the decisive steps), `Test-BodyServo.ps1` (servo convergence, horizon stability, and that walking
   follows the gaze), plus the measurement helpers `weapon_shift.py` (template match, exact to a
   pixel), `diff_panel.py` (A/B difference sheets), `contact_sheet.py` and `weapon_metric.py`.
   **Compare captures taken seconds apart, never minutes**: Io's lighting drifts and the weapon has
   an idle animation, so a whole-frame metric across a long session has a noise floor of tens of
   pixels, while an A/B pair plus a null control pair is reliable.
6. `sunrise-vr\scripts\testing\Run-Orbit.ps1` is the whole boot-to-orbit-to-F9 cycle on the mock
   (about 4 min), with screenshots in `build\shots` and both logs dumped at the end.
   `Run-Ember.ps1` continues through Sunrise's Activity override into a mission (see gotchas).
   `scripts\diagnostics\` holds the loader forensics (code-page diff, symbol naming, probe DLLs).

`build.ps1` and `deploy.ps1` both kill the game first. To rebuild while a session is being used,
call MSBuild directly with the two overrides below; only the deploy needs the game gone.

**Two MSBuild overrides are mandatory and are NOT project changes:**

- `/p:PlatformToolset=v143` — the project asks for v145 (VS 2026); only VS 2022 BuildTools is here.
- `/p:PreferredToolArchitecture=x64` — without it MSBuild picks the 32-bit `HostX86` compiler,
  which dies with `error C1060: out of compiler heap space` on this codebase's C++20 templates.

Cap parallelism at 4: 16 GB has to hold four compilers and often the running game. Clean build is
about 3–4 minutes.

## The VR module

`Sunrise/src/client/hooks/vr/`:

- `vr_camera.cpp` — the camera probe and the body servo. Reads the pose the engine just produced,
  lays the head pose or the manual deltas over it, writes it back, and re-reads the block to prove
  the store landed. It also owns the **room anchor**: the world yaw that the direction the player
  faced at the last recentre maps to. The view is built from that anchor plus the head's own
  rotation, so nothing but an explicit turn can move the horizon — and that is what makes it safe
  for the servo to drive the character round to face wherever the player is looking.
- `xr_runtime.cpp` — OpenXR. **Its own mini-loader**: the only OpenXR loader build on this machine
  is compiled against the dynamic CRT while this DLL uses the static one, so instead of linking it
  the module reads the active runtime's manifest (`XR_RUNTIME_JSON`, else
  `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`), loads the library and negotiates the interface
  itself. The mock and a real runtime are reached by the same path. It also copies the presented
  back buffer into the runtime's swapchain (direct `CopyResource` into an sRGB-typed image when the
  format allows, a shader blit otherwise) and submits one projection layer, the same image for
  both eyes, at the eye poses and the FOV the frame was actually rendered with (read back from the
  pose block every frame).
- `vr_gamepad.cpp` — controllers as keyboard and mouse. The game reads pads through HID, not
  XInput (the executable holds no XInput symbol), so an XInput detour feeds nothing. Instead the
  action set (left stick, right stick, triggers, grips, A/B/X/Y, stick clicks, menu) is mapped onto
  `SendInput` scancodes and relative mouse motion, the same path the harness uses. Runs once per
  present; injects only while the game has focus and the mod UI is closed, and releases every key
  the moment either stops being true. Two departures from a plain pad mapping: the **right stick
  turns the room anchor** by an exact number of radians instead of injecting mouse counts (no
  calibration, no drift with the game's sensitivity), and **locomotion is rotated by how far the
  character still lags the gaze** before it reaches WASD, so forward means forward even while the
  servo is catching up or cannot inject at all. Vertical stick look is dropped: pitch comes from
  the player's neck.

- `vr_weapon.cpp` — the weapon on the controller. Detours three engine functions: the camera pose
  getter (`+0x12D22C0`, whose caller `D5D832` feeds the weapon and `B363FA` the render view), the
  pose pointer (`+0x12D50F0`), and **the weapon's own transform builder (`+0xD5D7F0`)**. That third
  one is what makes 6DOF possible: the pose the weapon is handed carries only an orientation — its
  position field is never read — while the transform builder's 32-byte output holds a quaternion
  and a **world-space offset** the weapon is placed by. See `docs/RESEARCH.md` for how the eight
  floats were identified. F9 applies the proven configuration by itself; the command file
  (`SVR_Weapon.txt`) remains as a live override, and `xform force`/`xform lanes` are there for
  probing without a rebuild.

  **The placement is stateless, and that is the whole design** — see `docs/WEAPON-6DOF.md`, which
  is the current description; `docs/HANDOFF.md` is the diagnosis that led to it. One equation:

  ```
  lanes = (hand.palm − head.offset) + R(q) · pivot
  ```

  Everything on the right is read from the same frame, out of one lock acquisition, so nothing can
  go stale. The room anchor's yaw fold is inside both terms and cancels in the difference, which is
  what makes artificial turning geometrically incapable of moving the weapon relative to the
  player. `pivot` is the only constant: three floats in the controller's basis, cancelling the
  engine's own 0.33 m viewmodel offset so the gun pivots about itself instead of about the eye.
  It is **measured**, not guessed: `Solve-PivotRoll.ps1` drove the translation caused by a wrist
  roll from 72.0 px to **0.0 px** and gives **(-0.3300, -0.0915, 0.1090) m**, whose magnitude of
  0.359 m agrees to 9% with the 0.33 m measured by an unrelated route in `RESEARCH.md`. Solve on
  **roll**, never yaw or pitch: a roll preserves the gun's silhouette, while a yaw foreshortens it
  and correlation then reports about 100 px of offset with no translation behind it — the same size
  as the effect. Roll is blind to its own axis, so the forward component comes from RESEARCH.md.

  Two defects from the first Quest 3 session are fixed by that formulation (a captured rest
  reference held in the folded frame, and the uncancelled pivot term), plus three robustness holes
  found afterwards: lost controller tracking used to make the gun **snap** to the engine's default
  spot (it now holds the last good pose, kept **unfolded** and re-folded each frame); the
  orientation and its correction were sampled separately and could straddle a frame boundary (the
  getter now latches the exact sample into a `thread_local` that the transform detour consumes —
  guaranteed pairing, because `+0xD5D7F0` calls the getter itself); and the offset was applied to
  every caller of the transform builder (`xform caller` gates it).

  **The controller's position comes from OpenXR's `grip` pose, not `aim`.** They are not
  interchangeable: `aim` is a pointing ray whose origin floats in front of the hand, so a weapon
  anchored there pivots outside the player's fist however well `pivot` is tuned. `grip` is the palm
  centroid — the point a wrist really rotates about. Orientation still comes from `aim`, which is
  defined as the pointing ray of a gun-like hold. `anchor palm|aim` switches it live.

  Commands added: `pivot <fwd> <right> <up>`, `anchor palm|aim`, `turn <degrees>` (turns the room
  anchor by an exact angle — the test lever for the turning regression), `xform caller all|<rva>`.
  `xform ref` is now a no-op and F7 no longer retakes anything: there is no reference left to
  retake.

**The executable's anti-tamper blocks every foreign DLL.** `destiny2.exe` registers a DLL-load
notification callback (`+0x3AB4B0`) that stops any new module's `DllMain` from running, plus
hot-patches on `kernelbase!LoadLibraryExW` and `kernel32!GetProcAddress`. That was the 1114 of the
first handoff. Before loading a runtime, `xr_runtime.cpp` restores the two exports from the on-disk
image and points the callback at a no-op (`ev=vr.xr unhook`, `ev=vr.xr ldr_notify` in the log).
Details and evidence in `docs/RESEARCH.md`.

**The body servo will not fight a character that is not answering, and that matters for testing.**
It stops injecting after 20 frames of no response and logs `ev=vr.body servo=stuck`. Two situations
produce it, both seen on hardware: a world transition, where the character is not there to be
turned, and an **unfocused game window**, where the injection is gated off before it reaches
anything. Before the guard existed the gain learner ran away (507 → 3211 counts/rad), injected about
28,000 mouse counts a second, and **froze the game one second after a destination finished
loading** — which reads as a stuck loading screen. Full write-up in `docs/BODY-SERVO-HANG.md`.
Consequence: **click the game window before a headset session**, and expect the character not to
follow the gaze while the window is unfocused.

**F9 off keeps the OpenXR frame loop running.** It gates the camera write and the input injection,
nothing else. Returning early — which is what it used to do — left the session alive with no frames
to show, so the headset held the last one and F9 read as "it will not come out of VR". While off the
frame ends with no layer, so the runtime shows its own environment.

Hotkeys, live whenever the mod's own UI is closed and the game has focus:

| Key | Effect |
| --- | --- |
| F9 | Toggle the module. Also what brings OpenXR up, lazily, on the first present after. |
| F10 | +45° of yaw trim (wraps) |
| F11 | +1 unit of height (wraps at 8) |
| F8 | Reset switch and all trim |
| F7 | Recentre — takes the next head position **and yaw** as the origin, and re-seats the weapon's rest position |

The OpenXR frame runs on the present thread, so the camera lags the headset by one frame. Fix that
first if the view ever feels swimmy.

## Camera, body and locomotion (agreed 2026-09-09, verified in Io)

Built the way native VR games are, after the user pushed back on an earlier reading of the question:

- **Turning the headset turns the horizon and the character.** The horizon turns exactly with the
  neck; the character follows a moment later, in silence.
- **Turning with the stick turns both together**, as artificial turning always has.
- **Walking goes where the player is looking**, because the character's facing and the gaze coincide.
- **No roomscale.** Head translation moves the camera only — parallax, leaning, crouching — and
  never the character. Walk far enough in the room and the camera separates from the body and can
  pass through a wall; that is the accepted trade for a PoC, and it is where the presence comes from.

The ordering matters and is the whole reason this took a design discussion. The horizon used to hang
off the character's yaw, so making the character follow the head in *that* arrangement would rotate
the world on its own a moment after every head turn — a reliable way to make someone sick. So the
horizon is re-anchored to a **room anchor** the module owns, and only then may the character chase.

Two properties fall out of that ordering, both verified by `Test-BodyServo.ps1`:

- **The servo cannot move the horizon.** The view is the anchor plus the head, full stop. A servo
  that lags, overshoots, or cannot inject at all costs only the character's alignment — legs,
  direction of travel, and later where a bullet goes. Measured: `mad 2.53` between a capture taken
  while the character was turning and one after it settled, against a scene noise floor of 1.5–2.6.
- **The mouse-counts-per-radian factor is learned, not assumed.** It converged on 579 on this
  machine from a starting guess of 900, and the residual error at rest is 0.4°.

One lesson worth not repeating: the servo's own corrections must be **credited** before deciding
that a yaw change came from outside (a teleport, a respawn). The first version compared the raw
per-frame change against a threshold its own maximum correction exceeded, so it read its own work as
a teleport, carried the anchor along, and chased its tail — the error sat at a constant 1.05 rad
while 402,978 mouse counts went out and the character walked in circles. After the fix the whole
session used **10** counts.

Consequence to know: with VR active the physical mouse no longer turns the view, because the servo
corrects it back. For headset play that is right; for desktop poking it is surprising.

**Basis conversion**: OpenXR is +X right, +Y up, −Z forward, metres. The game is X forward, Z up,
right-handed, which puts its **+Y axis pointing LEFT**. So `(gx, gy, gz) = (−xz, −xx, xy)` — not a
sign flip. The body yaw from the engine's own camera is folded into the published pose, so mouse
look still turns the body and the head adds on top.

**FOV and aspect**: the engine exposes one *symmetric* horizontal FOV, so the module publishes the
smallest symmetric frustum containing both eyes. `aspect` **is** written now (it was not, until
2026-09-09): without it the layer's vertical extent came from the back buffer's 16:9 pixel ratio,
declaring about 71° where a Quest 3 eye has 98, which put black bands above and below the image.
The cost is that the desktop capture is horizontally stretched, so screenshot-based verification has
to allow for it — that is why it was left unwritten while the weapon work needed undistorted frames.

**Mono is submitted at one cyclopean pose, not at each eye's.** Declaring per-eye poses for a single
image is what caused the double vision of the first headset session; the reasoning is in
`docs/RESEARCH.md`. It cannot be reproduced in the mock, which reports identical orientations for
both eyes.

## Mock OpenXR harness

`scripts\build-mockxr.ps1` builds `build\mockxr\SunriseVR_MockXR.dll` with one `cl.exe` call (it
IS a runtime, so it must never link the loader — headers and d3d11 only, no vcpkg, no CMake).

Driven by two files in the **game's** directory, `C:\Games\Sunrise`:

- `SVR_MockHmd.txt` — `w h fovL fovR fovU fovD`; Quest 3 is `1824 1968 -52 42 48 -50`.
- `SVR_MockInput.txt` — synthetic poses, including `handRoll`/`lhandRoll` (appended last, so old
  step lists keep working). Roll is the axis a wrist actually turns about and the one the weapon's
  pivot error shows up in, so it is not optional for testing. Use `Set-MockInput` from `GameIO.ps1`, which writes every
  field explicitly: the mock keeps the previous value for anything omitted, so partial writes
  stick across steps in a way that is very easy to misread.

**With no input file the mock sweeps head yaw as a slow sine, ±34°.** That is the cheapest possible
end-to-end check: if the pipeline works, the view pans on its own with nothing driving it. **Once
an input file has been read the head stays file-driven for the rest of the session**, even after
`Clear-MockInput` deletes the file: to move the head again, write new values.

The mock reads both files and writes `SVR_MockXR.log` relative to the game's working directory,
and keeps the log open, so read it with a sharing-tolerant reader (`[IO.File]::Open` with
`FileShare.ReadWrite`), not `Get-Content`. Its action-state lookups are keyed on action **names**
(`move`, `turn`, `trigger_r`, `btn_a`, ...), so the names in `xr_runtime.cpp` are load-bearing. The
pose actions especially: `xrCreateActionSpace` decides which hand a space belongs to from the last
character of the action name, so they must stay `aim_r` and `aim_l` — and the grip poses
`palm_r` and `palm_l`, which the mock also recognises by the `palm` prefix and then offsets 7 cm
back and 3 cm down in the controller's own frame, as real hardware does. Without that offset,
confusing `aim` for `grip` is undetectable in the mock and only shows up in the headset. Note
`grip_l`/`grip_r` are already taken: those are the squeeze-value actions, which is why the pose
actions are named `palm_*`.

`Enable-MockXr` points the next launch at it. Our copy of the mock also reads the pose file from
`xrWaitFrame`, not only `xrSyncActions`, so a build with no action sets attached is still
steerable.

## Verification: what Claude can and cannot judge

Can, alone: launch the game, screenshot at full resolution, inject keyboard/mouse the game
accepts, drive the mod's ImGui menu, read the structured log, and — once the mock OpenXR runtime
is ported from the sister projects — exercise the whole OpenXR lifecycle with synthetic head and
controller poses.

Cannot, ever: latency, judder, perceived scale, comfort, nausea. **The user is the tester for
anything needing the headset.** Build and verify in the mock; ask for hardware validation at
milestones, not for debugging.

## Gotchas

- **Screenshots must be DPI-aware.** The desktop is 1536x864 logical / 1920x1080 physical; without
  `SetProcessDPIAware` the capture silently crops to the top-left. `GameIO.ps1` handles it.
- **Injected input must use SCANCODES** via `keybd_event`/`mouse_event`. WM_-level fakes
  (`SendKeys`) are ignored by the game's raw-input path.
- **In-game `Enter` opens the chat box**, it does not confirm. Character select needs a mouse click.
- **This machine's locale writes decimals with a COMMA.** `"$value"` on a PowerShell double
  produces `0,6`, the module's `sscanf` stops at the comma and reads `0`, and the command silently
  does nothing — which reads exactly like a broken feature. Always
  `([double]$v).ToString([System.Globalization.CultureInfo]::InvariantCulture)` when a number goes
  into a command file. `Set-MockInput` already does this; anything new must too.
- **The game window may be LARGER than the screen.** `windowed_resolution_width/height` in
  `%APPDATA%\Bungie\DestinyPC\prefs\cvars.xml` accepted `2560x1440` and the swapchain copy went
  with it, four times the pixels of 720p. But then everything past 1920x1080 is unreachable by the
  mouse, the Director's LAUNCH button included, so scripted navigation and the player's own
  Director both break. The default is **1920x1080** at position `0,0`; a backup of the original
  sits beside it as `cvars.backup-vr.xml`.
- **PowerShell 5.1 will not parse `-replace 'a', 'b'` inside a hashtable value in an `if`
  expression** — wrap it in parentheses. Nor does it have heredocs: `python - <<'PY'` is a parser
  error there, so run inline Python through the Bash tool instead.
- **Never write files with PowerShell `Set-Content -Encoding UTF8`** — 5.1 emits a BOM and it
  breaks `settings.json` for every JSON parser. Use the Write tool, or
  `[System.IO.File]::WriteAllText($p, $t, (New-Object System.Text.UTF8Encoding($false)))`.
- **The Bash tool collapses `\\` to `\` inside heredocs**: write C++/PowerShell files with the
  Write/Edit tools, never `cat <<EOF`.
- **MSVC talks Spanish here** (`error C1060: espacio de montón insuficiente en el compilador`).
- Logs are off by default: `core.logging.file_sink` must be `true` in
  `C:\Games\Sunrise\bin\x64\Sunrise\settings.json`, and channel levels default to `warn`, so log
  probe output at `warn` or raise the level.
- Launch `C:\Games\Sunrise\destiny2.exe` **directly, never through Steam**.
- **Patrol zones load black at random unless `client.region_private` is `true`** in
  `settings.json` (public bubbles wait for a matchmaking host that may never come; the Tower is
  social and never hits this). Set since 2026-09-08. Symptom: `activity:in_world` in the log, Esc
  works, no world/HUD/audio.
- **Straight into a patrol zone**: `C:\Games\Sunrise\SVR_Destination.txt` with
  `<package> <bubble> <slice_set> <activity_index>` (Io Giant's Scar: `eden_freeroam 20 160 7`;
  the index is mandatory in practice, see `docs/ZONES.md`) is read by
  `hooks/vr/vr_destination.cpp` in orbit and forced through Sunrise's own override; any Director
  launch then lands there. Rename to `.off` to give the Director back. Table of zones in
  `docs/HANDOFF.md`.
- Deterministic entry to a destination: `state.activity.default_destination` in `settings.json`
  holds `package_name`, `activity_index`, `bubble_count`, `stateful_bubble_mask`,
  `initial_slice_set` and `spawn_set_hash`. Sunrise's own Activity override panel (Insert →
  Activity) is **session-only**, nothing it sets is saved, so a scripted run has to redo the
  clicks every launch (`Run-Ember.ps1` does). `mission_ember` / `281 1AU` / bubble 6 / slice 49
  never finished loading in the mock session (ship-in-flight screen for 5+ minutes,
  `mission_script ... no_script`), so it is not a usable test bed; the user picks missions.
- **The Director needs real pointer motion.** `SetCursorPos` + click on a node only pans the map;
  hover with a couple of relative `mouse_event` moves, wait about a second for the card
  ("Select" / LAUNCH), then click. `Hover-Click` in `Run-Ember.ps1` does exactly that. Tower path:
  OPEN DIRECTOR (960,848) → Tower (960,250) → landing node (908,290) → LAUNCH (1587,884).
- **The executable is VMProtect-packed.** Code is encrypted on disk (static disassembly is
  garbage; read code from the live process), every thread hides from debuggers, and any
  single-step/hardware-breakpoint exception in the process gets the game killed a few minutes
  later at `destiny2.exe+0x7D6E6CE`. Detours are fine; exceptions are not. See RESEARCH.md.
- **`deploy.ps1 -Restore` restores Sunrise 0.3.2**, which cannot read this 0.4.0 install and exits
  with "Problema al leer el contenido del juego". Build upstream `7e3875d` for a real control.
- **Do not Alt+Tab out of the game in exclusive fullscreen** (it froze once, unrecoverably) and
  do not leave it unfocused during a destination load until the loading hang of 2026-09-08 is
  understood (docs/HANDOFF.md). The user has switched it to Windowed 1280x720, which invalidates
  every hard-coded click coordinate; Windowed Fullscreen would keep 1920x1080.
- **The loading transition (ship in flight) renders black while the module writes the camera**,
  with the mock at 104° FOV and a level head. Toggling F9 off shows the scene at once. Not yet
  bisected between the FOV and the up vector; harmless for a PoC, but do not mistake it for a hang.

## Rules

- No game assets and no Bungie code in the repo. Never build inside the game directory.
- Commit only when the user asks. Work on branch `vr`.
- The user wants the plan agreed before implementation starts, and wants honest statements about
  what can and cannot be self-verified. Do not overstate autonomy.
