# Nintendo Switch port layout

Mirrors the KongaTime PS5 port's convention: the SDK under `src/` and
`include/rex/` stays **universal** (game- and console-neutral), and everything
Switch- or game-specific lives in its own folder.

| Path | What | Universal? |
|---|---|---|
| `src/`, `include/rex/` | ReXGlue SDK + Switch platform backend (`*_horizon.cpp`, `REX_PLATFORM_SWITCH`) | yes |
| `src/x360/deko/`, `include/rex/x360/` | deko3d renderer + `rex::x360` GPU layer | yes |
| `switch/` | this port's docs, tests, provenance | n/a |
| `games/<title>/` | per-game shaders, geometry quirks, `RenderHooks`, manifests — consumes the SDK | **no** |

Rule: a fix that helps every game goes in the SDK; anything that names a
specific title goes under `games/`. That is what lets NFS gain from Halo's
work without the two sharing code.
