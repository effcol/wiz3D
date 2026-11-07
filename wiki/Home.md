# wiz3D

A proxy-DLL stereo-3D framework for PC games. LGPL-2.1-or-later. Fork of the iZ3D 1.13 archive, rewritten for modern Windows and modern toolchains.

wiz3D ships as a set of **proxy DLLs** you drop next to a game's executable. The wrapper intercepts the game's graphics API, generates left-and-right-eye frames, and composites them to your chosen output format — side-by-side, top-and-bottom, interlaced, checkerboard, Simulated-Reality weave, or a native-stereo signal if your GPU supports one.

## Pick your path

| You have… | Use |
|---|---|
| Any stereoscopic display that accepts SBS / TAB / Interlaced / Checkerboard (Xreal, 3D TVs, Oculus-type HMDs via third-party drivers, etc.) | **Stereo-Injector** — pick the matching API folder |
| A Leia / Simulated-Reality panel (Acer SpatialLabs, Samsung Odyssey 3D) | **Stereo-Injector** → set OutputMode to `SR Weave` |
| An AMD GPU + older HD3D-supported title | **HD3D** |
| An NVIDIA GPU + shutter glasses, with a game listed in NativeProfile.xml | **3D Vision Direct** |

Which Stereo-Injector folder? Match the game's graphics API:

- `Stereo-Injector/DX7/` — Direct3D 7 (ancient; most 1999-2002 titles)
- `Stereo-Injector/DX8/` — Direct3D 8 (early 2000s)
- `Stereo-Injector/DX9/{32,64}-bit/` — Direct3D 9 (mid-2000s → early 2010s)
- `Stereo-Injector/DX10-11/{32,64}-bit/` — Direct3D 10 / 11 (most modern PC releases)
- `OpenGL-Quad-Buffer-Stereo/{32,64}-bit/` — OpenGL titles (idTech games, Minecraft Java, emulators)
- `Vulkan/` (0.5.0) — Vulkan titles, Dolphin / Cemu / Ryubing / Lichii / KytyPS5

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
