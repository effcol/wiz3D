# 3D Vision Direct (NVIDIA)

✅ **Mostly Working** _(expanded in 0.4.0)_. The NVIDIA native-stereo path for games that render both eyes themselves via NVAPI "Direct Mode". wiz3D's NvApi vendor proxy (NvApiProxy) exposes a stand-in NVAPI surface, captures the game's per-eye output, and composites to your chosen output mode. 0.4.0 added DX9 MinHook migration, SR Weave port, `NativeProfile.xml` cross-session persistence, and the DX11 NVAPI stereo bridge (so in-game 3D sliders drive wiz3D's own separation / convergence).

## "Direct Mode" ≠ shutter output

**Important**: this path does **not** drive shutter glasses on its own. It's a per-eye capture + 2D compositor. For actual NVIDIA 3D Vision shutter output on modern Windows, pick `OutputMode="3DVisionGlasses"` in `wiz3D_Config.xml` — that routes through [NV3D-Lib](https://github.com/oneup03/NV3D-Lib) (statically linked into every output-producing wrapper from 0.5.0) and needs either a legacy R417-or-earlier NVIDIA driver *or* Oscar's [Vision Restoration](https://github.com/oscardelatorre/VisionRestoration) patch on modern drivers. The two cooperate: 3D Vision Direct captures the per-eye content, `3DVisionGlasses` output mode drives the shutter signal.

## If your game isn't in the list below

Many 2010-2018 titles supported **NVIDIA 3D Vision "Automatic Mode"** (the driver injected stereo without the game knowing), not Direct Mode. These games communicate with 3D Vision via in-game settings / config options / built-in shader fixes, but their renderer is otherwise mono. For those, use the [Stereo-Injector](API-DirectX-9) route — specifically the [3D Vision Automatic-aware games](Injector-3D-Vision-Automatic-Aware) page, which lists the ~40 known titles plus the config-file knobs each one needs (`-stereo` cmdline, `Stereo=ON` in config.ini, `AllowNvidiaStereo3d=True`, etc.). 0.4.0 wired the NVAPI stereo bridge on DX9 and DX11 so in-game 3D sliders in those titles move wiz3D's own stereo state.

## Install path

Under `3D-Vision-Direct/DX9/{32,64}-bit/`, `3D-Vision-Direct/DX10/{32,64}-bit/`, `3D-Vision-Direct/DX11/{32,64}-bit/`. Runs alongside a Stereo-Injector install and only activates for games listed in `NativeProfile.xml`.

The DX11 x86 subfolder has an `EOSStub/` variant for titles that bundle Epic Online Services and need EOS disabled for SR Weave cohabitation — see Tomb Raider (2013) below.

## Games

| Game | Install Path | Testing | Notes |
|---|---|---|---|
| Hard Reset | `3D-Vision-Direct/DX9/32-bit` | ✅ Working | Game must be set to 120Hz. |
| Hitman: Absolution | `3D-Vision-Direct/DX11/32-bit` | ✅ Working | Also supports HD3D. |
| Oil Rush | `3D-Vision-Direct/DX11/32-bit` | ✅ Working | DX11 output only. |
| Tomb Raider (2013) | `3D-Vision-Direct/DX11/32-bit` | 🟡 Mostly | Also supports HD3D. Use the `EOSStub/` subfolder if you need Epic Online Services disabled for SR Weave cohabitation. |

Plus the shipped `NativeProfile.xml` ships entries confirmed-good for: Max Payne 3, Alan Wake, Hard Reset, Tomb Raider (2013), BioShock Infinite, Batman: Arkham City, UDK (generic). Those cover both the Direct Mode capture path and the NvDirectMode compositor.

**Excluded (Depth Map not loading, better support on HD3D):** *Battlefield 3*, *DiRT 2*, *DiRT 3*, *DiRT 3 Complete Edition*, *DiRT Showdown*, *DiRT Rally*, *F1 2010*, *F1 2011*, *F1 2012*, *F1 2013*, *GRID 2*, *GRID Autosport*.

**Excluded (Works on HD3D and 3D Vision Direct Mode, HD3D recommended):** *World of Warcraft*. <sub>(Stereo3D removed in 2018 DX12 update.)</sub>

## See also

- [3D Vision Automatic-aware games](Injector-3D-Vision-Automatic-Aware) — for titles that used 3D Vision Automatic (not Direct) — these use the Stereo-Injector instead
- [HD3D](Native-HD3D) — AMD's counterpart native-stereo path
- [Output Modes](Output-Modes#3dvisionglasses) — `3DVisionGlasses` mode for the shutter signal
