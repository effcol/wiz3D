# lib/d3d7/ — vendored DirectX 7 headers

Old MS DirectX 7 / DirectDraw 7 headers, kept here because the Windows 10
SDK's modern `d3d.h` cannot coexist with `d3d9.h` in the same translation
unit, and `S3DWrapper7` needs both.

Only consumer: `S3DWrapper7/S3DWrapperD3D7.vcxproj`, which adds
`..\lib\d3d7\include;` to every configuration's
`AdditionalIncludeDirectories`. Six configurations
(Debug/Release/Final Release × Win32/x64) all reference it.

## Why the SDK copy doesn't work

Win10 SDK ships `um/d3d.h` with this guard at the top of the body:

```c
#ifndef DIRECT3D_VERSION
#define DIRECT3D_VERSION         0x0700
#endif

// include this file content only if compiling for <=DX7 interfaces
#if(DIRECT3D_VERSION < 0x0800)
... DECLARE_INTERFACE_(IDirect3D7, ...) ...
... DECLARE_INTERFACE_(IDirectDraw7, ...) ...
#endif
```

Win10 SDK `shared/d3d9.h` opens with:

```c
#ifndef DIRECT3D_VERSION
#define DIRECT3D_VERSION         0x0900
#endif
```

So when `StdAfx.h` does `#include <d3d9.h>` before `#include <d3d.h>`
(it must — the wrapper handles both DX7 and DX9 in the same TU), `d3d9.h`
sticks `DIRECT3D_VERSION = 0x0900`, and the subsequent `d3d.h` body is
gated out entirely. `IDirect3D7`, `IDirectDraw7`, etc. never get declared,
and `Direct3D7.h` fails to compile with `'IDirect3D7': base class
undefined`.

Microsoft deliberately made `d3d.h` and `d3d9.h` mutually exclusive in the
SDK starting around the Win8 SDK era. The included reordering, `#undef`,
and `DIRECT3D_VERSION` preset workarounds all bottom out on the same
mutual-exclusion design.

The vendored copy in this folder predates that gate. It declares the DX7
interfaces unconditionally and happily compiles alongside `<d3d9.h>`.

## Why not move to ThirdPartyLibs/

The `ThirdPartyLibs/` layout is for `<Name>_v<Version>/` vendor drops with
their own libs/binaries. These are just headers, the project pattern for
WDK/SDK-extract bundles is to keep them under `lib/` (mirrors
`lib/d3d10/`), and the long-term direction is toward `lib/`-only
vendoring for MS-origin bundles. Don't promote this to ThirdPartyLibs.

## What's actually used vs. cargo

| Header | Used by S3DWrapper7? |
|---|---|
| `d3d.h` | yes — `StdAfx.h`, `Direct3D7.h` |
| `d3dcaps.h` | transitively via `d3d.h` |
| `d3dtypes.h` | transitively via `d3d.h` |
| `d3dvec.inl` | transitively via `d3dtypes.h` |
| `d3drm.h` | **unused** — Direct3D Retained Mode, deprecated |
| `d3drmdef.h` | **unused** — Retained Mode |
| `d3drmobj.h` | **unused** — Retained Mode |
| `dxtrans.h` | **unused** — DirectShow Transform Filters |

The 4 Retained Mode / DirectShow headers (~273 KB) are pure cargo from
the original DXSDK extract. They can be deleted as a follow-up — verify
with grep, then `git rm`. Not done in this pass to keep the restore
minimal.

## Maintenance posture

- **Don't delete the folder** — the SDK `d3d.h` doesn't substitute (see above).
- **Don't strip the include-path entry** from `S3DWrapperD3D7.vcxproj`.
- **Don't move to `ThirdPartyLibs/`**.
- **Do prune** the 4 confirmed-unused Retained Mode / DirectShow headers
  if you want — they're cargo, not load-bearing.
