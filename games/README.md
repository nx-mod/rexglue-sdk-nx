# Games

Each title is an SDK **consumer**: it links the universal ReXGlue + deko3d SDK
(via `add_subdirectory`) and supplies only its own game-specific code —
shader library, vertex/geometry quirks, `RenderHooks`, manifests. No universal
code lives here; no game code lives in the SDK.

| Folder | Title | State |
|---|---|---|
| `halo3/` | Halo 3 (Xbox 360) | boots to menu with audio; no geometry yet |
| `nfsmw/` | Need for Speed: Most Wanted (PAL) | released build runs; SDK-compile verified |

(Game trees are migrated from `xbox360-nx` in a following step.)
