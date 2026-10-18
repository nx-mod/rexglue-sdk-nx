# Switch SDK lift — tracker

Lifting the nfsmw-nx Switch/deko/`x360` layer onto the fork's clean v0.10.0,
**keeping Vulkan/D3D12** (Strategy 2, universal). Base confirmed = v0.10.0.

Surface vs v0.10.0: **71 new files + 86 modified**. The Vulkan removal in our
source tree was commit `c38edd3` (pure deletions + CMake); the fork still has
every Vulkan file, so we add the Switch layer *on top* rather than restore
anything. `c38edd3^` had deko + Vulkan side-by-side — the reference for CMake merges.

| Batch | What | Files | State |
|---|---|---|---|
| 1 | Pure additions: `x360/`, `ui/deko3d/`, `*/switch/`, `switch_compat/`, FFmpeg-switch, `rexglue_switch.cmake` | 71 | in progress |
| 2 | SW-only base edits (platform layer: threading/fiber/xthread/xmemory/dynlib/host_path/clock) | 39 | todo |
| 3 | OTHER base edits (universal fixes; reconcile codegen with the 7 upstream cherry-picks) | 38 | todo |
| 4 | MIXED CMake (graphics/ui/thirdparty/core/system + presenter/surface): keep Vulkan, add deko | 9 | todo |

Build/test only possible after batch 4 wires CMake. Englishization is a later pass.
