# DirectX 8

✅ **Partially Working.** The DX8 wrapper (`Stereo-Injector/DX8/`) converts Direct3D 8 calls to the DX9 stereoisation pipeline. Known-good on a handful of titles; broader coverage is an open contribution area.

## How the wrapper works

`S3DWrapper8` exposes a full Direct3D 8 proxy surface (Direct3D8, Direct3DBaseTexture8, Direct3DCubeTexture8, Direct3DIndexBuffer8, Direct3DVertexBuffer8, Direct3DDevice8, …) and internally boots a DX9 renderer via `Direct3DCreate9()` (`S3DWrapper8/S3DWrapper8.cpp:432`). Every DX8 draw translates to DX9 calls, so the whole DX9 stereo / OSD / hotkeys / output-mode pipeline runs unchanged.

## Known issues (historical, fixed)

- `BuildAdapterModesList` R5G6B5-seed bug broke adapter enumeration — fixed in 0.4.x.
- Logging path emits `[wrap]` lines regardless of release build (dev aid; not harmful).

## Install path

`Stereo-Injector/DX8/`. Copy the contents next to the game's `.exe`. DX8 titles are 32-bit.

## Games

| Game | Install Path | iZ3D Profile | Testing | Notes |
|---|---|---|---|---|
| AquaNox | `Stereo-Injector/DX8` | ✕ | ✅ Working |  |
| Ballance | `Stereo-Injector/DX8` | ✓ | 🧪 Untested |  |
| Command & Conquer: Generals | `Stereo-Injector/DX8` or `DX9/32-bit` | ✓ | 🟡 Partial | Single-player only. Partially works on DX9 with Tiberian Technologies patch, but needs shader fix. |
| Command & Conquer: Renegade | `Stereo-Injector/DX8` | ✓ | 🧪 Untested | Single-player only. |
| Deus Ex: Invisible War | `Stereo-Injector/DX8` | ✕ | 🟡 Partial | Starts up, gets into menu on Linux. |
| Duke Nukem: Manhattan Project | `Stereo-Injector/DX8` | ✕ | ✅ Working | May need convergence and separation adjusting for best settings. |
| FATE | `Stereo-Injector/DX8` | ✕ | 🧪 Untested |  |
| Mega Man X8 | `Stereo-Injector/DX8` | ✓ | 🧪 Untested |  |
| Mercedes-Benz World Racing | `Stereo-Injector/DX8` | ✓ | 🧪 Untested |  |
| Sniper Elite | `Stereo-Injector/DX8` | ✓ | 🔴 Not Working | Game crashes. |
| The Lord of the Rings: The Return of the King | `Stereo-Injector/DX8` | ✓ | 🧪 Untested |  |
| Thief: Deadly Shadows | `Stereo-Injector/DX8` | ✓ | 🟡 Partial | In-game, also heavy artifacts. |
| Tony Hawk's Pro Skater 3 | `Stereo-Injector/DX8` | ✓ | 🧪 Untested |  |

**Excluded (Online Ban Risk):** *Empire Earth II*, *Freelancer*, *GTR - FIA GT Racing Game*, *NASCAR Racing 2003 Season*, *Tom Clancy's Rainbow Six 3: Raven Shield*. <sub>(Active community servers with stringent anti-cheat.)</sub>
