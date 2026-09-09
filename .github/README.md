# Project Sunrise, with a VR mod on branch `vr`

You are looking at a fork of [stanuwu/Sunrise](https://github.com/stanuwu/Sunrise), the offline
Destiny 2 server emulator. Everything here is theirs except a monoscopic 6DOF VR mod, which is
what this fork exists for. Head tracking, motion controllers, and the weapon held where your hand
actually is.

**GitHub is showing you this file instead of Sunrise's own README, which is still there at
[`README.md`](../README.md) and is the one to read for what Sunrise is.**

> ### Status: proof of concept. Not maintained.
>
> Written over two days in September 2026 and tested on exactly one headset (Meta Quest 3 over
> Link) and one Windows 11 machine. Fork it if you want it to go further.

## See exactly what was added, in one click

**[Compare `7e3875d` against `vr`](https://github.com/thecosmictangerine-cloud/destiny-2-vr/compare/7e3875d...vr)**

`7e3875d` is the upstream Sunrise commit this branch is built on. That comparison is the whole
truth about this fork: 65 files, of which **5 belong to Sunrise and take 48 added lines between
them**. The entire diff contains one single deleted line, and it is a build property being
extended with an include path.

| | Files | Lines added |
| --- | --- | --- |
| Sunrise files modified | 5 | 48 |
| The mod, `Sunrise/src/client/hooks/vr/` | 12 | 5,243 |
| Khronos OpenXR headers, vendored unmodified | 3 | 7,125 |
| Test harness, `scripts/` and `mockxr/` | 43 | 5,164 |
| READMEs added by this fork, including this one | 2 | 119 |

Or from a clone:

```
git remote add upstream https://github.com/stanuwu/Sunrise.git
git fetch upstream
git diff upstream/master vr --stat
```

## The four call sites

Sunrise gains one accessor it did not previously expose, and three calls hung off two hooks it
already had. Remove them and the build is upstream again.

| Sunrise file | What was added |
| --- | --- |
| `hooks/teleport/teleport_lifecycle.cpp` | `vr::poll_keys()` and `vr::apply_camera()` after the existing `capture_camera_pose()` |
| `hooks/graphics/renderer/graphics_renderer_lifecycle.cpp` | `vr::present_frame()` at the end of `present()` |
| `hooks/teleport/runtime.h` and `teleport_move.cpp` | `camera_block()`, returning the camera pose block base |
| `Sunrise.vcxproj` | Registration of the 12 new sources and the OpenXR include path |

All 48 lines are reproduced verbatim in
[docs/UPSTREAM-FOOTPRINT.md](https://github.com/thecosmictangerine-cloud/destiny-2-vr/blob/main/docs/UPSTREAM-FOOTPRINT.md).

## Where things are

| Path | What |
| --- | --- |
| [`Sunrise/src/client/hooks/vr/`](../Sunrise/src/client/hooks/vr/) | The mod. Has its own README mapping the 12 files. |
| [`mockxr/`](../mockxr/) | A fake OpenXR runtime that exercises the whole lifecycle with no headset. |
| [`scripts/`](../scripts/) | Build, deploy, and the measurement harness the mod was verified with. |
| Branch `main` | All project documentation, including the research on the engine and the executable's anti-tamper layer. |

Start with the [documentation branch](https://github.com/thecosmictangerine-cloud/destiny-2-vr/tree/main),
not with the code. It explains how to build and run this, and why the mod is shaped the way it is.

## License

Sunrise is **GPL-3.0** and so is this fork. The VR module includes Sunrise headers and compiles
into their DLL, so it is a derivative work with no ambiguity. Upstream's `LICENSE` is intact and
the modifications are marked.

Two directories have no dependency on Sunrise at all and are additionally offered under **MIT**,
so they can be reused elsewhere: `mockxr/` and `scripts/`. See [`mockxr/LICENSE`](../mockxr/LICENSE).

`Sunrise/vendor/openxr/include/` holds the Khronos OpenXR headers, vendored unmodified under
Apache-2.0.

## How this was built

Every line of the mod, the harness and the documentation was written by Claude, Anthropic's coding
agent, driven by prompts. The reverse engineering went the same way: the agent launched the game,
read process memory, wrote probe DLLs, and read back logs and screenshots to check its own work.
There was no human code review. The one thing it could not do was wear the headset, so latency,
comfort and scale were checked by a person.
