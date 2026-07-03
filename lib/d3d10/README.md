# lib/d3d10/ — UMD DDI header bundle (misnamed)

Despite the folder name, this is **not** a D3D10 SDK header drop. It is the
**User-Mode Driver DDI (UMD DDI) bundle** that the wrapper projects need to
hook the interface between the OS DirectX runtime and an IHV graphics
driver. The bundle covers D3D9, D3D10, D3D11, and DXGI driver-side types
together; the `d3d10` name is historical.

These headers normally ship in the **Windows Driver Kit (WDK)**, not the
regular Windows 10 SDK, so they have to be vendored here. The bundle has
been triaged: obsolete SDK headers live in `_obsolete_sdk_headers/`, and
two unused files carry the `.legacy_unused` suffix
(`dxgiformat.h.legacy_unused`, `dxgitype.h.legacy_unused`).

## Why the wrappers need DDI headers (not API headers)

iZ3D's original architecture hooked the **UMD-DDI layer** to inject
stereo rendering. That was paired with kernel-mode driver tooling for
process injection (since abandoned — see the project's injection
architecture notes). The proxy-DLL approach used today still consumes the
UMD-DDI types (`D3D10DDIARG_*`, `D3DDDIARG_*`, `DXGI_DDI_*`, etc.) to
define the wrapper signatures, so this bundle is load-bearing even though
the kernel-mode driver path was dropped.

## What's globally included

Projects that use the D3D10 UMD DDI headers add `lib\d3d10\include\` to their
own `AdditionalIncludeDirectories` (the S3DWrapper* and OutputMethods projects).
Removing the folder breaks ~30 source files.

## Header → consumer map (high-traffic only)

| Header | Used by |
|---|---|
| `d3d10umddi.h` | `S3DWrapper10/*` (8 files including `Streamer/`), `DX10SharedLibrary`, `DX10GenerateCodeFromDump`, `UILIB/{DX10Texture,DX10Sprite,DX10Device}`, `CommonUtils/ShaderProcessor`, `ShaderAnalysis/*` (5 files), `OutputMethods/OutputLib/PostprocessedOutput_dx10.h` |
| `d3dumddi.h` | `S3DWrapper9/{UMDShutterPresenter,UMDriverHook,UMDDeviceWrapper,EnumToString}` |
| `d3d9types.h` | `S3DWrapper9/EnumToString`, `S3DWrapper10/*` (DXGI_RGBA shim — needs `D3DCOLORVALUE`) |
| `dxgiddi.h` | `S3DWrapper10/EnumToString.h` |
| `dxgitype.h` | `S3DWrapper9/PresenterHelpers.h` |

`d3dkmddi.h`, `d3dkmdt.h`, `d3dkmthk.h`, `d3dukmdt.h`, `d3dnthal.h`,
`d3dhal.h`, `d3dhalex.h`, `dxmini.h`, `wmidata.h`, `dmmdiag.h`, `d3dtypes.h`,
`d3dvec.inl`, `d3d10tokenizedprogramformat.hpp`,
`d3d11TokenizedProgramFormat.hpp` are pulled in transitively or are
referenced from a smaller number of sites.

## Why this stays under `lib/` and is not moved to `ThirdPartyLibs/`

The `ThirdPartyLibs/` convention is for libraries that come from a
distinct upstream vendor with a live upstream and follow a
`<Name>_v<Version>/` drop layout (e.g. `ThirdPartyLibs/MinHook_v1.3.4/`,
`ThirdPartyLibs/zlib_v1.3.2/`). This bundle is a curated extract of
Microsoft WDK headers with no upstream — same shape as `lib/d3d7/` and
`lib/d3d8/`, which also stay under `lib/`. Treating it as "third-party"
would over-promote it and trigger pointless migration churn for no
functional gain.

Leave it under `lib/` as project-internal vendored infrastructure.

## What's *not* in here that you might expect

- The Win10 SDK ships `dxgiddi.h` in `um/`, so this folder's copy is
  redundant for that one header — but the rest of the chain it pulls in
  (`d3d10umddi.h`, `d3dumddi.h`, ...) is unique to this bundle, so the
  whole thing stays.
- Nothing in here is consumed by `S3DWrapper7` or `S3DWrapper8` — those
  use SDK/vcpkg headers. UMD-DDI hooking only matters for the D3D9/10/11
  era wrappers.

## Maintenance posture

- **Don't delete** — load-bearing for ~30 source files.
- **Don't migrate to `ThirdPartyLibs/`** — it's not a vendor drop.
- **Don't add fresh headers from a current WDK** unless a specific wrapper
  needs a newer interface; the bundle is intentionally frozen to the era
  the wrappers were written against.
- **Do prune** anything that grep confirms is unreferenced. The
  `_obsolete_sdk_headers/` quarantine and the `.legacy_unused` suffixes
  are the existing pattern; follow it.
