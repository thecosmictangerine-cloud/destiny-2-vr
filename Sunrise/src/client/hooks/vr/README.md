# The VR module

Everything the VR mod adds to Sunrise lives here. 12 source files, 5,243 lines, and no Sunrise
file outside this directory changes by more than a few lines. See
[the fork's README](../../../../../.github/README.md) for the four call sites that reach in here.

The module is monoscopic on purpose. One view is rendered and the same frame goes to both eyes,
so there is no stereo depth. Head tracking in 6DOF carries the presence instead.

| File | Lines | What it does |
| --- | --- | --- |
| `runtime.h` | 98 | The entire surface Sunrise sees: `poll_keys()`, `apply_camera()`, `present_frame()`. Nothing else in the module is reachable from outside. |
| `vr_camera.cpp` | 791 | The camera. Reads the pose the engine just produced, lays the head pose over it, writes it back, and re-reads the block to prove the store landed. Owns the **room anchor**, the world yaw that the direction the player faced at the last recentre maps to, which is what stops anything but an explicit turn from moving the horizon. Also the **body servo** that drives the character round to face where the player is looking. |
| `xr_runtime.cpp` | 2146 | OpenXR, and the largest file here for two reasons. It carries **its own mini loader**, because the only loader build on the development machine was compiled against the wrong CRT, so the module reads the active runtime's manifest, loads the library and negotiates the interface itself. And it defeats the executable's **anti-injection layer** before doing so. It also copies the presented back buffer into the runtime's swapchain and submits one projection layer. |
| `xr_runtime.h` | 212 | The pose and input structures the rest of the module reads. |
| `vr_weapon.cpp` | 862 | The weapon on the controller. Detours three engine functions, including the weapon's own transform builder, which is what makes 6DOF possible: the pose the weapon is handed carries only an orientation, while the transform builder's output holds the world space offset it is placed by. The placement is **stateless**, one equation evaluated from a single frame's data. |
| `vr_weapon.h` | 49 | |
| `vr_gamepad.cpp` | 309 | Controllers as keyboard and mouse. The game reads pads over HID and the executable holds no XInput symbol at all, so an XInput detour would feed nothing. The action set is mapped onto `SendInput` scancodes instead. Two departures from a plain pad mapping: the right stick turns the room anchor by an exact angle rather than injecting mouse counts, and locomotion is rotated by how far the character still lags the gaze, so forward means forward while the servo catches up. |
| `vr_gamepad.h` | 52 | |
| `vr_destination.cpp` | 156 | Reads a destination from a file in orbit and forces it through Sunrise's own activity override, so a test run lands in the same patrol zone every time. A testing convenience, not part of the VR pipeline. |
| `vr_destination.h` | 24 | |
| `vr_watch.cpp` | 529 | A hardware breakpoint watcher: DR0 on every thread plus a vectored handler, used once to find which code sites touch the camera pose block. **Kept only as a last resort.** The executable is VMProtect packed and every such watch kills the game within minutes, always at the same address. One watch buys one relaunch. |
| `vr_watch.h` | 15 | |

## Reading order

`runtime.h` first, for the three entry points. Then `vr_camera.cpp`, which is where the room
anchor and the servo live and where the design decisions are visible. `xr_runtime.cpp` is worth
reading only if you care about loading an OpenXR runtime into a process that refuses foreign DLLs.

The reasoning behind all of it is on the repository's `main` branch, not here. `docs/RESEARCH.md`
covers the engine internals and the anti-tamper findings, and `docs/WEAPON-6DOF.md` explains why
the obvious approach to placing the weapon is wrong.
