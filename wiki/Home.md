# wiz3D

A proxy-DLL stereo-3D framework for PC games. LGPL-2.1-or-later. Fork of the iZ3D 1.13 archive, rewritten for modern Windows and modern toolchains.

wiz3D ships as a set of **proxy DLLs** you drop next to a game's executable. The wrapper intercepts the game's graphics API, generates left-and-right-eye frames, and composites them to your chosen output format — side-by-side, top-and-bottom, interlaced, checkerboard, anaglyph, mirror, NVIDIA 3D Vision shutter, Katanga shared texture, or a native-stereo signal if your GPU supports one.

## Pick your path

wiz3D ships two broadly different kinds of path:

- **Stereo-Injector** — wiz3D generates both eye frames from a mono game. Use this for any DirectX / Vulkan title you want in stereo.
- **Native-stereo** — the game (or driver) already produces two eye frames; wiz3D just composites them to your display. Use this for OpenGL titles that use quad-buffer stereo (RtCW, Quake 3 / Doom 3 engines, Minecraft with a stereo mod, etc.), AMD HD3D-era titles, and NVIDIA 3D Vision titles.

| You have… | Use |
|---|---|
| Any stereoscopic display that accepts SBS / TAB / Interlaced / Checkerboard / Anaglyph (Xreal, 3D TVs, Oculus-type HMDs via third-party drivers, etc.) + a DirectX / Vulkan game | **Stereo-Injector** — pick the matching API folder |
| An OpenGL title that supports quad-buffer stereo natively (older id Software games, Minecraft + shader mod, emulators) | **OpenGL Quad-Buffer** (native) |
| An AMD GPU + an HD3D-supported title | **HD3D** (native) |
| An NVIDIA GPU + shutter glasses + a game listed in `NativeProfile.xml` | **3D Vision Direct** (native) |
| An NVIDIA 3D Vision "Automatic Mode"-era title (had an NVIDIA 3D Vision profile) | **Stereo-Injector** — see [3D Vision Automatic-aware games](Injector-3D-Vision-Automatic-Aware) |

Which Stereo-Injector folder? Match the game's graphics API:

- `Stereo-Injector/DX7/` — Direct3D 7 (most 1999-2002 titles) — see [DirectX 7](API-DirectX-7)
- `Stereo-Injector/DX8/` — Direct3D 8 (early 2000s) — see [DirectX 8](API-DirectX-8)
- `Stereo-Injector/DX9/{32,64}-bit/` — Direct3D 9 (mid-2000s → early 2010s) — see [DirectX 9](API-DirectX-9)
- `Stereo-Injector/DX10-11/{32,64}-bit/` — Direct3D 10 & 11 (shared DLL set; most modern PC releases) — see [DirectX 10](API-DirectX-10) and [DirectX 11](API-DirectX-11)
- Direct3D 12 — **coming soon**, see [DirectX 12](API-DirectX-12)
- `Stereo-Injector/Vulkan/` (0.5.0) — Vulkan titles, Dolphin / Cemu / Ryubing / Lichii / KytyPS5 — see [Vulkan](API-Vulkan)
- `OpenGL-Quad-Buffer-Stereo/{32,64}-bit/` — **native path**, not an injector. Requires the OpenGL game to be quad-buffer-stereo-aware (writing to `GL_BACK_LEFT` / `GL_BACK_RIGHT` itself). Mono-only OpenGL titles will NOT be upgraded to stereo by this wrapper — a separate OGL injector is planned.

If you don't know the API, [Quick-Start](Quick-Start) has a short way to figure it out.

## Where things are

- **Installation and first-run** → [Quick-Start](Quick-Start)
- **Does my game work?** → [Compatibility](Compatibility)
- **What output format do I want?** → [Output Modes](Output-Modes)
- **Hotkey reference** → [Hotkeys](Hotkeys)
- **Editing profile XML** → [Profiles](Profiles)
- **Something's wrong** → [Troubleshooting](Troubleshooting)

## Project links

- Source and releases: https://github.com/effcol/WiZ3D
- Licence: LGPL-2.1-or-later (fork retains the original iZ3D 1.13 licence)
- Sibling projects: Stereopticon (launcher), SR-Loom (desktop weaver)
