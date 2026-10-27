# wiz3D "We See 3D"

A universal stereoscopic 3D wrapper for DirectX 7-11, OpenGL, AMD HD3D, and Nvidia 3D Vision. Based on iZ3D. 

**iZ3D** meant "**I** See 3D", so **wiz3D** means "**We** See 3D"

<img width="5760" height="1080" alt="vlcsnap-2026-04-05-21h46m56s809_Parralel _and_Cross" src="https://github.com/user-attachments/assets/b8d2c574-2962-4bfa-b15d-380956552a32" />
<img width="5760" height="1080" alt="vlcsnap-2026-04-03-22h37m17s146_Parralel _and_Cross" src="https://github.com/user-attachments/assets/89859e2f-915f-499f-a307-b8738d0e561f" />
<img width="5760" height="1080" alt="Screenshot 2026-03-26 190057_Parralel _and_Cross" src="https://github.com/user-attachments/assets/493f2e32-1c3b-42c6-984d-ef9eacf96620" />

---

## What Is This?
wiz3D is an open-source stereoscopic 3D wrapper that hooks into DirectX, OpenGL, and AMD HD3D native and Nvidia 3D Vision games to generate real-time stereo 3D output (Half Side-by-Side, Anaglyph, Simulated Reality, etc.) without requiring kernel drivers or proprietary hardware or software.

iZ3D was a commercial product (~2002–2010) and one of the pioneers in modding games for stereoscopic 3D using kernel-level driver injection. The original developers kindly open-sourced the code under the MIT license, hosted by [bo3b/iZ3D](https://github.com/bo3b/iZ3D).

This project modernises that source code, replaces kernel-level hooks with a proxy DLL loader, and expands the scope to re-enable stereoscopic 3D in games that have native AMD HD3D and Nvidia 3D Vision support.

### Credits & Acknowledgements
wiz3D stands on the shoulders of some incredible people and projects. Huge thanks to:
- **Vadim and the original iZ3D development team**, for building the groundbreaking stereoscopic 3D wrapper that this project modernises, and for releasing it to the community.
- **[bo3b](https://github.com/bo3b)**, for hosting the iZ3D source archive at [bo3b/iZ3D](https://github.com/bo3b/iZ3D) under an MIT license, and for laying the groundwork for people to contribute.
- **[@AkshayUHegde](https://github.com/AkshayUHegde)**, for PR #31, which lands the full DX11 Stereo-Injector pipeline in 0.4.0 (COM-wrap, DXBC analyser, 14 per-game profiles).
- **The NV3DLib project**, LGPLv2.1, which 0.5.0 statically links into every output-producing wrapper to drive NVIDIA 3D Vision shutter glasses on modern Windows (`OutputMode=3DVisionGlasses`). wiz3D authored and upstreamed NV3DLib's native DX9 backend as part of this work. See `THIRD_PARTY_NOTICES.txt` entry 11 for the full LGPLv2.1 notice and build-from-source instructions.
- **[bo3b](https://github.com/bo3b)** (again), for the open-source [Katanga](https://github.com/bo3b/Katanga) 3D viewer. 0.5.0's `OutputMode=Katanga` publishes to Katanga's shared-texture protocol from every wrapper; no Katanga source is linked, so the protocol stays an API contract — see `THIRD_PARTY_NOTICES.txt` entry 12.
- Every contributor who has filed issues, PRs or game-test results. The compatibility tables below exist because of you.

## Current Status

* **Nvidia 3D Vision 'Direct Mode':** ✅ **Mostly Working.** _(expanded in 0.4.0)_ DX9, DX10 and DX11 vendor proxies all ship. 0.4.0 adds the MinHook migration on d3d9, SR Weave port, `NativeProfile.xml` cross-session persistence, and the DX11 NVAPI stereo bridge (so games can drive wiz3D's separation/convergence from their own 3D settings).
* **AMD HD3D:** ✅ **Mostly Working!** HD3D games render stereo internally; the proxy enables rendering, captures the quad-buffer output and displays it using modern stereo-3D formats. Currently Half SBS / Half TAB work for about half the games; the rest still need output work. Deus Ex: Human Revolution + Director's Cut fixed in 0.4.0.
* **OpenGL Quad-Buffer Stereo:** ✅ **Partially Working.** Captures native OpenGL quad-buffer stereo output (`PFD_STEREO` / `GL_BACK_LEFT/RIGHT`) from games that produce it (id Tech 3/4, CAD, flight sims) and displays it in modern stereo formats. 0.4.0 migrates OpenGL to the unified OutputMode naming (SR Weave default where supported). Does not inject stereo into mono OpenGL games, that's a separate project planned for a later release.
* **DirectX 11:** ✅ **Mostly Working!** _(new in 0.4.0)_ Full DX11 COM-wrap pipeline, devices, deferred contexts, DXGI 1.2+ factories, DXBC analyser with per-shader view-projection capture, draw-time constant patching, HUD at scene depth, stereo scissors, deferred-light + decal + fog + screen-ray cameras, compute/UAV eye duplication, 3D Vision stereo texel for GameWorks ShadowLib. Per-game profiles ship for Battlefield 3, Battlefield Hardline, BioShock Infinite, Far Cry 3, Far Cry 4, Grand Theft Auto V (Legacy), Mass Effect: Andromeda, Mass Effect Legendary Edition, Max Payne 3, Metro Redux, NieR:Automata, Ryse: Son of Rome, Star Wars Jedi: Fallen Order and What Remains of Edith Finch.
* **DirectX 10:** ⚠️ **In Progress.** The DX10 vendor proxy was revived for 0.4.0 (SR stub + Win11 SDK fix) and builds again, but the DX10 analyser coverage is minimal, DX10 inherits DX11 scaffolding but no per-game profiles currently work. Testing DX10 titles is one of the biggest open contribution areas.
* **DirectX 9:** ✅ **Mostly Working!** `d3d9.dll` proxy loader works. Left 4 Dead 2 and many others run in full stereo 3D; profile system loads shader fixes and stereo settings for every originally-supported iZ3D game. Modern OSD + laser-sight crosshair ported from DX11 in 0.4.0.
* **DirectX 8:** ✅ **Partially Working.** Wrapper that converts DX8 to use the DX9 stereoisation path. Known-good on a handful of titles, needs broader coverage.
* **DirectX 7:** ⚠️ **In Progress.** Wrapper that converts DX7 to use the DX9 stereoisation path. DX7→DX9 conversion is working, present for testing.
* **Nvidia 3D Vision 'Automatic Mode':** ✅ **Partially Working.** Games that relied on NVIDIA's driver stereo injection, the ones listed have 3D Vision in-game settings, shader fixes and config options. wiz3D's stereo injector replaces the driver path, with the aim of letting those shader fixes and separation/convergence sliders apply to wiz3D's output. In 0.4.0 the NVAPI stereo bridge extends to DX11, so convergence and separation sliders now hook up via `NvAPI_Stereo_SetConvergence`/`SetSeparation` on both DX9 and DX11.

---

## Getting Started Playing

1. Launch the game before downloading, and set the resolution and refresh rate, and make sure fullscreen is enabled.
2. Download the latest release from the **Releases** tab.
3. Find the executable (`.exe`) of the game you want to play, and check its entry in the tables below, the **Install Path** column tells you which `wiz3D` subfolder to use.
4. Copy the **contents** of that subfolder (e.g. the contents of `Stereo-Injector/DX9/32-bit/` for a DX9 32-bit game) directly into the folder containing the game's `.exe`.
5. Launch the game. Stereo 3D should activate automatically! If it's an HD3D game, you may need to activate `Stereoscopic 3D` or `HD3D` in the in-game menu.

Configure your output mode (Half Side-by-Side, Anaglyph, SR Weave, etc.) and any other settings you'd like to modify using the included `wiz3D_Config.xml` file.

---

## wiz3D Game Test Results

*Legend:* ✅ ***Working** = Stereo output activated and playable.* ⚠️ ***Partial** = Stereo activated but with issues (crash, wrong settings, shader problems).* ❌ ***Not loading** = Wrapper not activating. **Untested** = I haven't tested yet or don't have access to that game*

The **Install Path** column shows which wiz3D folder you copy next to the game's `.exe`.

### Nvidia 3D Vision "Direct Mode" Games 
Games that render stereoscopic 3D themselves and display that via 3D Vision's 'Direct Mode'.

| Game | Install Path | Testing | Notes |
|------|--------------|---------|-------|
| Hard Reset | `3D-Vision-Direct/DX9/32-bit` | ✅ Working | Game must be set to 120Hz. |
| Hitman: Absolution | `3D-Vision-Direct/DX11/32-bit` | ✅ Working | Game supports 3D Vision Direct Mode and HD3D. |
| Oil Rush | `3D-Vision-Direct/DX11/32-bit` | ✅ Working | DX11 output only. |
| Tomb Raider (2013) | `3D-Vision-Direct/DX11/32-bit` | ✅ Mostly Working | Game supports 3D Vision Direct Mode and HD3D. Use the `EOSStub` subfolder if you need Epic Online Services disabled for SR Weave cohabitation. |

- **Excluded (Depth Map not loading, better support on HD3D):** *Battlefield 3*, *DiRT 2*, *DiRT 3*, *DiRT 3 Complete Edition*, *DiRT Showdown*, *DiRT Rally*, *F1 2010*, *F1 2011*, *F1 2012*, *F1 2013*, *GRID 2*, *GRID Autosport*.
- **Excluded (Works on HD3D and 3D Vision Direct Mode, HD3D recommended):** *World of Warcraft*. <sub>(Stereo3D removed in 2018 DX12 update)</sub>

### AMD HD3D Native Games

| Game | Install Path | Testing | Notes |
|------|--------------|---------|-------|
| Battlefield 3 | `HD3D/32-bit` | ✅ Mostly Working | Campaign only. Strange depth issue in the game not seeming caused by wiz3D. Might need shader fix. |
| Deus Ex: Human Revolution | `HD3D/32-bit` | ✅ Working |  |
| Deus Ex: Human Revolution Director's Cut | `HD3D/32-bit` | ✅ Working |  |
| DiRT 2 | `HD3D/32-bit` | Untested | `hardware_settings_config.xml` needs `stereo enabled="true"`. |
| DiRT 3 | `HD3D/32-bit` | Untested | `hardware_settings_config.xml` needs `stereo enabled="true"`.<br>Check iZ3D Shader Fix. |
| DiRT 3 Complete Edition | `HD3D/32-bit` | ⚠️ Partial | `hardware_settings_config.xml` needs `stereo enabled="true"`.<br>Only top half of Half SBS visible. |
| DiRT Showdown | `HD3D/32-bit` | ⚠️ Partial | `hardware_settings_config.xml` needs `stereo enabled="true"`.<br>Only top half of Half SBS visible. |
| DiRT Rally | `HD3D/32-bit` | ⚠️ Partial | `hardware_settings_config.xml` needs `stereo enabled="true"`.<br>Only top half of Half SBS visible. |
| F1 2010 | `HD3D/32-bit` | Untested |  |
| F1 2011 | `HD3D/32-bit` | Untested |  |
| F1 2012 | `HD3D/32-bit` | Untested |  |
| F1 2013 | `HD3D/32-bit` | Untested |  |
| GRID 2 | `HD3D/32-bit` | ⚠️ Partial | `hardware_settings_config.xml` needs `stereo enabled="true"`.<br>Only top half of Half SBS visible. |
| GRID Autosport | `HD3D/32-bit` | ⚠️ Partial | `hardware_settings_config.xml` needs `stereo enabled="true"`.<br>Only top half of Half SBS visible. |
| Sleeping Dogs | `HD3D/32-bit` | ✅ Working |  |
| Sleeping Dogs: Definitive Edition | `HD3D/64-bit` | ✅ Working | Undocumented AMD HD3D support. |
| Sniper Elite V2 | `HD3D/32-bit` | ✅ Working | Colour shifting when separation increased.<br>[Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Sniper Elite III | `HD3D/32-bit` | ✅ Working | `customersupportlogging` beta branch only.<br>[Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Sniper Elite 4 | `HD3D/64-bit` | ✅ Working | DX11 only. DX12 doesn't seem to trigger HD3D stereo.<br>[Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Sniper Elite: Nazi Zombie Army | `HD3D/32-bit` | ✅ Working | Colour shifting when separation increased.<br>[Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Sniper Elite: Nazi Zombie Army 2 | `HD3D/32-bit` | ✅ Working | Colour shifting when separation increased.<br>[Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Zombie Army Trilogy | `HD3D/32-bit` | ✅ Working | [Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Thief (2014) | `HD3D/32-bit` or `HD3D/64-bit` | ✅ Working | Both x86 and x64 working. |

- **Excluded (Works on 3D Vision Direct Mode):** *Hitman: Absolution*, *Tomb Raider (2013)*.
- **Excluded (Native SBS/TAB):** *Crysis 2*, *Crysis 3*, *Rise of the Tomb Raider*, *Shadwen*, *Two Worlds II*, *Trine 1*, *Trine 2*, *Trine 3*.
- **Excluded (Online Ban Risk):** *World of Warcraft*. <sub>(Stereo3D removed in 2018 DX12 update)</sub>

### OpenGL Quad-Buffer Stereo Games

| Game | Install Path | Testing | Notes |
|------|--------------|---------|-------|
| American McGee's Alice | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rec1TwuooyYEpOvAb) to enable the game's native stereo 3D. |
| Beyond Sunset | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Darkest Dungeon | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rec7BJH9upc90nvsi) to enable the game's native stereo 3D. |
| Captain Wayne - Vacation Desperation | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Final Fantasy IV (3D Remake) | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested |  |
| Final Fantasy IV: The After Years | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested |  |
| FlightGear | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Use instructions on [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recNbODkwtnnqlQqO) and [flightgear wiki](https://wiki.flightgear.org/Anaglyph_(3D)) to enable the game's native stereo 3D. |
| Goodbye Deponia | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recJuY1XRDu9lF9gB) to enable the game's native stereo 3D. |
| Hands of Necromancy | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Hands of Necromancy II | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Heavy Metal: F.A.K.K.² | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recx8IhEW9qN2VC6t) to enable the game's native stereo 3D. |
| Hedon | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Hotline Miami | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recW9i65jXiVue5TQ) to enable the game's native stereo 3D. |
| I Am Sakuya: Touhou FPS Game | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Prey (2006) | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rech3exawnWvSNm31) to enable the game's native stereo 3D. |
| Quake | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=reciOCmaXBxKj4N3D) to enable the game's native stereo 3D. |
| Quake II | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recY4PQQVRFPDYUh7) to enable the game's native stereo 3D. |
| Quake III Arena | `OpenGL-Quad-Buffer-Stereo/32-bit` | ✅ Working | Use instructions on [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rec1zJdFTfKoty2ZO) and [ioquake3 wiki](https://web.archive.org/web/20190507130336/https://wiki.ioquake3.org/Stereo_Rendering) to enable the game's native stereo 3D. |
| Project Absentia | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| REKKR: Sunken Land | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Return to Castle Wolfenstein | `OpenGL-Quad-Buffer-Stereo/32-bit` | ✅ Working | Use instructions on [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recIgh2WvNLp0VdKe) to enable the game's native stereo 3D. |
| Selaco | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| SpaceEngine | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recg8wr1aqpc4BoFH) to enable the game's native stereo 3D. |
| Star Wars: Jedi Knight - Jedi Academy | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=reclLl1NuWWFtyD73) to enable the game's native stereo 3D. |
| Star Wars: Jedi Knight II: Jedi Outcast | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recg8qqxNUmWEcMHS) to enable the game's native stereo 3D. |
| SuperTuxKart | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rectLIRCA4uYSpMnk) to enable the game's native stereo 3D. |
| Supplice | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| The Binding of Isaac: Rebirth | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recYlJ9YTgmF7wMc5) to enable the game's native stereo 3D. |
| The Forestale | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Twilight Town: A Cyberpunk FPS | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Twilight Manor: Roguelite FPS | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Vomitoreum | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| World of Padman | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rec5wq4OBzfTqGSJq) to enable the game's native stereo 3D. |
| Wrath: Aeon of Ruin | `OpenGL-Quad-Buffer-Stereo/32-bit` | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recVVJQxrjuGFbsxA) to enable the game's native stereo 3D. |

### DirectX 10 / 11 Games

DirectX 11 titles use the full 0.4.0 Stereo-Injector COM-wrap pipeline. DirectX 10 titles use the same wrapper but with minimal analyser coverage, they're listed here for testing.

| Game | Install Path | iZ3D Profile | Testing | Notes |
|------|--------------|--------------|---------|-------|
| Aliens vs. Predator (2010) | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ DX9 Working | Needs shadow shader fix on DX9. |
| Assassins Creed | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| Battlefield 3 | `Stereo-Injector/DX10-11/32-bit` | ✕ | ✅ Working | Works with DirectX 11. |
| Battlefield Hardline | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working |  |
| BioShock | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| BioShock 2 | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| BioShock Infinite | `Stereo-Injector/DX10-11/32-bit` | ✕ | ✅ Working |  |
| Call of Juarez: Bound in Blood | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| Company of Heroes | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| Cryostasis | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| Crysis | `Stereo-Injector/DX10-11/32-bit` or `64-bit` | ✓ | Untested |  |
| Crysis: Warhead | `Stereo-Injector/DX10-11/32-bit` or `64-bit` | ✓ | Untested |  |
| Crysis 2 | `Stereo-Injector/DX10-11/32-bit` or `64-bit` | ✓ | Untested | Includes Shader Fix. See if can be applied to the game's native SBS. |
| DCS: Black Shark | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested | Single Player may be okay. Multiplayer not recommended. |
| De Blob | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| Dragon Age II | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| Far Cry 2 | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| Far Cry 3 | `Stereo-Injector/DX10-11/32-bit` | ✕ | ✅ Working | DX11 only. Fullscreen or windowed recommended over borderless. |
| Far Cry 4 | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working |  |
| F.E.A.R. 3 | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| Gears of War | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| Homefront | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| Lost Planet | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| Mass Effect Andromeda | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working |  |
| Mass Effect Legendary Edition | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working | Mass Effect 1, 2 and 3. Launcher window renders at 1440p when panel is sub-native, being refined. |
| Metro 2033 Redux | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Mostly Working | Minor shader artefacts. |
| Metro: Last Light Redux | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Mostly Working | Shares the Metro Redux profile. |
| Microsoft Flight Simulator X | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| NecroVisioN | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| NieR:Automata | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working |  |
| Red Faction: Guerrilla | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| Ryse: Son of Rome | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working |  |
| S.T.A.L.K.E.R.: Clear Sky | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| S.T.A.L.K.E.R.: Call of Pripyat | `Stereo-Injector/DX10-11/32-bit` or `64-bit` | ✓ | Untested |  |
| Serious Sam HD: The First Encounter | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| Star Wars Jedi: Fallen Order | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working | Launcher renders at 1440p when panel is sub-native. |
| Tempest 4000 | `Stereo-Injector/DX10-11/32-bit` | ✕ | ✅ Working |  |
| Tom Clancy's H.A.W.X | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| Tom Clancy's Splinter Cell: Blacklist | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Mostly Working | DX9 only. Needs shader fix. |
| Trine 4: The Nightmare Prince | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Mostly Working | Fireplaces can flash in the right eye. |
| Trine 5: A Clockwork Conspiracy | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Mostly Working | Raindrops hitting the ground make a puff of smoke in the right eye instead of ripples. |
| What Remains of Edith Finch | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working |  |
| World in Conflict | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |

- **Excluded (Online Ban Risk):** *Age of Conan: Unchained*, *Champions Online*, *DC Universe Online*, *Entropia Universe*, *Final Fantasy 14*, *TERA Online*, *The Lord of the Rings Online*, *Warcraft III: Reign of Chaos*, *World of Tanks*.

### DirectX 9 Games (32-bit)

| Game | Install Path | iZ3D Profile | Testing | Notes |
|------|--------------|--------------|---------|-------|
| Antichamber | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| A.R.E.S.: Extinction Agenda | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| AaAaAA!!! A Reckless Disregard for Gravity | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Alan Wake | `Stereo-Injector/DX9/32-bit` | ✕ | ❌ Not Working | Crashes on title screen. |
| Alien Breed 2: Assault | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Alien Swarm | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Aliens Versus Predator | `Stereo-Injector/DX9/32-bit` | ✕ | ❌ Not Working | Crashes on level load. |
| Alone in the Dark (2008) | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| America's Army | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Anomaly Warzone Earth | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| AquaNox 2: Revelation | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Arma: Armed Assault | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | aka ARMA: Combat Operations  |
| Armies of Exigo | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Assassin's Creed II | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Assassin's Creed: Brotherhood | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Audiosurf | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Back to the Future: The Game | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | Episode 1-5 |
| Batman: Arkham Origins Blackgate | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| Battlefield 2 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Battlefield 2142 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Battlestations: Pacific | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Beowulf: The Game | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Bet on Soldier | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Beyond Good & Evil | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Mostly Working | Artifacts when paused. |
| Bayonetta | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Performance issues. Might need shader fix. |
| Bionic Commando (2009) | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Partially Working | Needs HUD and shadow shader fix, fullscreen exclusive, crashes on AMD. |
| Bionic Commando Rearmed | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Black & White 2 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Black Mesa | `Stereo-Injector/DX9/32-bit` | ✕ |  ❌ Not Working | Crashing. |
| BlazBlue: Calamity Trigger | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Mostly Working | Needs sprite depth shader fix, use [Geo-11 Fix instead](https://helixmod.blogspot.com/2026/05/blazblue-calamity-trigger-dx11.html). |
| Blur | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | May need shader fix. |
| Borderlands | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Brothers in Arms: Road to Hill 30 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Brothers in Arms: Earned in Blood | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Brothers in Arms: Hell's Highway | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Bulletstorm | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Burnout Paradise | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Cabela's Big Game Hunter: 10th Anniversary Edition | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | Not sure which game, profile says "Cabela Big Game Hunter BGH10.exe". |
| Call of Cthulhu: Dark Corners of the Earth | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Call of Duty 2 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Call of Duty: World at War | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Call of Duty 4: Modern Warfare (2007) | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Call of Duty: Modern Warfare 2 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Cars | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Castlevania: Lords of Shadow 2 | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Mostly Working | Needs shadow shader fix. |
| Chromadrome 2 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Command & Conquer 3: Tiberium Wars | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Place in `/RetailExe/1.10`. Game crashes when changing settings. |
| Command & Conquer: Red Alert 3 | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Place in `/Data/` folder. |
| Command & Conquer 4: Tiberian Twilight | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Condemned: Criminal Origins | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Demigod | `Stereo-Injector/DX9/32-bit` | ✕ | Untested |  |
| Damnation | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Dark Messiah of Might and Magic | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Might need to use [Mod Launcher](https://steamcommunity.com/sharedfiles/filedetails/?id=3378556750). |
| Dark Void | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Dead or Alive 5 Last Round | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Deadpool | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix |
| Dead Space | `Stereo-Injector/DX9/32-bit` | ✓ | ⚠️ Partial | Crashes past Main Menu. |
| Dead Space 2 | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Dead Space 3 | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Mostly Working | Needs HUD Shader fix. |
| Defence Grid: The Awakening | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Delta Force: Xtreme | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Devil May Cry 3: Special Edition | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| DiRT | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Disciples III: Renaissance | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Divinity II: Ego Draconis  | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Dragon Age: Origins | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Drakensang: The Dark Eye | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Driver: San Francisco | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working |  |
| Dungeons (2011) | `Stereo-Injector/DX9/32-bit` | ✕ | Untested |  |
| Dungeon Siege II | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Dungeon Siege III | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Empire: Total War | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Enemy Engaged 2 | `Stereo-Injector/DX9/32-bit` | ✕ | Untested |  |
| Eragon | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Evolution GT | `Stereo-Injector/DX9/32-bit` | ✕ | Untested |  |
| Fable: The Lost Chapters | `Stereo-Injector/DX9/32-bit` | ✓ | ⚠️ Partial | Has artifacts, needs shader fix. |
| Fable III | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Fable Anniversary | `Stereo-Injector/DX9/32-bit` | ✕ | ⚠️ Partial | Has artifacts, needs shader fix. |
| Fahrenheit | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | aka Indigo Prophecy |
| Fallout 3 | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Fallout: New Vegas | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Mostly Working | Needs Skybox & Pip-Boy UI shader fix |
| F.E.A.R. | `Stereo-Injector/DX9/32-bit` | ✓ | ❌ Not Working | Crashes on launch. |
| F.E.A.R. Perseus Mandate | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| F.E.A.R. 2: Project Origin | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| FIFA 10 | `Stereo-Injector/DX9/32-bit` | ✕ | Untested |  |
| FIFA 14 | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working |  |
| Flashback (2013) | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | May need shader fix. |
| FlatOut | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| FlatOut 2 | `Stereo-Injector/DX9/32-bit` | ✕ | Untested |  |
| FlatOut: Ultimate Carnage | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Foreign Legion: Buckets of Blood | `Stereo-Injector/DX9/32-bit` | ✕ | Untested |  |
| Frontlines: Fuel of War | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Front Mission Evolved | `Stereo-Injector/DX9/32-bit` | ✕ | Untested |  |
| Fuel | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix |
| G-Force | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Garshasp: The Monster Slayer | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Ghostbusters: The Video Game | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Ghostbusters: Sanctum of Slime | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Google Earth | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | [Google Earth Pro 7.1.5.1557](https://web.archive.org/web/20171014110844/https://dl.google.com/earth/client/GE7/release_7_1_8/googleearth-win-pro-7.1.8.3036.exe) |
| Gothic 3 | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Mostly Working | Sky at screen depth, needs shader fix. |
| Grand Theft Auto: San Andreas | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| GRID | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Guitar Hero III: Legends of Rock | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Half-Life 2 | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Mostly Working | `steam_legacy` beta branch. Use `-game ` command line argument. Shadows have issues. |
| Halo: Combat Evolved | `Stereo-Injector/DX9/32-bit` | ✕ | ❌ Not Working | Crashes on load. |
| Heroes of Might and Magic V | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Hitman: Blood Money | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Needs shadow and water shader fix |
| Hunted: The Demon's Forge | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Injustice: Gods Among Us | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| Insane 2 | `Stereo-Injector/DX9/32-bit` | ✕ | ❌ Not Working |  |
| Jet Set Radio | `Stereo-Injector/DX9/32-bit` | ✕ | ❌ Not Working | Only works in mono. |
| Kane & Lynch 2: Dog Days | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Killing Floor | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| King Arthur: The Role-Playing Wargame | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | Specifies 'King Arthur: The Druids' expansion pack |
| King's Bounty: The Legend | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Left 4 Dead | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Only tested single player. Use `-insecure` command line argument to avoid VAC ban. |
| Left 4 Dead 2 | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Only tested single player. Use `-insecure` command line argument to avoid VAC ban. |
| Legacy of Kain: Defiance | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Needs shadow shader fix |
| Lego Star Wars: The Video Game | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Lego Star Wars III: The Clone Wars | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Madden NFL 08 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Mad Riders | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| Majesty 2: The Fantasy Kingdom Sim | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Mass Effect | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Mass Effect 2 | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Medal of Honor: Airborne | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Mercenaries 2: World in Flames | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Metal Gear Rising: Revengeance | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| Microsoft Flight Simulator 2004: A Century of Flight | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Mini Ninjas | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Mirror's Edge | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Monday Night Combat | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Mortal Kombat Komplete Edition | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working |  |
| MTX: Mototrax | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Mythos | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | Mythos (2009) maybe? Not sure which game this is. |
| Naruto Shippuden: Ultimate Ninja Storm 3 Full Burst (2013) | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Mostly Working |  Needs shader fix and swap eye. |
| Need for Speed: ProStreet | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Need for Speed: Undercover | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Need for Speed: Hot Pursuit (2010) | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Need for Speed: Shift | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Need for Speed: Shift 2 Unleashed | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Ninja Blade | `Stereo-Injector/DX9/32-bit` | ✓ | ❌ Not Working | Black screen on AMD. |
| Oddworld: New 'n' Tasty! | `Stereo-Injector/DX9/32-bit` | ✕ | ⚠️ Partial | Heavy artifacts, needs shader fix. |
| Operation Flashpoint: Dragon Rising | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| OutRun 2006: Coast 2 Coast | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Overlord | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Overlord II | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Painkiller | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Painkiller: Overdose | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Portal | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Use `-game Portal -dx9` command line argument. Place files in `/portal/bin/` folder. |
| Portal 2 | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Mostly Working | Place files in `/Portal 2/bin/` folder. Separation initially too high, needs wall display shader mod. |
| Portal Stories: Mel | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Needs particle glow, glass and portal shader fix. |
| Prince of Persia (2008) | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| Prince of Persia: The Forgotten Sands | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Pro Evolution Soccer 2017 | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| ProtoGalaxy | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Prototype 2 | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Mostly Working | Needs shadows shader fix. |
| Rayman Legends | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| RefRain - prism memories - | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Partially Working | Play with depth set low. Needs higher depth shader fix and issues with Simulated Reality output. |
| Richard Burns Rally | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Rise and Fall: Civilizations at War | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Rise of Flight | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Rise of the Argonauts | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Risen | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Saints Row 2 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Sam & Max Save the World | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | Notes Episode 4: Abe Lincoln Must Die! |
| Sam & Max: The Devil's Playhouse | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | Notes Episode 1: The Penal Zone |
| Samurai Warriors 2 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Sanctum | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Section 8 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Sega Rally Revo | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Needs shadow shader fix. |
| Serious Sam 2 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Sexy Beach 3 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Shadowgrounds | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Sid Meier's Civilization IV | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Sid Meier's Railroads! | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Silent Hunter 3 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Silent Hunter 4: Wolves of the Pacific | `Stereo-Injector/DX9/32-bit` | ✓ | ❌ Not Working | Crashes on startup. |
| Silent Hunter 5: Battle of the Atlantic | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Sine Mora EX | `Stereo-Injector/DX9/32-bit` | ✕ | ❌ Not Working |  |
| Singularity | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Sins of a Solar Empire | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Mostly Working | Background at wrong depth. |
| SkyDrift | `Stereo-Injector/DX9/32-bit` | ✕ | ❌ Not Working |  |
| Sonic Adventure 2 | `Stereo-Injector/DX9/32-bit` | ✕ | ❌ Not Working | Black screen |
| Sonic & Sega All-Stars Racing | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working |  |
| Sonic R (2004) | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Mostly Working | Needs coin depth shader fix. Use with [Sonic R Updater](https://github.com/cheatfreak47/SRUpdater). |
| Spec Ops: The Line | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working |  |
| Split/Second: Velocity | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Mostly Working | Needs shader fix. Aka Split/Second. |
| Spore | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| S.T.A.L.K.E.R.: Shadow of Chernobyl | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Star Trek: Legacy | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Star Wars: Battlefront (2004) | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Star Wars: Battlefront II (2005) | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Star Wars: Episode I - Racer | `Stereo-Injector/DX9/32-bit` | ✕ | ❌ Not Working | Crashes on startup. |
| Star Wars: The Force Unleashed II | `Stereo-Injector/DX9/32-bit` | ✕ | ⚠️ Partial | Needs shader fix. |
| Starship Troopers | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Street Fighter IV | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Supreme Commander | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Supreme Commander: Forged Alliance | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Supreme Commander 2 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Test Drive Unlimited | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| The Ball | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Mostly Working | Ball shadow different in both eyes. |
| The Chronicles of Narnia: The Lion, the Witch and the Wardrobe | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | (Guess based on 'Narnia' and 'Narnia.exe') |
| The Elder Scrolls IV: Oblivion | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| The Elder Scrolls V: Skyrim | `Stereo-Injector/DX9/32-bit` | ✕ | ❌ Not Working | Enters main menu, freezes on level load. |
| The Lord of the Rings: The Battle for Middle-earth | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Mostly Working | Needs UI shader fix. |
| The Movies (2005) | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| The Settlers II: 10th Anniversary | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| The Sims 2: University | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | Targets "Sims2EP1.exe", might just be expansion pack or all of The Sims 2. |
| The Sims 3 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| The Sims Medieval | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| The Witcher | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Place files in `/System/` folder. |
| TimeShift | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Titan Quest | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Mostly Working | Needs shadow shader fix. |
| TOCA Race Driver 3 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Tom Clancy's Ghost Recon Advanced Warfighter 2 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Tom Clancy's Rainbow Six: Vegas | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Tom Clancy's Rainbow Six: Vegas 2 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Tom Clancy's Splinter Cell: Double Agent | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Tom Clancy's Splinter Cell: Conviction | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Mostly Working | Needs shader fix. |
| Tomb Raider: Legend | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Mostly Working | Needs shadow shader fix. |
| Tomb Raider: Anniversary | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Tomb Raider: Underworld | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Torchlight | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| TrackMania Nations Forever | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Trine (2009) | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Two Worlds | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Unreal Tournament 3 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Valkyria Chronicles | `Stereo-Injector/DX9/32-bit` or `64-bit` | ✕ | ⚠️ Partial | Needs UI, reflections and shadow shader fix |
| Vanquish | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Mostly Working | Needs crosshair shader fix. |
| Virtua Tennis 2009 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Virtua Tennis 4 | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix |
| Wallace & Gromit's Grand Adventures | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | Episodes 1-4 |
| Wanted: Weapons of Fate | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Warhammer 40,000: Dawn of War | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Warhammer 40,000: Dawn of War: Dark Crusade | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Warhammer 40,000: Dawn of War: Soulstorm | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Warhammer 40,000: Dawn of War II | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Watchmen: The End is Nigh | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Wheelman | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | May need shader fix. |
| Wings of Prey | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Wolfenstein (2009) | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| WorldShift | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| X-Men Origins: Wolverine | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working |  |
| Yaiba: Ninja Gaiden Z | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working |  |
| Zeno Clash | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |

- **Excluded (Game Not Playable):** *Darkspore*. <sub>(Game not playable til [Resurrection Capsule](https://github.com/vitor251093/resurrection-capsule) project completes.)</sub>
- **Excluded (Online Ban Risk):** *Allods Online*, *Dark Age of Camelot*, *Darkfall Online*, *Fury (2007)*, *Global Agenda*, *Guild Wars*, *Monster Hunter Frontier Online*, *Warhammer Online: Age of Reckoning*.

### DirectX 9 Games (64-bit)

| Game | Install Path | iZ3D Profile | Testing | Notes |
|------|--------------|--------------|---------|-------|
| Chess Titans | `Stereo-Injector/DX9/64-bit` | ✕ | Untested | Freezes on level load. |
| Dragon Ball Xenoverse | `Stereo-Injector/DX9/64-bit` | ✕ | ✅ Working | May need shader fix. |
| Far Cry | `Stereo-Injector/DX9/32-bit` or `64-bit` | ✓ | ❌ Not Working | Crashes on startup. |
| Final Fantasy IX | `Stereo-Injector/DX9/64-bit` | ✕ | ⚠️ Partial | Dual-View Output works, flat 3D though. |
| Pro Evolution Soccer 2016 | `Stereo-Injector/DX9/64-bit` | ✕ | ✅ Working |  |
| REFLEX XTR² | `Stereo-Injector/DX9/32-bit` or `64-bit` | ✓ | Untested |  |
| Tales of Berseria | `Stereo-Injector/DX9/64-bit` | ✕ | ✅ Mostly Working | Not working in SR Weave output, working in other 3D outputs. |
| Ultimate Marvel vs. Capcom 3 | `Stereo-Injector/DX9/64-bit` | ✕ | ❌ Not Working |  |
| Unreal Tournament 2004 | `Stereo-Injector/DX9/64-bit` | ✓ | ✅ Working |  |

- **Excluded (VAC Ban Risk):** *Counter-Strike: Source*, *Day of Defeat: Source*, *Half-Life 2: Deathmatch*.
- **Excluded (Online Ban Risk):** *Dungeons & Dragons Online*, *EVE Online*, *EverQuest*, *EverQuest 2*, *Flyff (Fly For Fun)*, *RIFT*, *Starcraft II*.

### DirectX 7 & 8 Games

| Game | Install Path | iZ3D Profile | Testing | Notes |
|------|--------------|--------------|---------|-------|
| AquaNox | `Stereo-Injector/DX8` | ✕ | ✅ Working |  |
| Ballance | `Stereo-Injector/DX8` | ✓ | Untested |  |
| Command & Conquer: Generals | `Stereo-Injector/DX8` or `DX9/32-bit` | ✓ | ⚠️ Partial | Single player only. Partially works on DX9 with Tiberian Technologies patch, but needs shaderfix. |
| Command & Conquer: Renegade | `Stereo-Injector/DX8` | ✓ | Untested | Single player only. |
| Deus Ex | `Stereo-Injector/DX7` | ✕ | ❌ Not Working | Listed for DX7 testing. |
| Deus Ex: Invisible War | `Stereo-Injector/DX8` | ✕ | ⚠️ Partial | Starts up, gets into menu on Linux. |
| Duke Nukem: Manhattan Project | `Stereo-Injector/DX8` | ✕ | ✅ Working | May need convergence and separation adjusting for best settings. |
| FATE | `Stereo-Injector/DX8` | ✕ | Untested |  |
| Mega Man X8 | `Stereo-Injector/DX8` | ✓ | Untested |  |
| Mercedes-Benz World Racing | `Stereo-Injector/DX8` | ✓ | Untested |  |
| Sniper Elite | `Stereo-Injector/DX8` | ✓ | ❌ Not Working | Game crashes. |
| The Lord of the Rings: The Return of the King | `Stereo-Injector/DX8` | ✓ | Untested |  |
| Thief: Deadly Shadows | `Stereo-Injector/DX8` | ✓ | ⚠️ Partial | In-game, also heavy artifacts |
| Tony Hawk's Pro Skater 3 | `Stereo-Injector/DX8` | ✓ | Untested |  |

- **Excluded (Online Ban Risk):** *Empire Earth II*, *Freelancer*, *GTR - FIA GT Racing Game*, *NASCAR Racing 2003 Season*, *Tom Clancy's Rainbow Six 3: Raven Shield*. <sub>(Active community servers with stringent anti-cheat)</sub>

### Nvidia 3D Vision "Automatic Mode" Games 
Games that used 3D Vision "Automatic Mode" driver stereo injection, but communicate with 3D Vision via in-game settings, built-in shader fixes, or config options. For our release, these use wiz3D's stereo injector instead, with the aim of making any shader fixes and separation/convergence sliders compatible with our injector. 0.4.0 wires the NVAPI stereo bridge on DX9 and DX11, so in-game sliders now move wiz3D's own stereo state.

| Game | Install Path | iZ3D Profile | Testing | Notes |
|------|--------------|--------------|---------|-------|
| Alice: Madness Returns | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Mostly Working | `AllowNvidiaStereo3d=True` in `AliceEngine.ini`. Needs windows and shadow shader fix. |
| Assassin's Creed: Revelations | `Stereo-Injector/DX9/32-bit` | ✕ | Untested | `3D Vision Fog` in game settings |
| Batman: Arkham Asylum | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Enables `Nvidia Stereoscopic 3D` in game setting. |
| Batman: Arkham City | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Partially Working | DX9 working, DX11 not working. Might need to implement more 3D Vision features. |
| Batman: Arkham Origins | `Stereo-Injector/DX9/32-bit` | ✕ | ❌ Not Working | DX9 needs `-force-d3d9`. Not working in DX9 or DX10. |
| Battlefield: Bad Company 2 | `Stereo-Injector/DX10-11/32-bit` | ✓ | ✅ Mostly Working | DX11. Incorrect skybox. Including 'Vietnam' Expansion Pack. |
| Brave: The Video Game | `Stereo-Injector/DX9/32-bit` | ✕ | Untested |  |
| Call of Duty: Black Ops | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Might need to implement more 3D Vision features. |
| Carrier Command: Gaea Mission | `Stereo-Injector/DX9/32-bit` | ✕ | Untested |  |
| Dead Rising 2 | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Mostly Working | Needs mirrors and fade-out zombies shader fix. |
| Deep Black: Reloaded | `Stereo-Injector/DX9/32-bit` | ✕ | Untested |  |
| Depth Hunter | `Stereo-Injector/DX9/32-bit` | ✕ | Untested |  |
| Devil May Cry 4 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | `Stereo=ON` in `config.ini`. |
| Devil May Cry 4 Special Edition | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | `Stereo=ON` in `config.ini`. |
| Dragon's Dogma: Dark Arisen | `Stereo-Injector/DX9/32-bit` | ✕ | Untested | `Stereo=ON` in `config.ini`. |
| Duke Nukem Forever | `Stereo-Injector/DX9/32-bit` | ✕ | Untested |  |
| Grand Theft Auto IV | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Mostly Working | `-stereo` command line argument.<br>Needs GTA 4 shadows shader fix. |
| Grand Theft Auto V (Legacy) | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working | GTA 5 Story mode only, `-nobattleye` command line argument. PCSS shadows suggested. |
| GT Legends | `Stereo-Injector/DX9/32-bit` | ✕ | Untested |  |
| Inversion | `Stereo-Injector/DX9/32-bit` | ✕ | ⚠️ Partial | Might need to implement more 3D Vision features or shader fix. |
| Just Cause 2 | `Stereo-Injector/DX10-11/32-bit` | ✓ | Untested |  |
| L.A. Noire | `Stereo-Injector/DX9/32-bit` | ✕ | Untested | Game tells Automatic Mode what depth to use per frame. |
| Lost Planet 2 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | `Stereo=ON` in `config.ini`. |
| Mafia II | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Max Payne 3 | `Stereo-Injector/DX10-11/32-bit` | ✕ | ✅ Working | DX11. `-stereo 1` command line argument. |
| Medal of Honor (2010) | `Stereo-Injector/DX9/32-bit` | ✓ | Untested | Use Single Player only. |
| Metro 2033 | `Stereo-Injector/DX10-11/32-bit` | ✓ | ✅ Mostly Working | Can't see weapon in hand. |
| Metro: Last Light | `Stereo-Injector/DX10-11/32-bit` | ✕ | Untested | Lists 3D Vision support in [Official PC Requirements](https://www.reddit.com/r/Games/comments/1cjh4l/metro_last_light_official_pc_requirements/). |
| [Orbiter Space Flight Simulator](https://github.com/orbitersim/orbiter) | `Stereo-Injector/DX9/64-bit` | ✕ | Untested | `Stereoscopic 3D` in settings. |
| Resident Evil 5 | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | `Stereo=ON` in `config.ini`.<br>DX9 only on Steam, only tested DX9. |
| Resident Evil 6 | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | `Stereo=ON` in `config.ini`. Might need shader fix. |
| rFactor 2 | `Stereo-Injector/DX9/64-bit` | ✕ | Untested | Use Single-player only. |
| Roller Coaster Rampage | `Stereo-Injector/DX9/32-bit` | ✕ | Untested |  |
| Sid Meier's Civilization V | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| Super Street Fighter IV Arcade Edition | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Might need to implement more 3D Vision features. |
| Street Fighter X Tekken | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need to implement more 3D Vision features and shader fixes. |
| Tom Clancy's H.A.W.X 2 | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |
| The Witcher 2: Assassins of Kings | `Stereo-Injector/DX9/32-bit` | ✓ | Untested |  |

- **Excluded (Native SBS/TAB):** *Deus Ex: Mankind Divided*, *DOOM 3: BFG Edition*, *Avatar: The Game*, *Sonic Generations*. 
- **Excluded (Native AMD HD3D):** *Battlefield 3*, *DiRT 2*, *DiRT 3*, *DiRT Showdown*, *DiRT Rally*, *GRID 2*, *GRID Autosport*, *Tomb Raider (2013)*. 
- **Excluded (Online Ban Risk):** *Aion: The Tower of Eternity*, *Diablo III*, *Hawken*, *Pirate101*, *Rusty Hearts*, *Wizard101*, *StarCraft II*.
- **Excluded (Demo or Benchmark):** *Aliens vs. Triangles*, *Endless City*, *Stone Giant*, *Supersonic Sled*, *Passion Leads Army Benchmark*, *Unigine: Heaven Benchmark*.

---

## Keyboard Shortcuts

In-game hotkeys for adjusting stereo on the fly. All bindings are remappable in `wiz3D_Config.xml` under `<HotKeyList>`, see the header comment in that file for the key-code format.

| Key | Action |
| --- | ------ |
| `Numpad *` | Toggle stereo 3D on/off |
| `Numpad +` | Increase separation (stereo depth / eye distance) |
| `Numpad -` | Decrease separation (stereo depth / eye distance) |
| `Shift+Numpad +` | Increase convergence (push parallax plane further away) |
| `Shift+Numpad -` | Decrease convergence (pull parallax plane closer) |
| `Numpad /` | Toggle auto-focus (auto-adjusts convergence based on scene depth) |
| `Numpad 7` / `8` / `9` | Switch between three saved stereo presets |
| `Ctrl+F8` | Swap left/right eyes (for inverted setups) |
| `Shift+Numpad *` | Open the on-screen wizard overlay |
| `Shift+F1` | Toggle on-screen hotkey hints |
| `Ctrl+F1` | Toggle status strip + FPS counter |
| `Ctrl+Numpad /` | Toggle laser sight (crosshair anchored at screen depth) |
| `Numpad 0` | **ShaderHunter**: enter / exit hunt mode (DX9 + DX11) — see [Profile Building & Shader Fixes](#profile-building--shader-fixes) |
| `Numpad 1` / `Numpad 2` | Hunt-mode only: cycle to previous / next drawn shader |
| `Numpad 3` | Hunt-mode only: capture the currently hidden shader to `wiz3D_Shaders\<Game>\Hunter\` |
| `Ctrl+F5` | Reload `BaseProfile.xml` live (apply profile edits without restarting the game) |
| `PrintScreen` | Save a stereo screenshot to `My Pictures\wiz3D Screenshots\<Game>\` (default format `.png`; pick PNG/PNS/JPEG/JPS/BMP/DDS via `<ScreenshotType>` in `wiz3D_Config.xml`) |
| `Ctrl+PrintScreen` | Save a stereo screenshot without applying the display's gamma ramp |

---

## Profile Building & Shader Fixes

Most games just work with wiz3D's built-in per-engine profiles (`BaseProfile.xml` ships with coverage for UE3/UE4, CryEngine, RAGE, Dunia, Snowdrop, idTech 4/5, and 15+ others). When a game *mostly* works but a specific effect is wrong — HUD at the wrong depth, broken shadows, haloed decals, a reticle floating in space — you can fix just that effect without touching the rest of the game.

The fix lives in your copy of `BaseProfile.xml` next to the game `.exe`, so it's game-local, text-editable, and reversible.

### Quick workflow — DX11

1. **Launch the game.** Get to the scene with the broken effect.
2. **Press `Numpad 0`** to enter ShaderHunter mode. A `HUNT` strip appears at the bottom of the screen.
3. **Press `Numpad 1` / `Numpad 2`** to cycle through the shaders used this frame. Each shader is *hidden* (its draws are skipped) one at a time — when the broken effect disappears, you've found the shader responsible.
4. **Press `Numpad 3`** to capture that shader. A `Shader saved` toast appears bottom-right with the filename. The report lands at `wiz3D_Shaders\<Game>\Hunter\0x<CRC>_<stage>_<timestamp>.txt` and contains the shader disassembly, matrix-CB hints, and a suggested `<ShaderPatch>` / `<Mono>` skeleton you can paste into `BaseProfile.xml`.
5. **Press `Numpad 0`** again to leave hunt mode.
6. **Edit `BaseProfile.xml`** next to the game `.exe`. Drop the suggested skeleton inside the game's `<Profile>` block. The simplest fix is `<SkipStereo/>` — mono-force that shader so the broken effect stops shifting between eyes.
7. **Press `Ctrl+F5`** in-game to reload the profile. No restart needed.
8. **Iterate.** When happy, open a GitHub issue or PR with your profile addition so other players benefit.

### Fix types, cheapest first

**`<SkipStereo/>` — mono-force one shader.** The 90% case: a HUD element or 2D overlay that should sit at screen depth. One line inside a `<ShaderPatch CRC="0x...">` block.

```xml
<ShaderPatch CRC="0x1D9628E2" Stage="VS" Note="Minimap compass">
    <SkipStereo/>
</ShaderPatch>
```

**CB-math features — the big ones.** The engine's constant buffers already contain the view/projection matrices; wiz3D edits a few bytes before the right eye's draws so untouched game shaders naturally render per-eye. 37 declarative features cover shadow projection, deferred lights, screen-space rays, HUD depth, inverse-matrix cameras, UAV eye-duplication, etc. These are tagged in `BaseProfile.xml` under each `<Profile>` and documented inline in `S3DWrapper10/Feature*.cpp`.

**Inline ShaderOverride — hand-write HLSL.** For the shader-level cases a CB edit can't reach (custom depth outputs, loose scalar transforms, HUDs that bypass CBs). Paste HLSL inside the profile as a `CDATA` block; wiz3D compiles it at startup and swaps the game's bytecode. Sample the stereo-params SRV at `t125` (HelixMod / 3DMigoto convention) to apply your own per-eye offset:

```xml
<ShaderOverride CRC="0x7BCB56E1" Stage="VS"><![CDATA[
    // ...your HLSL...
    Texture2D<float4> stereo : register(t125);
    // ...sample at (0,0) for (separation, convergence, eye_sign, 0)...
]]></ShaderOverride>
```

### DX9

The DX9 wrapper has the same ShaderHunter + reload workflow (same hotkeys). Fix authoring uses the iZ3D-native declarative XML in `BaseProfile.xml`: `<Mono>` for shader-level stereo-off, `<Multiplier>` / `<ConvergenceShift>` for per-CRC stereo-param overrides, and `<Matrices>` for projection-matrix identification. See `docs/SHADER_FIXES.md` for the full technical breakdown.

### Mass-dump option

If you want every shader the game creates (not just the ones you hunt), flip `<DumpVS Value="1"/>` and `<DumpPS Value="1"/>` in `wiz3D_Config.xml`. Dumps land in `wiz3D_Shaders\<Game>\Dump\`. Useful when you have a known-good HelixMod fix from another wrapper and want to CRC-match the shaders yourself.

Developer shortcut: set the `WIZ3D_FORCE_DUMP=1` environment variable to auto-enable `DumpVS` + `DumpPS` across every game without having to flip the Config.xml knob per install — intended for local authoring workflows, never set in release/CI environments.

### Submitting fixes

Open a GitHub issue or PR with the game name, your `<Profile>` XML block, and a short note on what it fixes. Profiles merged into `BaseProfile.xml` ship for every player on the next release. By submitting, you agree to LGPL-2.1 for your profile content (the same licence the rest of the project uses).

---

## Building from Source

### Prerequisites

- **Visual Studio 2026** with the "Desktop development with C++" workload (v145 toolset)
- **Windows 11 SDK** (10.0.26100 or later)
- Git

### External SDKs (not included, download separately)

Several SDKs are excluded from the repository due to licensing or size. Download them and place them at the paths shown before building:

| SDK | Path in repo | Where to get it |
|-----|-------------|-----------------|
| Immersity LeiaSR SDK Win64 v1.36.2 + Win32 v1.34.10 | `lib/Simulated Reality/` | [https://support.immersity.ai/sdk/](https://support.immersity.ai/sdk/) and [Samsung Odyssey 3D Hub Installer](https://www.samsung.com/uk/support/model/LS27FG900XUXXU/#downloads) |
| NVAPI SDK | `lib/NVAPI/` and `lib/nvapi_2026/` | [https://github.com/NVIDIA/nvapi](https://github.com/NVIDIA/nvapi) |
| Boost 1.87 | `lib/boost/` | [https://www.boost.org/releases/1.87.0/](https://www.boost.org/releases/1.87.0/) |

`lib/PerfSDK/` is a deprecated NVIDIA proprietary SDK; all code referencing it is `#if 0`'d out, so you don't need it to build.

### Solutions

There are two solutions to build:

| Solution | What it builds |
|----------|---------------|
| `S3DDriver.sln` | All stereo wrapper DLLs, output method DLLs, NvApiProxy, vendor proxies |
| `wiz3D-proxy/wiz3D-proxy.sln` | Entry-point proxy loaders: `d3d8.dll`, `d3d9.dll`, `d3d10.dll`, `d3d11.dll`, `d3d12.dll`, `ddraw.dll`, `dxgi.dll`, `opengl32.dll`, `vulkan-1.dll` |

Build both in **Release \| Win32** for 32-bit games, or **Release \| x64** for 64-bit games.

#### One-shot build + deploy (recommended)

Run from the repo root in PowerShell:

```powershell
.\bin\build_and_deploy.ps1                # both archs, both solutions
.\bin\build_and_deploy.ps1 -Arch Win32    # x86 only
.\bin\build_and_deploy.ps1 -Arch x64      # x64 only
.\bin\build_and_deploy.ps1 -SkipBuild     # deploy only (after a manual VS build)
.\bin\build_and_deploy.ps1 -SkipProxy     # only build S3DDriver.sln
```

This builds both solutions and copies every freshly-built DLL into the right `releases/wiz3D/<api>/<arch>/` subfolder for you. If you'd rather drive MSBuild yourself (or use VS's F7), run `.\bin\deploy_to_releases.ps1` afterwards to populate the release tree.

### Testing a Build

1. Build both solutions (or run `.\bin\build_and_deploy.ps1`).
2. Copy the **contents** of the appropriate `releases/wiz3D/` subfolder (e.g. `Stereo-Injector/DX9/32-bit/`) directly into the game's `.exe` folder.
3. Set `wiz3D_Config.xml` or `HD3D_Config.xml` to match your display specifications
4. For some games (L4D2, Portal, HL2), copy into the `bin/` subfolder instead.
5. Launch the game. Stereo should activate automatically.
6. Toggle stereo on and off using `Numpad *`.

---

## Contributing

Contributions are very welcome. Key areas where help is most needed:

1. **Game testing**, test with DX7-11 and OpenGL games and add results to the tables above.
2. **DX10 completion**, the DX10 vendor proxy builds in 0.4.0 but has no per-game analyser coverage; porting the DX11 profiles across is a high-impact effort.
3. **DX7/8 stereo**, basic wrappers pass through to DX9 but stereo initialisation isn't working on every title yet.
4. **OpenGL stereo**, current OpenGL support is quad-buffer passthrough only; a mono-GL injector is on the roadmap.
5. **Output modes**, test the various stereo output plugins (SBS, anaglyph, interlaced, SR Weave, shutter).

Please open an issue before starting large changes. For game test results, edit the tables in `README.md` directly and open a PR.

---

## License & Commercial Use

The original legacy iZ3D code included in this repository remains under its original **MIT License**.

All new modifications, proxy DLLs, hooks, and modernisations introduced by the **wiz3D** project are licensed under the **GNU Lesser General Public License v2.1 (LGPLv2.1)** (see `LICENSE`).

**What this means for the community:** wiz3D is free for gamers, modders, and the open-source community to use, modify, and distribute. Thanks to the LGPLv2.1, this wrapper can be legally and safely injected into proprietary, closed-source games. However, any modifications made directly to the wiz3D wrapper codebase itself must remain open-source and be shared back with the community under the same license.
