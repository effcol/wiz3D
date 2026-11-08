# DirectX 7

⚠️ **In Progress.** The DX7 wrapper (`Stereo-Injector/DX7/`) converts DirectDraw / Direct3D 7 calls to the DX9 stereoisation pipeline. The DX7→DX9 bootstrap works and the wrapper is present for testing; broader compatibility sweeps are open to the community.

## How the wrapper works

`S3DWrapper7` presents a full Direct3D 7 proxy surface (DirectDraw7, DirectDrawClipper, DirectDrawPalette, Direct3DVertexBuffer7, Direct3DDevice7, …) and, internally, boots a DX9 renderer via `Direct3DCreate9()` (`S3DWrapper7/Direct3D7.cpp:54`). Every DX7 draw is translated into DX9 calls, and the standard DX9 stereo-injection / OSD / hotkey / output-mode pipeline runs unchanged — so anything that works in [DirectX 9](API-DirectX-9) works here once the translation is clean.

## Known issues

- 16-bit back-buffer games on Windows 10/11 can trip the R5G6B5 mapping path. Fix is deferred; please report with log file if you hit it.

## Install path

`Stereo-Injector/DX7/`. Copy the contents (DLLs + `OutputMethods/` + `BaseProfile.xml` + `wiz3D_Config.xml`) next to the game's `.exe`. No 64-bit DX7 subfolder — DX7 titles are 32-bit.

## Games

| Game | Install Path | iZ3D Profile | Testing | Notes |
|---|---|---|---|---|
| Deus Ex | `Stereo-Injector/DX7` | ✕ | 🔴 Not Working | Listed for DX7 testing. |

Please report additional DX7 titles via [GitHub Issues](https://github.com/effcol/WiZ3D/issues).
