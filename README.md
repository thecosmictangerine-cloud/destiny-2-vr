# Destiny 2 VR

A PC VR mod for [Project Sunrise](https://github.com/stanuwu/Sunrise), the offline Destiny 2 preservation
project: play the 2020 build of Destiny 2 in a headset with motion controllers. Free, open source, unofficial.

- **The view follows your head** in 6DOF, and turning your head turns your Guardian.
- **Your hands are your controllers**: the Guardian's arms reach for them, the gun sits in your hand and fires
  from its barrel where you point it.
- **Aim down sights by raising the gun to your eye**, and steady it with your other hand on the handguard.
- **Dual wield** any two weapons from your loadout, and draw them from holsters on your hips and shoulder.
- **Swords and supers in first person**, held in your hand, in one hand or both.
- **Ride the sparrow from the cockpit**: grab the handlebars to steer.
- **Menus, the map and loading screens on a big screen in front of you**, pointed at with the controllers.

> Status: **0.1.0, beta.** Developed and played on a Meta Quest 3 over Link / Air Link. Other OpenXR headsets
> should work but are less tested. The image is the same in both eyes (no stereo depth yet).

> [!WARNING]
> **Never put this mod into your normal Steam copy of Destiny 2.** That game is online and has anti-cheat, and a
> modified `steam_api64.dll` there can get your account banned. Destiny 2 VR is for a Project Sunrise install only.
> The installer and the mod itself both check the game build and refuse to run anywhere else.

## Install

1. Install **Project Sunrise** with the [Sunrise Launcher](https://projectsunrise.dev/guides/installing/) and check
   that it runs.
2. Download `Destiny2VR-<version>.zip` from the [Releases](../../releases) page and unzip it.
3. Double-click **`Install.bat`**. It finds the Sunrise folder (or asks for it), checks it, backs up Sunrise's own
   DLL and puts the VR one in its place.
4. Start your headset's PC link, then start `destiny2.exe` from the Sunrise folder.

After installing, the Sunrise Launcher shows **Repair** instead of **Play** ("Local files changed"), because
Sunrise's DLL was replaced: that is expected. Pressing **Repair** or **Update** puts the official DLL back (VR is gone until you run
`Install.bat` again). To uninstall, run `Uninstall.bat`.

The zip's `README.txt` has everything for players: the controls, the settings and troubleshooting.

### Requirements

| | |
|---|---|
| Game | A **Project Sunrise** install (the Launcher downloads the 2020 build; it needs a Steam account that owns Destiny 2). This release contains Sunrise **0.5.1**. |
| OS | Windows 10 or 11, 64-bit. |
| Headset | Any PC VR headset with an OpenXR runtime. Tested: Meta Quest 3 over Link / Air Link. |

## Not affiliated

This is not a Project Sunrise release, and it is not supported by the Sunrise team: report Destiny 2 VR problems in
this repository's [Issues](../../issues), not in Sunrise's. Attach the log (see the zip's README) and say what you
were doing.

Destiny 2 is a trademark of Bungie, Inc. This project is not affiliated with or endorsed by Bungie, and contains no
game files or game code.

## How it works

Sunrise loads into the game as its `steam_api64.dll`. Destiny 2 VR is a fork of Sunrise with the VR module compiled
into that same DLL, so the release is a single file. The fork is deliberately thin: everything VR lives in its own
directory, `Sunrise/src/client/hooks/vr/`, and Sunrise's own sources gain only a handful of call sites, so upstream
releases keep merging cleanly.

The module drives the game's camera from the headset, submits the rendered frame to OpenXR, turns the controllers into
the game's keyboard and mouse input, and re-poses the first-person arms and weapons on the GPU with inverse
kinematics so they reach the controllers.

## For developers

| Branch | Contents |
|---|---|
| `main` | This page, and the research notes from the first phase of the project under `docs/`. |
| `vr` | The mod: the Sunrise fork with its full history and `upstream` remote. Source of every release. |

On branch `vr`: `scripts/build.ps1` builds the DLL (Visual Studio 2022 Build Tools, C++20), `scripts/deploy.ps1`
puts it into a test install, `scripts/Make-Release.ps1` packs the zip. `mockxr/` is a mock OpenXR runtime that runs
the whole mod without a headset, with scripted head and controller motion, and `scripts/testing/` holds the
automated test batteries built on it.

The mod, its test harness and the documentation were written by Claude, Anthropic's coding agent, driven by prompts;
the reverse engineering went the same way. Everything that needs a headset (comfort, latency, feel) was checked by a
person.

## Support

If you enjoy the mod: [ko-fi.com/cosmictangerine](https://ko-fi.com/cosmictangerine) ·
[patreon.com/cw/CosmicTangerine](https://www.patreon.com/cw/CosmicTangerine)

## License

Branch `vr` contains Project Sunrise, which is **GPL-3.0**, and the VR module compiles into its DLL: the mod is GPL-3.0
as a whole, and every release names the commit it was built from. `mockxr/` and `scripts/` have no dependency on
Sunrise and are additionally offered under the MIT license. The OpenXR headers are Khronos', Apache-2.0 OR MIT. The
documentation on this branch is CC BY 4.0.

Made by **The Cosmic Tangerine**.
