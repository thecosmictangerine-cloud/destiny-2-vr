# Destiny 2 Sunrise VR mod

Six degrees of freedom VR for [Project Sunrise](https://github.com/stanuwu/Sunrise), the offline
Destiny 2 server emulator. Head tracking, motion controllers, and the weapon held where your hand
actually is, on the Season of Arrivals build from 2020.

The image is monoscopic. One view is rendered and the same frame goes to both eyes, so there is no
stereo depth. That was deliberate, and the reasoning is in [Scope](#scope).

> ### Status: proof of concept. Not maintained.
>
> Written over two days, 8 and 9 September 2026. Tested on **exactly one headset (Meta Quest 3
> over Link) and one Windows 11 machine**. It has never run anywhere else. Nothing here is a
> supported install, and nobody is working on it. Fork it if you want it to go further.

## What works

- **6DOF head tracking.** Position and rotation, at the FOV the frame was actually rendered with.
- **Motion controllers as input.** The OpenXR action set is mapped onto keyboard scancodes and
  relative mouse motion, because the game reads pads over HID and holds no XInput symbol at all.
- **The weapon on the controller.** The viewmodel is decoupled from the camera and placed where
  the physical controller is, pivoting about the gun rather than about your eye. The pivot
  constant was measured by a least squares solve, not guessed.
- **Turning the head turns the character.** The horizon follows the neck immediately. The
  character is driven round to match a moment later by a servo whose gain is learned at runtime.
- **Walking goes where you are looking**, including while the character is still catching up.

## What does not work

- **No stereo.** The world reads as flat and infinitely far. This is the single biggest limitation
  and it is by design, not a bug.
- **No enemies, NPCs, quests or saves.** That is Project Sunrise's scope, not this mod's.
- **The camera lags the headset by one frame.** The OpenXR frame runs on the present thread.
- **No roomscale.** Walking in your room moves the camera and not the character, so you can
  eventually push the view through a wall.
- **The loading screen renders black** while the module is writing the camera. Press F9 and the
  scene appears. It is not a hang.
- **Turning your head does nothing while the game window is unfocused.** Click the window before
  putting the headset on.

## What this changes in Sunrise

Almost nothing, and that was the point. Branch `vr` is upstream Sunrise at commit `7e3875d` with
60 files added and **48 lines added across 5 of their files**. The entire diff contains one single
deleted line, and it is a build property being extended with an include path.

Every added source lives in its own directory. Sunrise gains four call sites and one accessor it
did not previously expose. Remove those four calls and the build is upstream again.

**[See the whole diff in your browser](https://github.com/thecosmictangerine-cloud/destiny-2-vr/compare/7e3875d...vr)**, no clone needed. That is 65 files, and the 5 that
belong to Sunrise are marked in there among the rest.

[**docs/UPSTREAM-FOOTPRINT.md**](docs/UPSTREAM-FOOTPRINT.md) reproduces all 48 lines verbatim so
you can audit the whole integration surface in one sitting, along with the commands to reproduce
the diff yourself.

## Requirements

| | |
| --- | --- |
| Game | A working Project Sunrise **0.4.0** install. Follow [their instructions](https://github.com/stanuwu/Sunrise). This repository ships no game data and cannot help you obtain any. |
| Headset | Anything with an OpenXR runtime, in theory. In practice only a Meta Quest 3 over Link has ever been tried. |
| Build | Visual Studio 2022 BuildTools with the C++20 workload. |
| RAM | 16 GB is enough for four parallel compilers, barely. |

## Building

```powershell
sunrise-vr\scripts\build.ps1        # produces build\x64\Release\steam_api64.dll
sunrise-vr\scripts\deploy.ps1       # swaps it into <game>\bin\x64\
```

Both kill the running game first. If you call MSBuild yourself instead, two overrides are
mandatory:

- `/p:PlatformToolset=v143`, because the project file asks for v145 (Visual Studio 2026).
- `/p:PreferredToolArchitecture=x64`, because otherwise MSBuild picks the 32 bit compiler, which
  dies with `error C1060: out of compiler heap space` on this codebase's C++20 templates.

Cap parallelism at 4 on a 16 GB machine. A clean build takes 3 to 4 minutes.

Note that `deploy.ps1 -Restore` puts back the official Sunrise **0.3.2** DLL, which cannot read a
0.4.0 install. To get a real control build, compile upstream `7e3875d` instead.

## Running

Two settings in the game's `Sunrise\settings.json` matter:

- `client.region_private` must be `true`, or patrol zones load black at random while they wait for
  a matchmaking host that never arrives.
- `core.logging.file_sink` must be `true` if you want the structured log, which is off by default.

Then launch `destiny2.exe` **directly**, never through Steam. OpenXR comes up lazily on the first
present after you press F9, so the runtime does not start until you ask for it.

If your active OpenXR runtime is not the one you want, set `XR_RUNTIME_JSON`. The module reads that
first and falls back to `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`.

### Hotkeys

Live whenever the game has focus and the mod's own UI is closed.

| Key | Effect |
| --- | --- |
| F9 | Toggle VR. Also what brings OpenXR up the first time. |
| F7 | Recentre. Takes your current head position and yaw as the origin. |
| F8 | Reset the toggle and all trim. |
| F10 | Add 45 degrees of yaw trim, wrapping. |
| F11 | Add one unit of height, wrapping at 8. |

## Repository layout

This remote holds **two unrelated histories**, because the mod has to keep merging from upstream:

| Branch | Contents |
| --- | --- |
| `main` | This branch. Documentation and research. No mod code. |
| `vr` | The mod. A fork of stanuwu/Sunrise with its full history and its `upstream` remote intact, so `git pull upstream` still merges cleanly. All VR code, the mock OpenXR runtime and the build and test scripts are here. |

Keeping documentation outside the fork is deliberate. Nothing written here can ever collide with
an upstream file.

## Documentation

- [CLAUDE.md](CLAUDE.md) is the working context file: layout, the build and test loop, and every
  gotcha collected along the way. Start here.
- [docs/UPSTREAM-FOOTPRINT.md](docs/UPSTREAM-FOOTPRINT.md) is the complete diff against Sunrise.
- [docs/RESEARCH.md](docs/RESEARCH.md) covers engine internals: the camera pose block, the two
  pose consumer paths, the executable's anti-tamper layer and what VMProtect forbids. Read it
  before touching any offset.
- [docs/WEAPON-6DOF.md](docs/WEAPON-6DOF.md) explains the stateless weapon placement and why the
  obvious approach (capturing a rest reference) is wrong.
- [docs/BODY-SERVO-HANG.md](docs/BODY-SERVO-HANG.md) is a post mortem on a servo that ran away and
  froze the game one second after every destination finished loading.
- [docs/HANDOFF.md](docs/HANDOFF.md) and [docs/ZONES.md](docs/ZONES.md) are working notes, frozen
  where they stood.
- `docs/evidence/` holds the logs backing the claims above.

## Scope

Monoscopic was a deliberate trade. Head tracking in 6DOF carries most of the sense of presence,
stereo disparity matters mainly within a few metres, and these are large open spaces. Rendering
one view instead of two also halves the cost on a game that was never built for it.

The phases the work was organised into: **F0.a** build from source, **F0.b** prove the camera pose
can be written, **F1** OpenXR and 6DOF head tracking, **F2** controllers as input, **F3** weapon
6DOF on the controller. All five reached a working state. F3 was validated in the headset once.

## How this was built

Every line of the mod, the test harness and the documentation was written by Claude, Anthropic's
coding agent, driven by prompts. The reverse engineering went the same way: the agent launched the
game, read process memory, wrote probe DLLs, and read back logs and screenshots to check its own
work. There was no human code review.

The one thing it could not do was wear the headset. Latency, comfort and perceived scale were
checked by a person, and the documents record those sessions as such.

Two side effects a reader will notice. The comments are unusually dense, because the agent was
writing notes to its future self. And roughly as much code went into verification (5,164 lines of
harness and a mock OpenXR runtime) as into the mod itself (5,243 lines), which is what happens
when the target is a packed binary with no symbols that kills itself if you attach a debugger.

## What is not here, and why

- **No game data and no Bungie code.** Not one byte, in any commit, in either branch's history.
- **No screenshots.** They are frames of the running game and fall under the same rule. The local
  evidence directory keeps them and git ignores them.
- **No third party binaries.** The Sunrise installer and the official 0.3.2 DLL stay on the
  development machine for comparison and rollback.

## License

Branch `vr` contains Project Sunrise, which is **GPL-3.0**, and the VR module compiles into their
DLL and includes their headers. It is a derivative work and it is GPL-3.0 in its entirety. The
upstream `LICENSE` is intact and the modifications are marked, which is what the license asks for.

Two directories on that branch have no dependency on Sunrise at all and are the original work of
this project:

- `mockxr/`, a standalone mock OpenXR runtime used to test the whole lifecycle without a headset.
- `scripts/`, the build, deploy and measurement harness.

Those two are additionally offered under the **MIT** license, so they can be lifted into other
projects without pulling GPL obligations along. See `mockxr/LICENSE`.

`Sunrise/vendor/openxr/include/` holds the Khronos OpenXR headers, vendored unmodified under
Apache-2.0.

The documentation on this `main` branch is **CC BY 4.0**.
