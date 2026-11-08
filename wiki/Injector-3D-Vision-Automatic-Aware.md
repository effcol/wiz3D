# 3D Vision Automatic-aware games

These are games that used NVIDIA 3D Vision "Automatic Mode" driver stereo injection, but which communicate with 3D Vision via in-game settings, built-in shader fixes, or config options. For our release, **they use wiz3D's Stereo-Injector**, not the native-stereo [3D Vision Direct](Native-3D-Vision-Direct) path — because the game's renderer is otherwise mono; the 3D Vision driver was the thing generating stereo, and we do the same shape of injection.

0.4.0 wired the **NVAPI stereo bridge** on DX9 and DX11, so in-game sliders (separation / convergence / 3D depth) now move wiz3D's own stereo state. Any shader fixes the game shipped for 3D Vision usually still apply unchanged.

## What this means in practice

For many of these titles you need to flip a specific flag to tell the game to render in 3D-Vision-aware mode, so the shader fixes and NVAPI calls kick in. Common patterns:

- `-stereo` / `-stereo 1` command-line argument (GTA IV, Max Payne 3).
- `Stereo=ON` in a `config.ini` (Dead Space 2/3, Devil May Cry 4, Dragon's Dogma, Lost Planet 2, RE5, RE6).
- In-game menu toggle labelled "Nvidia Stereoscopic 3D" / "Stereoscopic 3D" / "3D Vision Fog" (Batman: Arkham Asylum, Assassin's Creed: Revelations, Orbiter).
- Engine config flag like `AllowNvidiaStereo3d=True` in `AliceEngine.ini` (Alice: Madness Returns).

See each row's **Notes** column for the specific flag.

## Install path

Depends on the game's API — see each row. Most 3D-Vision-Automatic-era titles are DX9, some are DX10/11.

## Games

| Game | Install Path | iZ3D Profile | Testing | Notes |
|---|---|---|---|---|
| Alice: Madness Returns | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Mostly | `AllowNvidiaStereo3d=True` in `AliceEngine.ini`. Needs windows + shadow shader fix. |
| Assassin's Creed: Revelations | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested | "3D Vision Fog" in-game setting. |
| Batman: Arkham Asylum | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Enable "Nvidia Stereoscopic 3D" in-game. |
| Batman: Arkham City | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Partial | DX9 working, DX11 not working. Might need more 3D Vision features. |
| Batman: Arkham Origins | `Stereo-Injector/DX9/32-bit` | ✕ | 🔴 Not Working | DX9 needs `-force-d3d9`. Not working in DX9 or DX10. |
| Battlefield: Bad Company 2 | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🟡 Mostly | DX11. Incorrect skybox. Includes Vietnam expansion. |
| Brave: The Video Game | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested |  |
| Call of Duty: Black Ops | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Might need more 3D Vision features. |
| Carrier Command: Gaea Mission | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested |  |
| Dead Rising 2 | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Mostly | Needs mirrors + fade-out zombies shader fix. |
| Deep Black: Reloaded | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested |  |
| Depth Hunter | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested |  |
| Devil May Cry 4 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | `Stereo=ON` in `config.ini`. |
| Devil May Cry 4 Special Edition | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | `Stereo=ON` in `config.ini`. |
| Dragon's Dogma: Dark Arisen | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested | `Stereo=ON` in `config.ini`. |
| Duke Nukem Forever | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested |  |
| Grand Theft Auto IV | `Stereo-Injector/DX9/32-bit` | ✓ | 🟡 Mostly | `-stereo` cmdline. Needs GTA 4 shadows shader fix. |
| Grand Theft Auto V (Legacy) | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working | GTA 5 Story mode only, `-nobattleye` cmdline. PCSS shadows suggested. |
| GT Legends | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested |  |
| Inversion | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Partial | Might need more 3D Vision features or shader fix. |
| Just Cause 2 | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| L.A. Noire | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested | Game tells Automatic Mode what depth to use per frame. |
| Lost Planet 2 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | `Stereo=ON` in `config.ini`. |
| Mafia II | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Max Payne 3 | `Stereo-Injector/DX10-11/32-bit` | ✕ | ✅ Working | DX11. `-stereo 1` cmdline. |
| Medal of Honor (2010) | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | Single-player only. |
| Metro 2033 | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🟡 Mostly | Can't see weapon in hand. |
| Metro: Last Light | `Stereo-Injector/DX10-11/32-bit` | ✕ | 🧪 Untested | Lists 3D Vision support in [official PC requirements](https://www.reddit.com/r/Games/comments/1cjh4l/metro_last_light_official_pc_requirements/). |
| [Orbiter Space Flight Simulator](https://github.com/orbitersim/orbiter) | `Stereo-Injector/DX9/64-bit` | ✕ | 🧪 Untested | "Stereoscopic 3D" in settings. |
| Resident Evil 4 (2005) | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Pre-rendered FMV intro renders black (fine once in-game). Known XAudio2 shutdown-race on Alt-F4 is now filtered. |
| Resident Evil 5 | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | `Stereo=ON` in `config.ini`. DX9 only on Steam (DX10 only tested as DX9). |
| Resident Evil 6 | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | `Stereo=ON` in `config.ini`. Might need shader fix. |
| rFactor 2 | `Stereo-Injector/DX9/64-bit` | ✕ | 🧪 Untested | Single-player only. |
| Roller Coaster Rampage | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested |  |
| Sid Meier's Civilization V | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Super Street Fighter IV Arcade Edition | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Might need more 3D Vision features. |
| Street Fighter X Tekken | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need more 3D Vision features and shader fixes. |
| Tom Clancy's H.A.W.X 2 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| The Witcher 2: Assassins of Kings | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |

**Excluded (Native SBS/TAB):** *Deus Ex: Mankind Divided*, *DOOM 3: BFG Edition*, *Avatar: The Game*, *Sonic Generations*.

**Excluded (Native AMD HD3D):** *Battlefield 3*, *DiRT 2*, *DiRT 3*, *DiRT Showdown*, *DiRT Rally*, *GRID 2*, *GRID Autosport*, *Tomb Raider (2013)*.

**Excluded (Online Ban Risk):** *Aion: The Tower of Eternity*, *Diablo III*, *Hawken*, *Pirate101*, *Rusty Hearts*, *Wizard101*, *StarCraft II*.

**Excluded (Demo or Benchmark):** *Aliens vs. Triangles*, *Endless City*, *Stone Giant*, *Supersonic Sled*, *Passion Leads Army Benchmark*, *Unigine: Heaven Benchmark*.

## See also

- [3D Vision Direct](Native-3D-Vision-Direct) — for games that render stereo themselves via NVAPI
- [DirectX 9](API-DirectX-9) — the injector target for most of the games listed above
- [DirectX 11](API-DirectX-11) — the injector target for the handful of DX11 titles above
