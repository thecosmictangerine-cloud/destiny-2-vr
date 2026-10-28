# Destiny 2 VR: Project Sunrise with a VR mod

You are looking at branch `vr` of [Destiny 2 VR](https://github.com/thecosmictangerine-cloud/destiny-2-vr), a fork
of [stanuwu/Sunrise](https://github.com/stanuwu/Sunrise), the offline Destiny 2 preservation project. Everything
here is theirs except the VR mod, which is what this fork exists for.

**To play, download the zip from the [Releases](https://github.com/thecosmictangerine-cloud/destiny-2-vr/releases)
page** and read the [project page](https://github.com/thecosmictangerine-cloud/destiny-2-vr) first. Never put the
DLL into your normal Steam copy of Destiny 2 (online, anti-cheat, ban risk).

GitHub is showing you this file instead of Sunrise's own README, which is still there at
[`README.md`](../README.md) and is the one to read for what Sunrise is.

## What the fork changes in Sunrise

Measured against Sunrise **0.5.1**, which this branch merges: **9 Sunrise files, 133 lines added and 1 removed.**
Everything else the fork adds lives in directories of its own:

| Path | What |
| --- | --- |
| [`Sunrise/src/client/hooks/vr/`](../Sunrise/src/client/hooks/vr/) | The VR module. |
| `Sunrise/vendor/openxr/include/` | The Khronos OpenXR headers, vendored unmodified. |
| [`mockxr/`](../mockxr/) | A mock OpenXR runtime that runs the whole mod with no headset. |
| [`scripts/`](../scripts/) | Build, deploy, release packing, and the test batteries built on the mock. |
| [`package/`](../package/) | What goes into the player's zip besides the DLL: installer, settings, README. |

The Sunrise files gain call sites into the VR module (the camera hook, the present, the world step), one accessor
for the camera pose block, a guard in the detour transaction, the source registration in `Sunrise.vcxproj`, and in
`dllmain.cpp` a check that closes the game unless it is the Sunrise build of `destiny2.exe`
(`vr_game_check.h`). The one removed line is the include path property, extended with `vendor\openxr\include`.

From a clone:

```
git remote add upstream https://github.com/stanuwu/Sunrise.git
git fetch upstream --tags
git diff 0.5.1 vr --stat -- . ':!Sunrise/src/client/hooks/vr' ':!scripts' ':!mockxr' ':!Sunrise/vendor/openxr' ':!package' ':!.github'
```

## Building and releasing

```powershell
scripts\build.ps1                          # build\x64\Release\steam_api64.dll (VS 2022 Build Tools, C++20)
scripts\deploy.ps1                         # into the test install
scripts\Make-Release.ps1 -Version 0.1.0    # build\dist\Destiny2VR-0.1.0.zip
```

`build.ps1` passes `PlatformToolset=v143` (the project asks for VS 2026's v145) and `PreferredToolArchitecture=x64`
(the 32-bit compiler runs out of heap on this codebase), and caps the compilers at 4 for 16 GB of RAM.
`Test-Lifecycle.ps1` in `scripts/testing/` boots the game on the mock with VR on and lives through the runtime's
failures (headset off, lost link, stalls).

## License

Sunrise is **GPL-3.0** and so is this fork: the VR module includes Sunrise headers and compiles into their DLL.
Upstream's `LICENSE` is intact. `mockxr/` and `scripts/` have no dependency on Sunrise and are additionally offered
under **MIT** (see [`mockxr/LICENSE`](../mockxr/LICENSE)). The OpenXR headers are Khronos', Apache-2.0 OR MIT.

## How this was built

The mod, the harness and the documentation were written by Claude, Anthropic's coding agent, driven by prompts; the
reverse engineering went the same way. What needs a headset (latency, comfort, scale) was checked by a person.
