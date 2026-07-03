# `lib/` — orphan vendor bits

This tree holds vendored third-party code that doesn't fit the standard
`ThirdPartyLibs/` shape: these are narrow-purpose, single-set-of-consumers, 
frozen upstream, "best we can find." Adding to it should be deliberate — most 
new vendored libs belong in `ThirdPartyLibs/` instead.

---

## How this differs from `ThirdPartyLibs/`

The two trees use the same word ("vendor drop") but mean different things.

**`ThirdPartyLibs/`** is for libs that plug into the standard build plumbing:

- Standard layout: `<lib>/Include/`, `<lib>/lib/<arch>/<config>/`
- A `$(XxxLib)` name property in `ThirdPartyLibs.props` picks the right
  arch/config-specific filename so consumers don't hand-type it
- Include and lib paths are added **globally** to every project via the
  shared `ThirdPartyLibs.props`
- Consumed by many projects across the solution
- Generally a recognizable, still-maintained upstream — vcpkg-port-shaped

Current `ThirdPartyLibs/`: MinHook, SR-Lib, zlib, Xerces-C,
libsquish, DevIL, DirectX SDK (June 2010), Boost.

**`lib/`** is for the misfits:

- No standard layout — each entry's structure is whatever the original
  source shipped
- No global `.props` plumbing for most entries — projects that need an entry
  add per-project `AdditionalIncludeDirectories` / `AdditionalLibraryDirectories`
  (the XML stack is a deliberate exception — see below)
- Typically consumed by **one or two** specific projects
- Upstream is usually dormant or gone; the drop is "best we can find" and
  will not be version-bumped
- Putting something here is itself a signal: *do not modernize this*

The rule of thumb: if the lib plugs cleanly into the
`$(XxxLib)`/`_Arch86`/`_Config` convention and has multiple consumers, it
goes in `ThirdPartyLibs/`. If it's narrow, weird, or frozen, it stays here.

---

## The XML stack: `ticpp_2.5.3/` + `LlamaXML/LlamaXML/` + `LlamaXML_shim.props`

This is the largest entry under `lib/`, and the reason this README exists.
The codebase has two distinct XML clients — one project, one *everyone else* —
and routing them cleanly required the structure documented below.

### Consumers

**Every project that touches XML** — `S3DAPI`, `S3DWrapper7/8/9/10/OGL`,
`DX9GenerateCodeFromDump`, `DX10GenerateCodeFromDump`, every
`OutputMethod` (via the transitive include chain through
`S3DWrapper10/Commands/Command.h → CmdFlowStreamers.h → XMLStreamer.h`,
and the parallel chain through `S3DWrapper9`'s
`BaseStereoRenderer-inl.h → CommandDumper.h`) — gets the ticpp/shim
wiring automatically. See "How the wiring is applied" below.

### Why ticpp lives here, not in `ThirdPartyLibs/`

ticpp is "TinyXML++" — a C++ wrapper around an older TinyXML 2.5.3 that
ticpp bundles inside its own drop. The only direct consumer is the
LlamaXML shim's implementation; no code in the codebase touches ticpp's
`ticpp::Element` wrapper API directly. ticpp upstream
(wxFormBuilder/ticpp) has been dormant for years and 2.5.3 is the latest
available. Single narrow consumer + frozen upstream = exactly the `lib/`
profile, not the `ThirdPartyLibs/` profile.

The drop here was built from the wxFormBuilder/ticpp source with CMake
and `TIXML_USE_TICPP` on (which auto-defines `TIXML_USE_STL`); both
release and debug variants are named `ticpp.lib` and distinguished by
their parent directory under `Lib/<arch>/<config>/`.

### Why the directory is `lib/LlamaXML/LlamaXML/`

The doubled name is deliberate, not a typo:

- **Outer `lib/LlamaXML/`** is the include path that consumers see via
  `LlamaXML_shim.props`. Keeping `lib/LlamaXML` (rather than bare `lib`)
  on the include path keeps the dependency visible and specific.
- **Inner `lib/LlamaXML/LlamaXML/`** is where the actual header files live,
  matching the *import path* used by four source files that already
  `#include "LlamaXML/XMLReader.h"` (etc.) the same way they did under
  the original GPL LlamaXML library. Zero source-file edits required to
  swap from the GPL drop to the shim.

So the include statement `#include "LlamaXML/XMLReader.h"` resolves to
`lib/LlamaXML/LlamaXML/XMLReader.h`. Read the doubled name as
"`lib/LlamaXML` contains the package whose import path is `LlamaXML/`."

### The LlamaXML shim itself

The headers at `lib/LlamaXML/LlamaXML/*.h` are an LGPL 2.1 **API-compatible
reimplementation** of LlamaXML, header-only, backed by ticpp internally.
The shim re-exposes the LlamaXML public surface (`XMLReader`, `XMLWriter`,
`FileInputStream`, `FileOutputStream`, `TextEncoding`, `XMLException` —
all in `namespace LlamaXML`) without licensing entanglement. The original
LlamaXML library was GPL-2 (incompatible with this project) and has been
removed from the tree.

### How the wiring is applied

A single shared props file, `lib/LlamaXML_shim.props`, declares all of:

- the `lib/ticpp_2.5.3/Include` and `lib/LlamaXML` include paths
- the `TIXML_USE_TICPP` preprocessor define
- the `ticpp.lib` link reference and its arch/config-specific lib dir

This props file is auto-imported by the root `Directory.Build.props` for
every project in the tree — no exclusions.

**Net effect: no per-project shim wiring exists in any vcxproj.** The
single source of truth lives in two files (`LlamaXML_shim.props` for
the *what*, `Directory.Build.props` for the *who*). New projects added
to the tree automatically inherit the shim.

### History: TinyXML 2.6.2 removal

S3DAPI previously linked a separate TinyXML 2.6.2 drop (without
`TIXML_USE_TICPP`) because it used `QueryBoolAttribute`, which is absent
in ticpp's bundled TinyXML 2.5.3. This created a cross-DLL ABI mismatch:
`TiXmlNode` objects created by S3DAPI (plain layout) were passed to
OutputMethod DLLs (ticpp layout with `TiCppRC` base class), causing
vtable corruption and crashes in `ReadConfigData`. The fix replaced S3DAPI's
`QueryBoolAttribute` calls with a `QueryIntAttribute`-based helper, allowing
S3DAPI to join ticpp like everyone else — one ABI across all DLLs.

- **Recommendation:** Remove _all_ XML parsing libraries in favor of a
modern version of TinyXML-2 with a single, unified API. Will require
refactoring all call sites but will be more maintainable long-term. If
that ever happens, this entire entry — `ticpp_2.5.3/`, `LlamaXML/`,
`LlamaXML_shim.props` — collapses together as a single removal.

---

## Current contents

| Entry | Purpose | Consumers |
|---|---|---|
| `DXUT/` | DirectX UT framework, retro D3D7/9/10/11 sample helpers | DTest, TestsPack |
| `DbgHelp/` | Microsoft DbgHelp headers used by the minidump path. Cleanup pending — eventual fix is to call the system `dbghelp.dll` instead | `S3DAPI/MiniDump.cpp` |
| `LlamaXML/LlamaXML/` | LGPL 2.1 API-compatible reimplementation of GPL LlamaXML, header-only, ticpp-backed. See the XML stack section above | Everyone except S3DAPI (auto-imported via `Directory.Build.props`) |
| `LlamaXML_shim.props` | Single source of truth for the shim wiring (includes, define, lib, lib dir). Auto-imported per the file above. Goes away when the recommendation in the XML stack section is acted on | (props file, not a code consumer) |
| `d3d10/` | Misnamed — actually the WDK User-Mode Driver DDI headers for D3D9/10/11+DXGI. Load-bearing, do not move to `ThirdPartyLibs/` | DX10/11 wrapper paths |
| `d3d7/` | Vendored DX7 headers. Win10 SDK's `d3d.h` is gated `<0x0800` and becomes empty when `d3d9.h` is in the same TU, so the vendored copy is necessary | S3DWrapper7 |
| `d3d8/` | Vendored DX8.1 headers + import libs. Win10 SDK has zero DX8 content | S3DWrapper8 |
| `gl/` | OpenGL extension headers | S3DWrapperOGL |
| `madCHook/` | API shim layer over MinHook, exposing the historical MadCHook API to existing call sites | S3DAPI, S3DDriver bits |
| `python27/` | Embedded Python 2.7 runtime (see its own README) | tooling |
| `ticpp_2.5.3/` | TinyXML++ C++ wrapper, prebuilt with `TIXML_USE_TICPP`. See the XML stack section above | LlamaXML shim only (transitively via auto-import) |
| `vld/` | Visual Leak Detector. Build wiring is commented out — kept as dormant dev tooling per the codebase convention | n/a (dormant) |

---

## When to add something to `lib/` vs `ThirdPartyLibs/`

Add to **`ThirdPartyLibs/`** if:

- The lib has a recognizable upstream that still ships versioned releases
- The standard layout fits (`Include/`, `lib/<arch>/<config>/`)
- Multiple projects across the solution will consume it
- The `$(XxxLib)` name-property pattern works for arch/config selection

Add to **`lib/`** if:

- The upstream is dormant, gone, or a one-off extract (frozen SDK
  headers, dead C++ wrapper, etc.)
- One or two specific projects consume it; the rest of the solution
  doesn't care
- The build wiring is unusual enough that the standard global props
  would do the wrong thing
- "Best we can find" applies — there isn't a newer version to track

If you're unsure, lean toward `ThirdPartyLibs/`. Things that genuinely
belong in `lib/` tend to announce themselves.
