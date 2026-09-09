# Research notes — Destiny 2 (Tiger Engine) and Project Sunrise

Everything here was measured or read out of Sunrise's own source at master commit `7e3875d`,
unless marked as unverified. The target build never changes (fixed Steam depot manifest), so these
numbers are stable.

## Project Sunrise: what it is

A client-side mod that plays Destiny 2 offline on the **Season of Arrivals** build (2020),
restoring content vaulted in that year. It replaces `bin/x64/steam_api64.dll` with its own DLL:
the game loads that as the Steamworks API, and the DLL installs every hook from there. The build
is fetched with DepotDownloader — depots `1085661` (manifest `7180122903232116872`) and `1085662`
(manifest `2210332166360342287`).

Stack: C++20, MSVC, Microsoft Detours for hooking, ImGui for the overlay, Lua for content
definitions, SQLite. GPL-3.0 — a distributed fork must ship sources; private use carries no
obligation.

Supported today: loading destinations and exploring them, fly/noclip/teleport, basic inventory,
activity override, a HUD and debug overlays. **Not** supported: enemies, NPCs, quests, persistent
saves. Missions are appearing on master (Ember).

## Network behaviour and ban risk

Measured on a live session: the only TCP socket the game holds is `LISTEN 127.0.0.1:30974` — no
established outbound connection to any host. Sunrise's own config states
`server.gameplay.bind_address = "127.0.0.1"`. The UDP endpoints bound on `0.0.0.0` are inherited
from the original netcode with no remote peer.

`BEService` (BattlEye) exists on the machine as a leftover of the live game's uninstall but is
`Stopped`/`Manual`; the offline build never launches it. So there is no telemetry to Bungie, no
anti-cheat, and nothing to attach to an account. The mod's own FAQ agrees, with one caveat worth
respecting: **do not run the offline build and the live game at the same time**, because BattlEye
is aggressive about injectors and API hooking in other processes.

The real exposure is legal, not account-level: a Bungie/Sony cease-and-desist against the project.
Mitigation is to distribute code only, never game data.

## The executable's own anti-tamper (measured 2026-09-08)

BattlEye is gone, but `destiny2.exe` itself carries an anti-injection layer that survives in the
offline build, and it is what made every `LoadLibrary` of an OpenXR runtime fail with
`ERROR_DLL_INIT_FAILED` (1114). Measured by diffing the executable code pages of ntdll,
kernelbase and kernel32 between the game process and a clean process (same boot, same bases):

| Where | What the game did |
| --- | --- |
| `ntdll!DbgBreakPoint`, `ntdll!DbgUiRemoteBreakin` | replaced by `xor rax,rax; ret` (anti-debug) |
| `kernelbase!CtrlRoutine`, `kernel32!LoadAppInitDlls` | replaced by `xor eax,eax; ret` |
| `kernelbase!LoadLibraryExW`, `kernel32!GetProcAddress` | hot-patched (`EB F9` into a `jmp` in the pad) to trampolines in a private page |
| DLL notification list | one callback registered with `LdrRegisterDllNotification`, at `destiny2.exe+0x3AB4B0` |

The notification callback is the one that matters. The loader runs it for every newly mapped
module *before* that module's `DllMain`; afterwards the `DllMain` never executes and the load ends
in `STATUS_DLL_INIT_FAILED`. Established by bisection inside the process: mapping the same DLL with
`DONT_RESOLVE_DLL_REFERENCES` succeeds, FLS and TLS slots are free, no exception is raised, and a
three-kilobyte DLL with no CRT and a `DllMain` that only writes a marker file fails identically
with its marker never written. Restoring the two hot-patched exports alone changed nothing;
neutralising the callback made the very same load succeed on the next frame.

`xr_runtime.cpp` therefore does three things before loading a runtime, all reversible and all
logged (`ev=vr.xr unhook`, `ev=vr.xr ldr_notify`): it puts the on-disk prologue back over the two
hot-patched exports (read from the System32 image by RVA), registers a throwaway notification of
its own to find the list, and points the game's callback at a no-op. The game has shown no
reaction in the sessions since. ntdll's own code is byte-identical to the file, so the loader
itself is trusted as is.

Two side facts from the same investigation: the executable holds no `XInput` string at all, so
controllers are read through HID/raw input (the loaded `xinput9_1_0.dll` and `XInput1_4.dll`
belong to Windows components), and the loaded module list contains nothing foreign but
`TableTextService.dll`.

## Camera

### The pose block

`hooks/teleport/internal.h` maps it. Blocks are indexed by player:

| Field | Offset | Type |
| --- | --- | --- |
| block stride | `0xC50` | per player |
| position | `+0x594` | 3 floats |
| forward | `+0x5BC` (`position + 0x28`) | 3 floats |
| up | `+0x5C8` (`position + 0x34`) | 3 floats |
| horizontal FOV | `+0x5D4` (`position + 0x40`) | float, **radians** |
| aspect | `+0x64C` (`position + 0xB8`) | float |

Plain floats, no packing. Tag 0.3.2 knows only the stride and forward (`1468` = `0x5BC`), which is
why the fork is pinned to master instead.

**Basis: X forward, Z up.** Documented in both `fly.h` and `teleport/runtime.h`, and confirmed by
the engine's default forward of `(1,0,0)`. So yaw is a rotation about Z, and height is lane 2.
Converting from OpenXR (Y up, −Z forward) is therefore a basis swap, not just a sign flip.

The block's base pointer lives in an **obfuscated global**, so Sunrise never reads the global: it
decodes a near call at `camera transform + 0x72` to recover the singleton getter and calls it.

### Where to write it

`hooks/teleport/teleport_lifecycle.cpp` detours the camera transform (pattern
`48 89 5C 24 10 48 89 74 24 18 ...`). Its shape matters:

```cpp
std::int64_t __fastcall camera_transform(std::uint32_t playerIndex) noexcept {
    const CameraTransform next = original<CameraTransform>(kCameraSlot);
    const std::int64_t result = next != nullptr ? next(playerIndex) : 0;  // engine runs FIRST
    capture_camera_pose(playerIndex);                                     // Sunrise reads it
    ...
}
```

The original runs **before** the hook body, so at that point the engine has already produced the
frame's pose. That makes it the natural override site, and it is the only site that reaches the
block at all. The VR probe runs immediately after `capture_camera_pose`, so what the teleport and
the world lines read stays the engine's own pose while everything downstream sees the overwritten
one.

Reads and writes both go through `ReadProcessMemory`/`WriteProcessMemory` rather than raw
dereferences — Sunrise's own idiom, so a torn pointer returns false instead of faulting.

Third person vs first person matters: in the Tower the block drives an **orbit camera anchored to
the character**, so writing a head pose there moves a satellite around the Guardian. Patrol
destinations are first person, which is where camera ≈ head. A patrol zone is the test bed for
everything from F0.b on. Whether both modes use the same fields is an open question worth one
cheap check.

## VMProtect, and what that forbids (measured 2026-09-08, evening)

`destiny2.exe` is VMProtect-packed: it carries a `.vmp0` section and a second `.text` of 80 MB at
RVA `0x3CC9000`, and the first `.text` (RVA `0x1000`, 28 MB) is **encrypted on disk**. Bytes read
from the running process at a code address are not found anywhere in the file, and a static
disassembly of any RVA is garbage. Every disassembly has to come from the live process
(`scripts\diagnostics\disasm_range.py`, `disasm_sites.py`, both ReadProcessMemory-based).

Every thread of the game is `ThreadHideFromDebugger` (70 of 70 measured with
`NtQueryInformationThread`), so an attached debugger receives no events at all: hardware data
breakpoints set from outside never fire (`scripts\diagnostics\watch_access.py`, kept as evidence).

Set from **inside** the process they fire fine (`hooks/vr/vr_watch.cpp`: DR0 on every thread plus
a vectored handler; 369 traps in 400 ms on the camera forward), but **each such watch kills the
game a few seconds to minutes later**: `c0000005` at `destiny2.exe+0x7D6E6CE`, inside the
VMProtect section, identical on both occasions. That is VMProtect's anti-debug reacting to
single-step exceptions in the process. Conclusion: no hardware breakpoints, no guard pages,
nothing that raises exceptions. Ordinary detours (Detours transactions, which Sunrise uses on
`camera_transform`) are tolerated. `vr_watch.cpp` stays in the tree as a last resort; one watch
buys one relaunch.

### What the one successful watch found

Six code sites touch `block+0x5BC` (forward.x) per frame. Four belong to the engine's own camera
controller and run *before* Sunrise's hook (they read the pose into locals, transform it and write
it back: `+0x12D3CE0`/`+0x12D3DE4`, `+0x12D4B5D` writing pos/fwd/up from an inner state at
`block+0x96C..+0x9A4`, `+0x12D6DB0` using the `0xC50` stride). Two are **consumers of the written
pose**, i.e. what the VR module needs:

| Site | Rate | What it is |
| --- | --- | --- |
| `+0x12D22F5..+0x12D2313` | 4 per frame, 6 threads | **Pose getter**: copies `pos` (`+0x594`), `fwd` (`+0x5BC`), `up` (`+0x5C8`) out of the block into caller-supplied vectors (`rbx`, `rsi`, `rdi` at the copy). Return addresses seen on its stack: `+0xB3598D`, `+0x27EDB80`, `+0x2E60F90`, `+0x3BE636`, `+0x35FA33`, `+0x189093F`, `+0x12E9919`. |
| `+0x36B070` | 1 per frame, camera thread | Tiny helper: `movups xmm0,[rdx]` with `rdx = block+0x5BC`, masks to a vec3, stores to a stack local. Called from `+0xDB8CE1`, itself under `+0x31C501`. |

The viewmodel follows the written camera (measured on the Moon with the mock head yawed: the gun
stays put on screen while the world turns), so whatever draws it obtains the pose through one of
these two paths.

### The two paths, detoured (`hooks/vr/vr_weapon.cpp`)

Both functions are detoured with Sunrise's Detours wrapper; the game tolerates it (no crash in
several sessions), and the detours count callers by return address without raising anything:

| Function | Signature | Callers seen (RVA of the return address, per frame) |
| --- | --- | --- |
| `+0x12D22C0` getter | `void(Vec3* pos, Vec3* fwd, Vec3* up)`, local player only, NaN-checked | `B363FA` x3, `B3598D` x1, `B40A78` rare |
| `+0x12D50F0` pose pointer | `float*(int playerIndex)` = `block(index)+0x594` | `D98F49` x4, `DB8CB6` x1, `B35BEE` x1, `D7FB5C`/`D8492B`/`D7FF2C` sporadic (orbit) |

Experiment in orbit with the mock head at yaw ±40°, three configurations:

1. Default (block written with the head pose): the view turns with the head.
2. `getter all body` (block still head, getter callers handed the engine pose): **the view stops
   following the head**. So the rendered view takes its pose from the getter.
3. `block off` + `getter all head` (block left with the engine's orientation, getter callers
   handed the head pose): the view turns with the head again. Head tracking works with the block
   untouched, and every direct reader of the block keeps the body's facing.

Configuration 3 is the candidate F3 architecture: inject the head only where the render view reads
it. Whether the weapon viewmodel reads through the getter or through the pointer path decides how
much more work F3 needs; that test needs a first-person area.

#### First person (Io, 2026-09-08 evening): the weapon has its own getter caller

In first person a getter caller that never appears in orbit shows up, `D5D832`, exactly once per
frame (`B363FA` x3, `B3598D` x1, `D5D832` x1). With `block off` and a rule on a single caller,
mock head at yaw 30° (screenshots `build\shots\io_10_*`, `io_2*`):

| Rule | World | Weapon viewmodel |
| --- | --- | --- |
| `getter B363FA head` | turns with the head (yaw and pitch) | stays with the body: slides across the screen, leaves it at 30° |
| `getter D5D832 head` | stays with the body | turns with the head: swings across the screen |
| `getter B3598D head` | no visible change | no visible change |
| `getter none` | body | body |

So `B363FA` is the render view and `D5D832` is the weapon viewmodel, and they are already
independent inputs of the same function. **F3 decoupling is done by construction**: the view gets
the head pose, the weapon gets whatever pose the module hands to `D5D832`. The pointer path was not
needed for either; `B3598D` is still unidentified (no visual effect, possibly audio or culling).

Next: feed `D5D832` a controller pose (right aim pose from the action set, in game basis, folded
onto the body yaw like the head) instead of `body`/`head`. Aim validation with the `world_lines`
marker comes after that.

#### The weapon's transform builder, and where its position really comes from (2026-09-09)

`getter D5D832 hand` moves the weapon's **orientation** and nothing else. Measured in Io: with the
controller's offset driven a metre and a half sideways, the gun did not shift by a pixel, and
leaning the head towards it left it glued to the screen while the world moved behind it. The
pointer path (`ptr all hand`) does nothing for the weapon either.

Disassembling backwards from the return address explains it. The caller is one function,
`destiny2.exe+0xD5D7F0`, and it takes **one argument** (an output buffer in `rcx`; `rdx` and `r8`
are overwritten before use). It calls the pose getter with three stack locals, then:

| Where | What it does |
| --- | --- |
| `+0xD5D857..+0xD5D8A6` | builds `right = fwd x up` by hand, three `mulss`/`subss` triples |
| `+0xD5D892..+0xD5D8A3` | masks up a `1.0f` lane, the homogeneous row |
| `+0xD5D8AE..+0xD5D8CE` | three calls to `+0x36B070`, the vec3 load helper, one per axis |
| `+0xD5D8D3..+0xD5D8F1` | assembles the 4x4 `[fwd, right, up, (0,0,0,1)]` at `rbp-0x49` |
| `+0xD5D8F5` | `+0x465100` converts it (it squares and sums row 0, so it normalises) |
| `+0xD5D8FA..+0xD5D904` | copies **32 bytes** of the result into the caller's buffer |

**The position the getter hands it, at `rbp-0x59`, is never read anywhere in the function.** That is
the whole explanation: the engine takes the weapon's rotation from the pose and its origin from the
camera, so a viewmodel at a fixed distance can be aimed anywhere on a sphere and never moved off it.

Those 32 bytes are the lever. Detoured as a third path in `hooks/vr/vr_weapon.cpp`, the buffer at
rest reads:

```
0.0000, 0.0000, -0.7863, 0.6179 | 0.0000, 0.0000, 0.0000, 1.0000
```

Lanes 0-3 are a **unit quaternion** (`0.7863^2 + 0.6179^2 = 1.0000`) of a pure turn about the game's
up axis: `z = sin(theta/2) = -0.7863` gives `theta = -1.808 rad`, which is exactly the body yaw the
module logged that frame. Lanes 4-7 are a **homogeneous position, at zero**.

Lanes 4-6 were identified by forcing one at a time and template-matching the weapon between
captures:

| lane | forced +0.08 | forced -0.08 | reading |
| --- | --- | --- | --- |
| 4 | dx **-166 px** | dx **+160 px** | horizontal |
| 5 | dx +126 px, scale change | dx -56 px, scale change | depth |
| 6 | dy **-172 px** | dy +20 px | vertical |

With the camera at yaw -103.6 deg its own left in world coordinates is `(0.972, -0.236, 0)`, almost
the world's +X, so lane 4 moving the gun leftwards identifies it as **world X**; lane 5 as world Y,
which from that camera is nearly straight backwards, hence the scale change; lane 6 as world Z.

**So lanes 4-6 are a world-space offset added to the weapon's position, in the game's own axes --
the same basis the head and hand offsets are already in, needing no rotation.** And 0.08 m
subtending 13.5 deg puts the weapon **0.33 m from the eye**, which independently confirms that a
game unit is a metre and that `g_unitsPerMetre = 1.0` was right.

The configuration that passes the whole F3 matrix, and which F9 now applies by itself:

- head into the camera block (`block on`)
- `getter D5D832 hand` -- the controller's orientation
- transform buffer lanes 4/5/6 += `(hand.offset - handRest) - head.offset` -- the controller's
  travel, referenced to where the controller was when the weapon last rested

Referencing the controller's rest pose rather than the recentre origin is what keeps the gun's
resting place the engine's own tidy viewmodel position; it then tracks travel, which is what reads
as 6DOF. `xform delta abs` gives the geometrically absolute version if a true calibration is ever
wanted.

#### The render view's position does NOT come from the camera block

Established while hunting the above. With `block pos off` -- the block left carrying the engine's
own position -- and `getter B363FA head`, the world still leaned when the head leaned. So the view
takes both its orientation and its position from getter caller `B363FA`, and the block's position is
not what the renderer uses. Worth knowing before anyone tries to separate the two through the block
again.

## Rendering

`hooks/graphics/` already owns the D3D11 beachhead: swapchain selection, device and context,
render-target views, texture upload, and `renderer/world_lines.cpp` — a private,
depth-independent line pass that draws world-space points, axes, boxes, spheres and edges
**without touching the game's render caches**. That is both the debug renderer for F3's impact
marker and the ruler for calibrating world scale to metres.

`world_lines` builds its projection from a camera-relative basis
(`position`, `right`, `up`, `forward`) plus `1/tan(hfov/2)` and aspect — the engine exposes a
**pose, not a view matrix**, and the frustum it describes is **symmetric**. Real HMDs have
asymmetric per-eye frusta, so a VR path cannot express an eye's true frustum through this field.
The way out is to render a symmetric FOV per eye and declare that same FOV in the OpenXR
composition layer, which is legal: the runtime is told what was actually rendered.

## Mono, and the double vision it was blamed for (2026-09-09)

The first headset session reported the image as double and unreadable without closing one eye. Mono
was not the cause and could not have been: identical pixels in identical world directions fuse
without effort and read as flat and far away, which is what was agreed.

The cause was in the projection layer. One image was submitted twice but declared **at each eye's
own pose**. A projection layer is a geometric promise -- the compositor places the content in the
world according to the pose it is told, then lets each eye look at it from where that eye really is
-- so two different declared poses put the same pixels in two different world directions. That is a
constant, wrong disparity across the whole frame, and a couple of degrees is already far past what
fuses, so the brain suppresses an eye.

The fix is to declare the same **cyclopean** pose for both views: the midpoint of the eyes, with a
single averaged orientation. Disparity becomes zero and the image fuses at infinity. The head's own
forward is now taken from that same averaged orientation too, rather than from the left eye, which
on a headset with canted panels was yawing the whole view by half the cant angle.

**None of this reproduces in the mock**, which reports identical orientations for both eyes -- which
is exactly why it survived so long. `ev=vr.xr eyes` now logs both eye poses and both FOVs once per
session so a headset run leaves the evidence behind.

Two further image defects, independent of the above:

- **Black bands above and below.** The layer's vertical extent was derived from the back buffer's
  pixel ratio (16:9), giving about 71 deg declared where a Quest 3 eye has 98. The engine exposes an
  `aspect` field, which the module already computed correctly (1.074 for a Quest 3) and deliberately
  did not write; it is written now, and the layer derives its vertical from the aspect read back out
  of the block. The cost is a horizontally stretched desktop capture, which is why it was left
  unwritten while the weapon work needed undistorted screenshots.
- **Resolution.** The headset was getting a copy of a 1280x720 window. The engine accepts a windowed
  resolution **larger than the screen**: `2560x1440` was verified, logged as
  `ev=vr.xr swapchain size=2560x1440`, four times the pixels. The catch is that anything the mouse
  has to reach beyond 1920x1080 is then off-screen, the Director's LAUNCH button included, so
  scripted navigation breaks and so does a player's own use of the Director. 1920x1080 is the
  default for that reason; 2560x1440 is there for a session that does not need the mouse.

## Stereo, if it is ever wanted

Not in the PoC's scope, recorded so the reasoning is not lost. Tiger Engine is proprietary and its
render loop is not understood, so a true two-pass stereo frame would be a large reverse
engineering effort. **Alternate Eye Rendering** — write one eye's pose per frame, submit to that
eye, alternate — needs only camera writes plus a backbuffer copy, which is exactly what is already
available. It is the technique the R.E.A.L. mods use for GTA V, RDR2 and Cyberpunk. The cost is
that stereo disparity updates at half rate, which shows as shimmer under fast motion but behaves
acceptably for slow exploration.

## Prior art: the harness this project inherited

The mock OpenXR runtime and the scripted test loop were not written for this project. They came
from an earlier agent-built VR mod for a different game and were adapted, which is also why the
mock still reports itself as `DishonoredVR Mock Runtime` in the logs under `docs/evidence/`. What
carried across:

- **`mockxr/mock_runtime.cpp`**: a fake OpenXR runtime DLL that reports a stereo HMD, accepts a
  D3D11 session, hands out real D3D11 swapchain textures and no-ops the compositor. Activated by
  pointing `XR_RUNTIME_JSON` at its manifest. It exercises the entire OpenXR lifecycle **with no
  headset**. It reads HMD geometry from a text file, and the Quest 3 values are
  `1824 1968 -52 42 48 -50` (per-eye size, then left-eye FOV L/R/U/D in degrees, right eye
  mirrored).
- **The scripted step list**, which became `scripts/lib/GameIO.ps1` here: `sleep:`, `key:`,
  `cmd:`, `focus` steps plus a field protocol that feeds synthetic head and controller poses.
  Gotcha inherited with it: unspecified pose fields keep their previous values, and `headY` is an
  offset on a 1.6 m standing height, not the height itself.
- **Screenshot differencing**, which became `diff_panel.py` and `compare_shots.py` here, with the
  warning that a quadrant-limited diff reads as "no change" when the subject moves out of that
  quadrant. Use the full frame and look at the PNG.
- The lesson adopted from it verbatim: *prefer changes verifiable via log, overlay or screenshot;
  the user is the tester for anything needing the headset.*

## Destination loading (Sunrise side, measured 2026-09-08 evening)

Patrol zones are public bubbles and Sunrise holds their slice-set switch until a public activity
host connects, which is a race against the embedded server. The losing side is a black screen with
the client in `activity:in_world`, not a hang. `client.region_private = true` ends the race; so
does a forced destination. Full write-up, zone table and log signatures: `ZONES.md`. Our
`hooks/vr/vr_destination.cpp` makes the forced destination persistent through a file, using
Sunrise's `state::activity::forced::publish`, which its own panel exposes but never saves.

## OpenXR runtimes on this machine (measured 2026-09-08 evening)

- **Meta (Quest Link)**: manifest `C:\Program Files\Meta Horizon\Support\oculus-runtime\oculus_openxr_64.json`,
  library `LibOVRRTImpl64_1.dll`, reports **OpenXR 1.1.61**. Its `xrNegotiateLoaderRuntimeInterface`
  **refuses** a loader whose `maxApiVersion` is 1.0.x (`negotiate_refused` in our log). Verified
  with a standalone probe: 1.0.0..1.0.65535 → refused; any range reaching 1.1 → accepted. The
  module now announces 1.0.0..1.65535.x and creates the instance as a 1.0 application, which the
  runtime accepts. With that: `views count=2 eye=1824x1968`, Touch controller profile bound (13
  actions), session `running`, frames submitted, real head poses arriving.
- **SteamVR** is the system's `ActiveRuntime` in the registry. Not needed: the module is pointed at
  a runtime by setting `XR_RUNTIME_JSON` in the launching shell (the same path the mock uses), so
  no registry change is required for either.
- The runtime is chosen when OpenXR is first brought up (first F9), and the env var is read from
  the process, so switching between mock and headset means relaunching the game.
- The Link "desktop" view is irrelevant to the game: once F9 succeeds the game is the immersive
  app and replaces the Link environment. Before that the headset shows Link and nothing else.
- F9 is read by the camera hook, which only runs once a player camera exists (orbit or in world).
  On the title screen F9 does nothing, by construction.

First headset session (Quest 3, user, 2026-09-08 ~21:00): head tracking correct in yaw and pitch,
scale fine, no comfort complaints reported. Image quality "awful" because the game window is
1280x720 and the swapchain copies the back buffer as-is into 1824x1968 eyes; mono as agreed.
Weapon decoupling by rule confirmed subjectively. Both the resolution (run the game at 1920x1080 or
bigger) and stereo are open follow-ups, not PoC scope.

## Open questions

- Does the renderer honour a pose written at the camera-transform hook, or does something later in
  the frame recompute it? **This is the F0.b gate.**
- Does the engine clamp `horizontalFov`, or will it accept the ~110° a Quest 3 needs?
- Are the same block fields used in third and first person?
- Does Sunrise's Lua surface expose anything useful for VR, or is it only content definitions?
- Head yaw versus body yaw: whether to feed head rotation back into the game's look input so
  movement stays head-relative. The sister project needed a **servo** for this, not feed-forward,
  because mouse-counts-per-degree was not repeatable between sessions.
