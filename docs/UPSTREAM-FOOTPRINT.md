# The complete footprint into Sunrise

Everything this mod changes in upstream [stanuwu/Sunrise](https://github.com/stanuwu/Sunrise),
reproduced verbatim so it can be audited in one sitting. Measured against upstream master
`7e3875d`, which is the commit branch `vr` is based on.

**[Open the comparison on GitHub](https://github.com/thecosmictangerine-cloud/destiny-2-vr/compare/7e3875d...vr)** to read the whole thing in your
browser with no clone at all. Or reproduce it locally:

```
git remote add upstream https://github.com/stanuwu/Sunrise.git
git fetch upstream
git diff upstream/master vr --stat
git diff upstream/master vr -- Sunrise/src/client/hooks/teleport
```

## The shape of it

| | Files | Lines |
| --- | --- | --- |
| Upstream, untouched | 1847 | — |
| Upstream, modified | **5** | **48 added, 1 removed** |
| Added by this mod | 60 | 17,651 |
| **Total on branch `vr`** | **1912** | |

The one removed line is the `AdditionalIncludeDirectories` property in `Sunrise.vcxproj`, replaced
by the same line with `vendor\openxr\include` inserted. Every other change in the entire diff is a
line addition. Nothing upstream is deleted, moved, renamed or rewritten.

The 60 added files break down by who wrote them and under what terms:

| Directory | Files | Lines | Provenance |
| --- | --- | --- | --- |
| `Sunrise/src/client/hooks/vr/` | 13 | 5,276 | This mod, 12 sources and a README. Derivative of Sunrise: it includes their headers and links into their DLL. GPL-3.0. |
| `Sunrise/vendor/openxr/include/` | 3 | 7,125 | **Khronos Group**, vendored unmodified. Apache-2.0. |
| `scripts/` | 39 | 4,488 | This mod. Build, deploy and test harness. No dependency on Sunrise. |
| `mockxr/` | 4 | 676 | This mod. A standalone mock OpenXR runtime. No dependency on Sunrise. |
| `.github/` | 1 | 86 | This fork explaining itself, rendered by GitHub in place of the upstream README. |

So of the 17,651 added lines, 7,125 are third-party headers, and 5,164 are test harness that never
touches Sunrise at all. The mod proper is 5,243 lines, of which roughly 40% is the OpenXR runtime
loader and frame loop in `xr_runtime.cpp`.

## The integration surface: four calls and one accessor

The mod does not restructure anything. It asks Sunrise for one thing it did not already expose,
and hangs three calls off two existing hooks:

| Site | What was added |
| --- | --- |
| `hooks/teleport/teleport_lifecycle.cpp` | `vr::poll_keys()` and `vr::apply_camera()` after the existing `capture_camera_pose()` |
| `hooks/graphics/renderer/graphics_renderer_lifecycle.cpp` | `vr::present_frame()` at the end of `present()` |
| `hooks/teleport/runtime.h` + `teleport_move.cpp` | `camera_block()`, a new accessor returning the pose block base |
| `Sunrise.vcxproj` | Registration of the 12 new sources and the OpenXR include path |

If the four call sites are removed, the build is upstream again. Nothing in the VR directory runs
on its own, and nothing upstream depends on it.

## The diffs, in full

### `Sunrise/src/client/hooks/teleport/runtime.h` (+9)

Declares the accessor. Sunrise already publishes the camera forward vector from this hook, so the
comment follows the wording of the declaration above it.

```diff
+/**
+ * Reports the base of one player's camera pose block.
+ * Exposed so a feature can write the pose the camera hook has just published: that hook is the
+ * only site that reaches the block at all.
+ * @param playerIndex Player whose block to address.
+ * @return The block base, or null while the camera singleton is unresolved.
+ */
+[[nodiscard]] std::byte* camera_block(std::uint32_t playerIndex) noexcept;
+
```

### `Sunrise/src/client/hooks/teleport/teleport_move.cpp` (+12)

Implements it, reusing the singleton and stride that `capture_camera_pose()` right below it
already uses.

```diff
+/** Reports the base of one player's camera pose block. */
+std::byte* camera_block(std::uint32_t playerIndex) noexcept {
+    if (playerIndex == kInvalidHandle || g_cameraSingleton == nullptr) {
+        return nullptr;
+    }
+    std::byte* const camera = g_cameraSingleton();
+    if (camera == nullptr) {
+        return nullptr;
+    }
+    return camera + kCameraBlockStride * playerIndex;
+}
+
```

### `Sunrise/src/client/hooks/teleport/teleport_lifecycle.cpp` (+6)

The camera write. Placement inside the function is the whole point: the engine's original runs
first, then Sunrise captures the pose it produced, and only then does the VR module overwrite it.
Sunrise's own consumers therefore keep reading the engine's pose.

```diff
 #include "../sword_skate/sword_skate.h"
+#include "../vr/runtime.h"

     const std::int64_t result = next != nullptr ? next(playerIndex) : 0;
     capture_camera_pose(playerIndex);
+    // The VR probe overwrites the pose the engine just produced. It runs after the capture on
+    // purpose, so what the teleport and the world lines read stays the engine's own pose, and
+    // before everything else in the frame, which is what should see the overwritten one.
+    hooks::vr::poll_keys();
+    hooks::vr::apply_camera(playerIndex);
     poll_request();
```

### `Sunrise/src/client/hooks/graphics/renderer/graphics_renderer_lifecycle.cpp` (+8)

The frame submission. It captures the device under the renderer lock and makes the call after the
lock is released, because entering the OpenXR runtime blocks on the compositor.

```diff
 #include "../../polled_input/runtime.h"
+#include "../../vr/runtime.h"

     bool framed = false;
+    ID3D11Device* vrDevice = nullptr;
     if (g_resources.swapChain == swapChain && fully_active_locked()) {
         render_frame_locked();
         framed = true;
+        vrDevice = g_resources.device;
     }
     ReleaseSRWLockExclusive(&g_rendererLock);

     (void)input::install_raw_input_window();
+    // The VR frame enters the OpenXR runtime, which blocks on the compositor, so it runs last and
+    // only once the renderer lock is gone.
+    if (vrDevice != nullptr) {
+        hooks::vr::present_frame(vrDevice, swapChain);
+    }
 }
```

### `Sunrise/Sunrise.vcxproj` (+13, -1)

Build registration only. The project lists all sources explicitly with no wildcards, so new files
have to be named by hand.

```diff
-<AdditionalIncludeDirectories>$(ProjectDir)src;...vendor\lua;$(ProjectDir)vendor\sqlite;...
+<AdditionalIncludeDirectories>$(ProjectDir)src;...vendor\lua;$(ProjectDir)vendor\openxr\include;$(ProjectDir)vendor\sqlite;...

+<ClCompile Include="src\client\hooks\vr\vr_camera.cpp" />
+<ClCompile Include="src\client\hooks\vr\vr_gamepad.cpp" />
+<ClCompile Include="src\client\hooks\vr\vr_destination.cpp" />
+<ClCompile Include="src\client\hooks\vr\vr_watch.cpp" />
+<ClCompile Include="src\client\hooks\vr\vr_weapon.cpp" />
+<ClCompile Include="src\client\hooks\vr\xr_runtime.cpp" />

+<ClInclude Include="src\client\hooks\vr\runtime.h" />
+<ClInclude Include="src\client\hooks\vr\vr_gamepad.h" />
+<ClInclude Include="src\client\hooks\vr\vr_destination.h" />
+<ClInclude Include="src\client\hooks\vr\vr_watch.h" />
+<ClInclude Include="src\client\hooks\vr\vr_weapon.h" />
+<ClInclude Include="src\client\hooks\vr\xr_runtime.h" />
```

## Why it was kept this small

So `git pull upstream` keeps merging cleanly. A mod that rewrote upstream files would conflict on
every pull and would have to be re-applied by hand against each new Sunrise release. Keeping the
footprint additive means the VR work and Sunrise's own development do not collide, and it also
means a reader can check in five minutes that this mod does not change how Sunrise itself behaves.
