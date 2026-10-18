# What the Switch port adapts, and from where

The platform-backend method follows the PS5 port (`ps5/PORTED.md`): a clean
`*_horizon` backend behind `include/rex/platform.h` (`REX_PLATFORM_SWITCH`),
English, with each adapted file crediting its source. Rendering is **deko3d
only** — never Vulkan.

| Area | Adapted from | License | Notes |
|---|---|---|---|
| Platform seam (`platform.h`, memory, startup, threading, dynlib, fault) | KongaTime PS5 port (`*_prospero`) | BSD-3 | PS5 is FreeBSD-like; Switch is newlib/Horizon, aarch64. Horizon cannot resume from a fault handler, so no read-fault guard. |
| deko3d renderer, PM4/copy decode, `rex::x360` GPU layer | nfsmw-nx | GPL-3 (credited) | already runs Halo to the menu; being Englishized on the lift |
| Horizon / deko specifics (surface, present, threads on fixed cores) | UnleashedRecomp-NX | — | actual Switch + deko recomp reference |
| Codegen correctness (barriers, atomics, vector aliasing, reg types) | upstream ReXGlue `development` | BSD-3 | cherry-picked with `-x` provenance |
| SDK-as-subdirectory build | upstream ReXGlue `development` | BSD-3 | enables games under `games/` to consume the SDK |

Not yet taken: persistent cross-frame vertex cache; online/system-link
netstack (adapt from Xenia-canary netplay, XNet/XLLP) — after Halo draws.
