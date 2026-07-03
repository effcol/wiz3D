# lib/d3d8/ — vendored DirectX 8 headers + import libs

DirectX 8 / Direct3D 8 vendored bundle: headers, x86/x64 import libraries
(`d3d8.lib`), and the `d3d8.def` module-definition file. Origin is the
DirectX SDK 8.1 era — pre-DXSDK-June-2010, predates the Win8/Win10 SDK
era where DX8 was removed entirely.

## Layout

```
lib/d3d8/
├── Include/
│   ├── d3d8.h
│   ├── d3d8caps.h
│   └── d3d8types.h
├── lib/
│   ├── x86/d3d8.lib
│   └── x64/d3d8.lib
└── d3d8.def
```

The .lib files are necessary to be able to build, and were manually
regenerated from the d3d8.def.

## Why the SDK has nothing to substitute

Microsoft fully removed Direct3D 8 from the Windows SDK. Confirmed:

```
find "C:\Program Files (x86)\Windows Kits\10\Include" -name "d3d8*"
# (no results)
```

There is no `d3d8.h`, no `d3d8.lib`, no DX8 anything in the Win10 (or
Win11) SDK. Unlike DX7 where the SDK header exists but is gated against
DX9 (see `lib/d3d7/README.md`), DX8 simply isn't there.

## How D3D8 is still reachable in the build

Two complementary paths, both used:

1. **vcpkg `directxsdk` port** — `vcpkg.json` declares `"directxsdk"`,
   which carries DX8/9/10/11 headers + import libs from the June 2010
   DXSDK snapshot. `Directory.Build.props` adds
   `vcpkg_installed\<triplet>\include\directxsdk` to every project's
   include path. `S3DWrapper8` resolves `d3d8.h` and links against
   vcpkg's `d3d8.lib` through this.
2. **This vendored bundle (`lib/d3d8/`)** — explicit, version-stable.
   `TestsPack/D3D8ResourceLeak/D3D8ResourceLeak.vcxproj` adds
   `$(SolutionDir)lib\d3d8\Include\` and
   `$(SolutionDir)lib\d3d8\lib\$(Platform)\` directly to its include and
   lib paths. Doesn't depend on vcpkg state being healthy.

Belt + suspenders. The vendored bundle is the **stable reference**; vcpkg
is the **convenient consumer-side resolver**.

## Why this lives under `lib/`, not `ThirdPartyLibs/`

It was briefly relocated to `ThirdPartyLibs/D3D8_v8.1/` during an earlier
audit pass; the user reversed that on 2026-05-27. Rationale: this is a
standalone object from the early iZ3D tree with no current upstream
counterpart. `ThirdPartyLibs/` is reserved for libraries that are
genuinely third-party and have a live upstream (TinyXML, LlamaXML).
DirectX 8 from 2001 is neither — it's a frozen MS-origin extract that
matches the same shape as `lib/d3d7/` and `lib/d3d10/`. Keep all three
under `lib/`.

This also drops the version-suffix convention (`_v8.1`) that
`ThirdPartyLibs/` uses, in favor of the flat `lib/d3d8/` naming consistent
with `lib/d3d7/` and `lib/d3d10/`.

## Consumer footprint

| Consumer | What it pulls |
|---|---|
| `TestsPack/D3D8ResourceLeak/D3D8ResourceLeak.vcxproj` | `Include/` for `d3d8.h`, `lib/x86/` or `lib/x64/` for `d3d8.lib` (8 vcxproj references — 4 configs × Include + LibDir) |
| `lib/d3d10/Include/d3dhal.h` (transitively) | `#include "d3d8.h"` — but `d3dhal.h` itself isn't included anywhere in the tree today, so this is latent, not active |
| `S3DWrapper8` | Resolves `d3d8.h` and `d3d8.lib` through vcpkg `directxsdk`, not this folder. |

## Maintenance posture

- **Don't delete** — `TestsPack/D3D8ResourceLeak` needs it directly; vcpkg
  directxsdk is the only other source on the machine and is fragile to
  vcpkg state.
- **Don't move back to `ThirdPartyLibs/`** — explicit user reversal.
- **Don't strip the consumer references** in the TestsPack vcxproj — the
  explicit path is intentional belt-and-suspenders against vcpkg
  resolution.
- **Don't refresh from a current upstream** — there isn't one. The SDK 8.1
  snapshot is the canonical source; nothing replaces it.
