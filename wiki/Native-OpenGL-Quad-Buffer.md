# OpenGL (Quad-Buffer)

✅ **Partially Working.** The OpenGL wrapper is a **native-stereo passthrough**, not an injector. It consumes the game's quad-buffer stereo output (`PFD_STEREO` / `GL_BACK_LEFT` / `GL_BACK_RIGHT`) and composites it into whichever modern stereo output you pick (SBS, TAB, SR Weave, interlaced, etc.).

## How the wrapper works

- `HglGetBooleanv` forces `GL_STEREO` → `TRUE` so the game thinks it has a quad-buffer context (`S3DWrapperOGL/Modified.cpp:316-319`).
- `HwglSetPixelFormat` strips `PFD_STEREO` from the request before passing it to the OS when `EmulateQB=1` (shipped default) — this is what lets us run on any GPU, not just real quad-buffer hardware (`Modified.cpp:343-350`).
- The game calls `glDrawBuffer(GL_BACK_LEFT)` / `GL_BACK_RIGHT` during its own stereo render path.
- `S3DWrapperOGL/Renderer.cpp` routes those per-eye draws separately, then composites at swap-buffer time.
- 0.4.0 unified the OpenGL wrapper with the DX output-mode naming (SR Weave default where supported).

**Requires the game to be QB-aware.** This wrapper will NOT turn a mono-only OpenGL title into stereo. idTech 3 / 4 lineage games (RtCW, Quake 3 engine, Doom 3 engine) typically are QB-aware; ZDoom-based GZDoom-derived titles depend on configuration; several emulators are QB-aware. See each game's notes column below.

## Config

`wiz3D_Config.xml` has an `<EmulateQB Value="1"/>` knob — leave it at 1 unless you have real quad-buffer hardware (rare). The wrapper logs `GL_STEREO`-state diagnostics to `wiz3D_Logs/OpenGLQuadBufferStereo.log`; `GL_STEREO=0` there means the game never requested QB and nothing to capture.

## Install path

`OpenGL-Quad-Buffer-Stereo/32-bit/` or `/64-bit/`. Copy the contents next to the game's `.exe`.

## Games

Checking the [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT) for a given title tells you how to enable its native stereo mode (console command, config flag, etc.).

| Game | Install Path | Testing | Notes |
|---|---|---|---|
| American McGee's Alice | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | Check [3D/VR Compatibility DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rec1TwuooyYEpOvAb). |
| Beyond Sunset | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | Check [3D/VR Compatibility DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX). |
| Darkest Dungeon | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | Check [3D/VR Compatibility DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rec7BJH9upc90nvsi). |
| Captain Wayne - Vacation Desperation | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | Check [3D/VR Compatibility DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX). |
| Final Fantasy IV (3D Remake) | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested |  |
| Final Fantasy IV: The After Years | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested |  |
| FlightGear | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | Follow [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recNbODkwtnnqlQqO) + [FlightGear wiki](https://wiki.flightgear.org/Anaglyph_(3D)). |
| Goodbye Deponia | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recJuY1XRDu9lF9gB). |
| Hands of Necromancy | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX). |
| Hands of Necromancy II | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX). |
| Heavy Metal: F.A.K.K.² | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recx8IhEW9qN2VC6t). |
| Hedon | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX). |
| Hotline Miami | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recW9i65jXiVue5TQ). |
| I Am Sakuya: Touhou FPS Game | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX). |
| Prey (2006) | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rech3exawnWvSNm31). |
| Quake | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=reciOCmaXBxKj4N3D). |
| Quake II | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recY4PQQVRFPDYUh7). |
| Quake III Arena | `OpenGL-Quad-Buffer-Stereo/32-bit` | ✅ Working | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rec1zJdFTfKoty2ZO) + [ioquake3 wiki](https://web.archive.org/web/20190507130336/https://wiki.ioquake3.org/Stereo_Rendering). |
| Project Absentia | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX). |
| REKKR: Sunken Land | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX). |
| Return to Castle Wolfenstein | `OpenGL-Quad-Buffer-Stereo/32-bit` | ✅ Working | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recIgh2WvNLp0VdKe). |
| Selaco | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX). |
| SpaceEngine | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recg8wr1aqpc4BoFH). |
| Star Wars: Jedi Knight - Jedi Academy | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=reclLl1NuWWFtyD73). |
| Star Wars: Jedi Knight II: Jedi Outcast | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recg8qqxNUmWEcMHS). |
| SuperTuxKart | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rectLIRCA4uYSpMnk). |
| Supplice | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX). |
| The Binding of Isaac: Rebirth | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recYlJ9YTgmF7wMc5). |
| The Forestale | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX). |
| Twilight Town: A Cyberpunk FPS | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX). |
| Twilight Manor: Roguelite FPS | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX). |
| Vomitoreum | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX). |
| World of Padman | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rec5wq4OBzfTqGSJq). |
| Wrath: Aeon of Ruin | `OpenGL-Quad-Buffer-Stereo/32-bit` | 🧪 Untested | [3D/VR DB](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recVVJQxrjuGFbsxA). |
