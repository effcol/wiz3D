# Compatibility

The wiz3D-tested games list. Entries are **community-reported** unless marked `[maintainer]`. If a title you've tested isn't here, please add a row via PR or open an issue — the goal is to make this exhaustive over time.

**Status key**

| Status | Meaning |
|---|---|
| ✅ Working | Stereo renders correctly; any per-game fix is shipped in `BaseProfile.xml`. |
| 🟡 Playable | Stereo works but has rendering artefacts (shadows, reflections, UI depth). Usually improved by a per-game ShaderFix. |
| 🔴 Broken | Crashes, hangs, or stereo is fundamentally wrong (wrong eye content, no depth). |
| 🧪 Pending | Awaiting a test report. |

**What's in each cell**

- **Mode** — the OutputMode value that was tested. Any output mode in that column should work unless noted.
- **Fix** — points at a per-game profile entry in `BaseProfile.xml` or a user override in `UserProfile.xml`.
- **Notes** — gotchas (Admin install, env-var setup, known-broken effects).

---

## Direct3D 11

Akshay's Feature Engine (PR #31, 0.4.0+) does most of the heavy lifting here. From 0.5.0 onward Akshay's DX11 update targets *stereo renders at all* on any DX11 title that uses standard constant-buffer layouts, even if shadows or reflections still need per-game work.

| Game | Engine | Status | Mode | Fix | Notes |
|---|---|---|---|---|---|
| BioShock Infinite | Unreal 3 | ✅ | SBS, TAB, SR Weave | BaseProfile | Also works via **3D Vision Direct** path on NVIDIA. |
| Batman: Arkham City | Unreal 3 | ✅ | — | BaseProfile | Also works via 3D Vision Direct. |
| Tomb Raider (2013) | Crystal Dynamics | ✅ | — | BaseProfile | Also works via 3D Vision Direct. |
| Sniper Elite 4 | Asura | ✅ | — | — | Native **HD3D** path recommended. |
| Deus Ex: Human Revolution Director's Cut | Crystal Dynamics | ✅ | — | BaseProfile | Native **HD3D** path. |
| Max Payne 3 | RAGE | ✅ | — | — | Native **3D Vision Direct** path. |
| Alan Wake | Alan Wake Engine | ✅ | — | — | Native **3D Vision Direct** path. |
| Hard Reset | Road Hog Engine | ✅ | — | — | Native **3D Vision Direct** path. |
| Persona 5 Royal | Catherine Classic engine | 🧪 | — | — | Pending 0.5.0 DX11 update test. |
| Sekiro: Shadows Die Twice | Dantelion2 | 🧪 | — | — | Pending 0.5.0 DX11 update test. |
| Prey (2017) | CryEngine (Arkane fork) | 🧪 | — | — | Pending 0.5.0 DX11 update test. |

Known DX11 ShaderFix profile coverage (14 titles in `BaseProfile.xml` as of 0.4.3) — see `D:/Coding/WiZ3D/releases/wiz3D/Stereo-Injector/DX10-11/<arch>/BaseProfile.xml` for the authoritative list.

## Direct3D 9

| Game | Engine | Status | Mode | Fix | Notes |
|---|---|---|---|---|---|
| Crysis | CryEngine 2 | ✅ | SBS, TAB | BaseProfile | |
| TinTin: The Secret of the Unicorn | Unreal 3 | 🟡 | SBS, SR Weave | — | 3D Vision-aware internally; UI mono needs <MarkMono> fix. See community issue. |
| Resident Evil 5 | MT Framework | 🟡 | — | BaseProfile | `Reset(fmt=A8R8G8B8)` spoof applied; alpha shadow pass still being inspected. |
| UDK-based titles (generic) | Unreal 3 | ✅ | — | — | Also works via **3D Vision Direct** generic UDK.exe profile. |
| Deathspank | custom | 🔴 | — | — | Black screen on launch — investigation open. |

## Direct3D 10

Historically treated as a thin DX11 sibling; the DX10 proxy is `dxgi.dll` and reuses DX10-11 settings. Game list same as DX11 for pure-DX10 titles.

## Direct3D 7

Shimmed to DX9 internally (`S3DWrapper7/Direct3D7.cpp:54` calls `Direct3DCreate9()`). Inherits output modes + OSD + hotkeys from the DX9 path, so anything that drives stereo in DX9 lights up here too. The proxy surface is full DirectDraw + Direct3D 7 (DirectDrawClipper, DirectDrawPalette, Direct3DVertexBuffer7…), separate from DX8.

| Game | Status | Notes |
|---|---|---|
| DX7 titles (generic) | 🧪 | Known issue: 16-bit back-buffer games on Win10/11 may need R5G6B5 mapping — see `project_dx7_potential_issues` notes. Please file reports with specific titles. |

## Direct3D 8

Also shimmed to DX9 internally (`S3DWrapper8/S3DWrapper8.cpp:432` → `Direct3DCreate9()`), with its own full Direct3D8 proxy hierarchy (Direct3DBaseTexture8, Direct3DCubeTexture8, Direct3DIndexBuffer8…). Ships as a distinct DLL set in `Stereo-Injector/DX8/`.

Historical gotcha landed in 0.4.x: `BuildAdapterModesList` had an R5G6B5-seed bug that broke adapter enumeration; fixed. Logging path emits `[wrap]` lines regardless of release build (see `project_dx8_wrapper_diagnostics` notes).

| Game | Status | Notes |
|---|---|---|
| DX8 titles (generic) | 🧪 | Please file reports with specific titles. |

## Vulkan (0.5.0 — in flight)

Akshay's upcoming Vulkan wrapper. Expected PR around 2026-11-08. Known targets:

| Game / target | Status | Notes |
|---|---|---|
| Red Dead Redemption 2 | 🧪 | Akshay's primary test target. |
| Doom (2016) | 🧪 | Pending our test. |
| Doom Eternal | 🧪 | Pending our test. |
| Cemu — Breath of the Wild | 🧪 | Pending. |
| Ryubing — Tears of the Kingdom | 🧪 | Pending. |
| Lichii — Ocarina of Time Remake | 🧪 | **Env var required** — otherwise the emulator uses the system Vulkan loader and skips our proxy. |
| KytyPS5 — Demon's Souls | 🧪 | **Env var required**. Needs a stereo-mode shader-precompilation pass to avoid first-run stutter. Akshay reports 200 FPS with 4× MFG + DLSS Quality. |

## Native-stereo paths

In these paths the **game** produces two eye frames; wiz3D just composites them to your chosen output format. This is a different shape to the Stereo-Injector wrappers above (which generate the second eye themselves from a mono game).

### OpenGL (Quad-Buffer Stereo)

Lives in `OpenGL-Quad-Buffer-Stereo/{32,64}-bit/`. The wrapper tells the game "yes you have a quad-buffer stereo context" (via `HglGetBooleanv(GL_STEREO)` returning `TRUE` and the `EmulateQB=1` shipped default in `wiz3D_Config.xml`), the game does its own L/R rendering to `GL_BACK_LEFT` / `GL_BACK_RIGHT` through standard OpenGL stereo API, and the wrapper composites those buffers for your display. Works on any hardware — wiz3D is lying to the game about QB support, so you don't need a Quadro / FirePro with real QB stereo.

**Requires the game to be QB-aware**. idTech 3 / 4 titles (RtCW, Quake 3, Doom 3 and derivatives) typically support this; so do several emulators. A mono-only OpenGL title (anything that never calls `glDrawBuffer(GL_BACK_LEFT)` etc.) will NOT be upgraded to stereo by this wrapper — that's planned as a separate OGL Stereo Injector sibling project.

| Game | Engine | Status | Mode | Notes |
|---|---|---|---|---|
| Return to Castle Wolfenstein | idTech 3 | ✅ | SR Weave | 0.4.x. |
| Minecraft (Java) + stereo mod | — | 🧪 | — | Needs a shader mod that emits QB stereo — bare Minecraft doesn't. |

### HD3D (AMD)

Requires: AMD GPU with HD3D support (older driver series). Games write stereo directly; wiz3D composites.

Known-good:

- Sniper Elite 4 (DX11)
- Deus Ex: Human Revolution Director's Cut (DX9)

### 3D Vision Direct (NVIDIA, via NvDirectMode)

The NvApi-side stand-in for the retired 3D Vision Automatic / Direct Mode stack. Does **not** drive shutter glasses on its own — it's a per-eye capture + compositor. For shutter output use the NV3D-Lib path (0.5.0+) that ships alongside.

Known-good (from shipped `NativeProfile.xml`):

- Max Payne 3
- Hard Reset
- Tomb Raider (2013)
- BioShock Infinite
- Batman: Arkham City
- Alan Wake
- UDK (generic UDK.exe)

### Simulated Reality (Leia / SR)

Phase-2a `SRWeave` passthrough. Validated end-to-end on an Acer SpatialLabs panel with CP2077 (2026-10-30). Known limit: stereo-settings change mid-game can crash.

| Game | Panel | Status | Notes |
|---|---|---|---|
| Cyberpunk 2077 | Acer SpatialLabs | ✅ | Driving Night City in stereo via SRWeave. |

## How to add a row

1. **Fork** this wiki, or raise an issue with your test report.
2. Row shape: `Game | Engine | Status | Mode | Fix | Notes`.
3. For "Fix" — if the title already needed a per-game profile, say which section of `BaseProfile.xml` or whether it was a `UserProfile.xml` override.
4. **Please include**: wiz3D version tested, GPU + driver, OS, output-mode value, screenshot if you can.

Reports via GitHub issue (preferred — easier to triage) or Discord.
