# DirectX 11

✅ **Mostly Working.** The DX11 Stereo-Injector pipeline landed in 0.4.0 via [PR #31](https://github.com/Stereopticon-Org/wiz3D/pull/31) by [@AkshayUHegde](https://github.com/AkshayUHegde), with 14 per-game profiles and the Feature Engine that handles constant-buffer stereo on the right eye. 0.5.0+ continues to expand profile coverage; Akshay's in-flight DX11 update targets stereo rendering on any DX11 title that uses standard CB layouts (crashes expected to be eliminated; shadows/reflections may still need per-game shader fixes).

## How the wrapper works

- `d3d11.dll` + `dxgi.dll` proxies intercept device / swap-chain creation.
- `SwapChain11Proxy` does the per-frame composite into whichever `OutputMode` is chosen.
- `Context11Proxy` wraps each draw; the Feature Engine inspects the pixel/vertex shader CRCs and applies right-eye CB edits based on a matched profile in `BaseProfile.xml`.
- An NVAPI stereo bridge wired in 0.4.0 means games that query NVAPI directly for stereo settings drive wiz3D's own separation/convergence state.

## Install path

`Stereo-Injector/DX10-11/32-bit/` or `/64-bit/`. Copy the contents (DLLs + `OutputMethods/` + `BaseProfile.xml` + `wiz3D_Config.xml`) next to the game's `.exe`. Some 2015+ titles are 32-bit despite being DX11 — try the other arch if the game crashes immediately.

## Games

| Game | Install Path | iZ3D Profile | Testing | Notes |
|---|---|---|---|---|
| Assassin's Creed | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| Battlefield 3 | `Stereo-Injector/DX10-11/32-bit` | ✕ | ✅ Working | DirectX 11. |
| Battlefield Hardline | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working |  |
| BioShock | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| BioShock 2 | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| BioShock Infinite | `Stereo-Injector/DX10-11/32-bit` | ✕ | ✅ Working |  |
| Call of Juarez: Bound in Blood | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| Company of Heroes | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| Cryostasis | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| Crysis | `Stereo-Injector/DX10-11/{32,64}-bit` | ✓ | 🧪 Untested |  |
| Crysis: Warhead | `Stereo-Injector/DX10-11/{32,64}-bit` | ✓ | 🧪 Untested |  |
| Crysis 2 | `Stereo-Injector/DX10-11/{32,64}-bit` | ✓ | 🧪 Untested | Includes shader fix. See if applicable to native SBS. |
| DCS: Black Shark | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested | Single-player may be OK; multiplayer not recommended. |
| De Blob | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| Dragon Age II | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| Far Cry 2 | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| Far Cry 3 | `Stereo-Injector/DX10-11/32-bit` | ✕ | ✅ Working | DX11 only. Fullscreen or windowed recommended over borderless. |
| Far Cry 4 | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working |  |
| Fallout Shelter | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working | Unity engine. UI mono via universal UI-Default handler (0x449010EA PS). |
| F.E.A.R. 3 | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| Gears of War | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| Homefront | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| Lost Planet | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| Mass Effect Andromeda | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working |  |
| Mass Effect Legendary Edition | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working | ME 1/2/3. Launcher window renders at 1440p when panel is sub-native (being refined). |
| Metro 2033 Redux | `Stereo-Injector/DX10-11/64-bit` | ✕ | 🟡 Mostly | Minor shader artefacts. |
| Metro: Last Light Redux | `Stereo-Injector/DX10-11/64-bit` | ✕ | 🟡 Mostly | Shares the Metro Redux profile. |
| Microsoft Flight Simulator X | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| NecroVisioN | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| NieR:Automata | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working |  |
| Red Faction: Guerrilla | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| Ryse: Son of Rome | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working |  |
| S.T.A.L.K.E.R.: Clear Sky | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| S.T.A.L.K.E.R.: Call of Pripyat | `Stereo-Injector/DX10-11/{32,64}-bit` | ✓ | 🧪 Untested |  |
| Serious Sam HD: The First Encounter | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| Star Wars Jedi: Fallen Order | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working | Launcher renders at 1440p when panel is sub-native. |
| Tempest 4000 | `Stereo-Injector/DX10-11/32-bit` | ✕ | ✅ Working |  |
| Tom Clancy's H.A.W.X | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |
| Tom Clancy's Splinter Cell: Blacklist | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Mostly | **DX9 only.** Needs shader fix. |
| Trine 4: The Nightmare Prince | `Stereo-Injector/DX10-11/64-bit` | ✕ | 🟡 Mostly | Fireplaces can flash in the right eye — workaround: add `setOption(rendererModule, "ParticleDistortion", false)` to `%APPDATA%\Trine4\options.txt` (issue #47). Shadows + 2D aim arrow still need shader-fix work. |
| Trine 5: A Clockwork Conspiracy | `Stereo-Injector/DX10-11/64-bit` | ✕ | 🟡 Mostly | Raindrops hitting the ground make a puff of smoke in the right eye instead of ripples. |
| What Remains of Edith Finch | `Stereo-Injector/DX10-11/64-bit` | ✕ | ✅ Working |  |
| World in Conflict | `Stereo-Injector/DX10-11/32-bit` | ✓ | 🧪 Untested |  |

**Excluded (Online Ban Risk):** *Age of Conan: Unchained*, *Champions Online*, *DC Universe Online*, *Entropia Universe*, *Final Fantasy 14*, *TERA Online*, *The Lord of the Rings Online*, *Warcraft III: Reign of Chaos*, *World of Tanks*.

## See also

- [DirectX 10](API-DirectX-10) — shares the DLL set, different renderer code paths
- [3D Vision Automatic-aware games](Injector-3D-Vision-Automatic-Aware) — games that had an NVIDIA 3D Vision profile tend to work well via this injector because the shape of the injection is similar
