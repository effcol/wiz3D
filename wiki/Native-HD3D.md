# HD3D (AMD)

✅ **Mostly Working.** HD3D games render stereo internally using AMD's HD3D API; wiz3D's HD3D proxy enables rendering, captures the quad-buffer output, and composites it into a modern stereo-3D format. Currently Half SBS / Half TAB work for about half the shipped games; the rest still need output work.

## How this path differs from the Stereo-Injector

HD3D is a **native-stereo** path: the game writes different content to left/right eye textures using the AMD HD3D API. wiz3D doesn't generate the stereo — it just enables and composites. Requires an AMD GPU with HD3D-capable drivers (older driver series).

See [Home](Home) for the Stereo-Injector vs native-stereo distinction.

## 0.4.0 highlight

Deus Ex: Human Revolution + Director's Cut fixed in 0.4.0 — see the shipped `BaseProfile.xml` entries.

## Install path

`HD3D/32-bit/` or `HD3D/64-bit/`. Runs alongside a Stereo-Injector install and only activates for games listed in `NativeProfile.xml`.

## Games

| Game | Install Path | Testing | Notes |
|---|---|---|---|
| Battlefield 3 | `HD3D/32-bit` | 🟡 Mostly | Campaign only. Strange depth issue, possibly not wiz3D-caused. Might need shader fix. |
| Deus Ex: Human Revolution | `HD3D/32-bit` | ✅ Working |  |
| Deus Ex: Human Revolution Director's Cut | `HD3D/32-bit` | ✅ Working |  |
| DiRT 2 | `HD3D/32-bit` | 🧪 Untested | `hardware_settings_config.xml` needs `stereo enabled="true"`. |
| DiRT 3 | `HD3D/32-bit` | 🧪 Untested | `hardware_settings_config.xml` needs `stereo enabled="true"`. Check iZ3D Shader Fix. |
| DiRT 3 Complete Edition | `HD3D/32-bit` | 🟡 Partial | `hardware_settings_config.xml` needs `stereo enabled="true"`. Only top half of Half SBS visible. |
| DiRT Showdown | `HD3D/32-bit` | 🟡 Partial | `hardware_settings_config.xml` needs `stereo enabled="true"`. Only top half of Half SBS visible. |
| DiRT Rally | `HD3D/32-bit` | 🟡 Partial | `hardware_settings_config.xml` needs `stereo enabled="true"`. Only top half of Half SBS visible. |
| F1 2010 | `HD3D/32-bit` | 🧪 Untested |  |
| F1 2011 | `HD3D/32-bit` | 🧪 Untested |  |
| F1 2012 | `HD3D/32-bit` | 🧪 Untested |  |
| F1 2013 | `HD3D/32-bit` | 🧪 Untested |  |
| GRID 2 | `HD3D/32-bit` | 🟡 Partial | `hardware_settings_config.xml` needs `stereo enabled="true"`. Only top half of Half SBS visible. |
| GRID Autosport | `HD3D/32-bit` | 🟡 Partial | `hardware_settings_config.xml` needs `stereo enabled="true"`. Only top half of Half SBS visible. |
| Sleeping Dogs | `HD3D/32-bit` | ✅ Working |  |
| Sleeping Dogs: Definitive Edition | `HD3D/64-bit` | ✅ Working | Undocumented AMD HD3D support. |
| Sniper Elite V2 | `HD3D/32-bit` | ✅ Working | Colour shifting when separation increased. [Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Sniper Elite III | `HD3D/32-bit` | ✅ Working | `customersupportlogging` beta branch only. [Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Sniper Elite 4 | `HD3D/64-bit` | ✅ Working | DX11 only. DX12 doesn't currently trigger HD3D stereo. [Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Sniper Elite: Nazi Zombie Army | `HD3D/32-bit` | ✅ Working | Colour shifting when separation increased. [Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Sniper Elite: Nazi Zombie Army 2 | `HD3D/32-bit` | ✅ Working | Colour shifting when separation increased. [Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Zombie Army Trilogy | `HD3D/32-bit` | ✅ Working | [Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Thief (2014) | `HD3D/32-bit` or `/64-bit` | ✅ Working | Both x86 and x64 working. |

**Excluded (Works on 3D Vision Direct Mode):** *Hitman: Absolution*, *Tomb Raider (2013)*.

**Excluded (Online Ban Risk):** *World of Warcraft* <sub>(Stereo3D removed in 2018 DX12 update)</sub>.

## See also

- [3D Vision Direct](Native-3D-Vision-Direct) — NVIDIA's counterpart native-stereo path
- Design decision tracked as task #57: HD3D shader-fix integration
