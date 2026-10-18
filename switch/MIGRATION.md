# Switch SDK lift — tracker

The nfsmw-nx Switch/deko/`rex::x360` layer has been lifted onto the fork's clean
v0.10.0, **keeping Vulkan/D3D12** (universal). Base = v0.10.0 (confirmed).

| Batch | What | Files | State |
|---|---|---|---|
| 1 | Additions: `x360/`, `ui/deko3d/`, `*/switch/`, `switch_compat/`, FFmpeg-switch, `rexglue_switch.cmake` | 70 | ✅ done |
| 2+3 | Switch platform-layer + base edits; merged with the 7 upstream cherry-picks | 73 | ✅ done |
| 4 | MIXED CMake: deko wired **alongside** Vulkan/D3D12, selected per platform | 9 | ✅ done |

Also: `platform.h` detects `__SWITCH__` (→ `REX_PLATFORM_SWITCH`, treated as
`LINUX` to reuse the `_posix` layer); 4 codegen + 3 build fixes cherry-picked
from upstream `development` with `-x` provenance.

Consumed by `xbox360-nx` as a submodule at `sdk/` (games build with `-DREXSDK_DIR=$PWD/sdk`).

### Remaining
- **Build validation** (Switch, devkitA64): populate `thirdparty/` (init submodules or
  `fetch_thirdparty.py`), then a `-j2` build — the real test of the CMake merge.
- `tools/switch_setup.py` makes a fresh clone self-sufficient (submodules + deko3d deps + FFmpeg overlay).
- Host-only `src/codegen` and `src/rexglue` (the CLI) are not built on Switch.
- Englishize the Spanish deko/`x360` identifiers and comments (one pass).
- Migrate the game trees into `games/` (currently still in `xbox360-nx`).
