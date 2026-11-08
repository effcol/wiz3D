# DirectX 9

✅ **Mostly Working.** The DX9 `d3d9.dll` proxy loader is wiz3D's most mature path. Left 4 Dead 2 and many others run in full stereo 3D; the profile system loads shader fixes and stereo settings for every originally-supported iZ3D game. 0.4.0 ported the modern OSD + laser-sight crosshair from DX11 back onto DX9. 0.4.1 added ShaderHunter / ShaderPatch / ShaderOverride to DX9 (task #53), so per-game fix authoring is now first-class on DX9 too.

## How the wrapper works

- `d3d9.dll` proxy intercepts device / swap-chain creation (MinHook-based since 0.4.0).
- `BaseStereoRenderer_Main` performs the per-draw right-eye replay with projection-matrix stereo-isation.
- `BaseProfile.xml` profiles override stereo behaviour per-exe (separation, convergence, per-shader mono flags, ShaderFix primitives).
- 0.4.0 added an NVAPI stereo bridge so games that query 3D-Vision state via NVAPI drive wiz3D's own separation / convergence.
- `UserProfile.xml` next to the game exe (0.4.3+) stores user-tuned knobs.

## Install path

`Stereo-Injector/DX9/32-bit/` or `Stereo-Injector/DX9/64-bit/`. Copy the contents (DLLs + `OutputMethods/` + `BaseProfile.xml` + `wiz3D_Config.xml`) next to the game's `.exe`.

## Games (32-bit)

| Game | Install Path | iZ3D Profile | Testing | Notes |
|---|---|---|---|---|
| Antichamber | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| A.R.E.S.: Extinction Agenda | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| AaAaAA!!! A Reckless Disregard for Gravity | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Alan Wake | `Stereo-Injector/DX9/32-bit` | ✕ | 🔴 Not Working | Crashes on title screen. |
| Alien Breed 2: Assault | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Alien Swarm | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Aliens vs. Predator (2010) | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ DX9 Working | Needs shadow shader fix on DX9. |
| Aliens Versus Predator | `Stereo-Injector/DX9/32-bit` | ✕ | 🔴 Not Working | Crashes on level load. |
| Alone in the Dark (2008) | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| America's Army | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Anomaly Warzone Earth | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| AquaNox 2: Revelation | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Arma: Armed Assault | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | aka ARMA: Combat Operations. |
| Armies of Exigo | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Assassin's Creed II | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Assassin's Creed: Brotherhood | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Audiosurf | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Back to the Future: The Game | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | Episode 1-5. |
| Batman: Arkham Origins Blackgate | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| Battlefield 2 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Battlefield 2142 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Battlestations: Pacific | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Beowulf: The Game | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Bet on Soldier | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Beyond Good & Evil | `Stereo-Injector/DX9/32-bit` | ✓ | 🟡 Mostly | Artifacts when paused. |
| Bayonetta | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Performance issues. Might need shader fix. |
| Bionic Commando (2009) | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Partial | Needs HUD and shadow shader fix, fullscreen exclusive, crashes on AMD. |
| Bionic Commando Rearmed | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Black & White 2 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Black Mesa | `Stereo-Injector/DX9/32-bit` | ✕ | 🔴 Not Working | Crashing. |
| BlazBlue: Calamity Trigger | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Mostly | Needs sprite depth shader fix; use [Geo-11 Fix](https://helixmod.blogspot.com/2026/05/blazblue-calamity-trigger-dx11.html). |
| Blur | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | May need shader fix. |
| Borderlands | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Brothers in Arms: Road to Hill 30 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Brothers in Arms: Earned in Blood | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Brothers in Arms: Hell's Highway | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Bulletstorm | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Burnout Paradise | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Cabela's Big Game Hunter: 10th Anniversary Edition | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | Profile says "Cabela Big Game Hunter BGH10.exe". |
| Call of Cthulhu: Dark Corners of the Earth | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Call of Duty 2 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Call of Duty: World at War | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Call of Duty 4: Modern Warfare (2007) | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Call of Duty: Modern Warfare 2 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Cars | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Castlevania Anniversary Collection | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Partial | Collection runs stereo but inner emulated games render on a single plane (flat 2D). Needs 2D-to-3D parallax primitive (planned 0.6.0+). |
| Castlevania: Lords of Shadow 2 | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Mostly | Needs shadow shader fix. |
| Chromadrome 2 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Command & Conquer 3: Tiberium Wars | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Place in `/RetailExe/1.10`. Game crashes when changing settings. |
| Command & Conquer: Red Alert 3 | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Place in `/Data/` folder. |
| Command & Conquer 4: Tiberian Twilight | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Condemned: Criminal Origins | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Demigod | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested |  |
| Damnation | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Dark Messiah of Might and Magic | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Might need [Mod Launcher](https://steamcommunity.com/sharedfiles/filedetails/?id=3378556750). |
| Dark Void | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Dead or Alive 5 Last Round | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Deadpool | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| Dead Space | `Stereo-Injector/DX9/32-bit` | ✓ | 🟡 Partial | Crashes past Main Menu. |
| Dead Space 2 | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Dead Space 3 | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Mostly | Needs HUD shader fix. |
| Defence Grid: The Awakening | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Delta Force: Xtreme | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Devil May Cry 3: Special Edition | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| DiRT | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Disciples III: Renaissance | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Divinity II: Ego Draconis | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Dragon Age: Origins | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Drakensang: The Dark Eye | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Driver: San Francisco | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working |  |
| Dungeons (2011) | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested |  |
| Dungeon Siege II | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Dungeon Siege III | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Empire: Total War | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Enemy Engaged 2 | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested |  |
| Eragon | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Evolution GT | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested |  |
| Fable: The Lost Chapters | `Stereo-Injector/DX9/32-bit` | ✓ | 🟡 Partial | Artifacts, needs shader fix. |
| Fable III | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Fable Anniversary | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Partial | Artifacts, needs shader fix. |
| Fahrenheit | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | aka Indigo Prophecy. |
| Fallout 3 | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Fallout: New Vegas | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Mostly | Needs Skybox & Pip-Boy UI shader fix. |
| F.E.A.R. | `Stereo-Injector/DX9/32-bit` | ✓ | 🔴 Not Working | Crashes on launch. |
| F.E.A.R. Perseus Mandate | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| F.E.A.R. 2: Project Origin | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| FIFA 10 | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested |  |
| FIFA 14 | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working |  |
| Flashback (2013) | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | May need shader fix. |
| FlatOut | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| FlatOut 2 | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested |  |
| FlatOut: Ultimate Carnage | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Foreign Legion: Buckets of Blood | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested |  |
| Frontlines: Fuel of War | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Front Mission Evolved | `Stereo-Injector/DX9/32-bit` | ✕ | 🧪 Untested |  |
| Fuel | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| G-Force | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Garshasp: The Monster Slayer | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Ghostbusters: The Video Game | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Ghostbusters: Sanctum of Slime | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Google Earth | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | [Google Earth Pro 7.1.5.1557](https://web.archive.org/web/20171014110844/https://dl.google.com/earth/client/GE7/release_7_1_8/googleearth-win-pro-7.1.8.3036.exe). |
| Gothic 3 | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Mostly | Sky at screen depth, needs shader fix. |
| Grand Theft Auto: San Andreas | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| GRID | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Guitar Hero III: Legends of Rock | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Half-Life 2 | `Stereo-Injector/DX9/32-bit` | ✓ | 🟡 Mostly | `steam_legacy` beta branch. Use `-game ` cmdline. Shadows have issues. |
| Halo: Combat Evolved | `Stereo-Injector/DX9/32-bit` | ✕ | 🔴 Not Working | Crashes on load. |
| Heroes of Might and Magic V | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Hitman: Blood Money | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Needs shadow and water shader fix. |
| Hunted: The Demon's Forge | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Injustice: Gods Among Us | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| Insane 2 | `Stereo-Injector/DX9/32-bit` | ✕ | 🔴 Not Working |  |
| Jet Set Radio | `Stereo-Injector/DX9/32-bit` | ✕ | 🔴 Not Working | Only works in mono. |
| Kane & Lynch 2: Dog Days | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Killing Floor | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| King Arthur: The Role-Playing Wargame | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | "King Arthur: The Druids" expansion. |
| King's Bounty: The Legend | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Left 4 Dead | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Single-player only. Use `-insecure` cmdline to avoid VAC ban. |
| Left 4 Dead 2 | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Single-player only. Use `-insecure` cmdline to avoid VAC ban. |
| Legacy of Kain: Defiance | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Needs shadow shader fix. |
| Lego Star Wars: The Video Game | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Lego Star Wars III: The Clone Wars | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Madden NFL 08 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Mad Riders | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| Majesty 2: The Fantasy Kingdom Sim | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Mass Effect | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Mass Effect 2 | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Medal of Honor: Airborne | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Mercenaries 2: World in Flames | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Metal Gear Rising: Revengeance | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| Microsoft Flight Simulator 2004: A Century of Flight | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Mini Ninjas | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Mirror's Edge | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Monday Night Combat | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Mortal Kombat Komplete Edition | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working |  |
| MTX: Mototrax | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Mythos | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | Possibly Mythos (2009). |
| Naruto Shippuden: Ultimate Ninja Storm 3 Full Burst (2013) | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Mostly | Needs shader fix and swap eye. |
| Need for Speed: ProStreet | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Need for Speed: Undercover | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Need for Speed: Hot Pursuit (2010) | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Need for Speed: Shift | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Need for Speed: Shift 2 Unleashed | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Ninja Blade | `Stereo-Injector/DX9/32-bit` | ✓ | 🔴 Not Working | Black screen on AMD. |
| Oddworld: New 'n' Tasty! | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Partial | Heavy artifacts, needs shader fix. |
| Operation Flashpoint: Dragon Rising | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| OutRun 2006: Coast 2 Coast | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Overlord | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Overlord II | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Painkiller | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Painkiller: Overdose | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Portal | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Use `-game Portal -dx9` cmdline. Place in `/portal/bin/`. |
| Portal 2 | `Stereo-Injector/DX9/32-bit` | ✓ | 🟡 Mostly | Place in `/Portal 2/bin/`. Separation initially too high, needs wall-display shader mod. |
| Portal Stories: Mel | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Needs particle glow, glass and portal shader fix. |
| Prince of Persia (2008) | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| Prince of Persia: The Forgotten Sands | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Pro Evolution Soccer 2017 | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| ProtoGalaxy | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Prototype 2 | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Mostly | Needs shadows shader fix. |
| Rayman Legends | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| RefRain - prism memories - | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Partial | Play with depth set low. Needs higher-depth shader fix and has SR Weave issues. |
| Richard Burns Rally | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Rise and Fall: Civilizations at War | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Rise of Flight | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Rise of the Argonauts | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Risen | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Saints Row 2 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Sam & Max Save the World | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | Episode 4: Abe Lincoln Must Die! |
| Sam & Max: The Devil's Playhouse | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | Episode 1: The Penal Zone. |
| Samurai Warriors 2 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Sanctum | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Section 8 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Sega Rally Revo | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Needs shadow shader fix. |
| Serious Sam 2 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Sexy Beach 3 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Shadowgrounds | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Sid Meier's Civilization IV | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Sid Meier's Railroads! | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Silent Hunter 3 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Silent Hunter 4: Wolves of the Pacific | `Stereo-Injector/DX9/32-bit` | ✓ | 🔴 Not Working | Crashes on startup. |
| Silent Hunter 5: Battle of the Atlantic | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Sine Mora EX | `Stereo-Injector/DX9/32-bit` | ✕ | 🔴 Not Working |  |
| Singularity | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Sins of a Solar Empire | `Stereo-Injector/DX9/32-bit` | ✓ | 🟡 Mostly | Background at wrong depth. |
| SkyDrift | `Stereo-Injector/DX9/32-bit` | ✕ | 🔴 Not Working |  |
| Sonic Adventure 2 | `Stereo-Injector/DX9/32-bit` | ✕ | 🔴 Not Working | Black screen. |
| Sonic & Sega All-Stars Racing | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working |  |
| Sonic R (2004) | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Mostly | Needs coin depth shader fix. Use with [Sonic R Updater](https://github.com/cheatfreak47/SRUpdater). |
| Spec Ops: The Line | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working |  |
| Split/Second: Velocity | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Mostly | Needs shader fix. Aka Split/Second. |
| Spore | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| S.T.A.L.K.E.R.: Shadow of Chernobyl | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Star Trek: Legacy | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Star Wars: Battlefront (2004) | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Star Wars: Battlefront II (2005) | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Star Wars: Episode I - Racer | `Stereo-Injector/DX9/32-bit` | ✕ | 🔴 Not Working | Crashes on startup. |
| Star Wars: The Force Unleashed II | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Partial | Needs shader fix. |
| Starship Troopers | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Street Fighter IV | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Supreme Commander | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Supreme Commander: Forged Alliance | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Supreme Commander 2 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Test Drive Unlimited | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| The Ball | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Mostly | Ball shadow different in both eyes. |
| The Chronicles of Narnia: The Lion, the Witch and the Wardrobe | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | Guess based on "Narnia.exe". |
| The Elder Scrolls IV: Oblivion | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| The Elder Scrolls V: Skyrim | `Stereo-Injector/DX9/32-bit` | ✕ | 🔴 Not Working | Enters main menu, freezes on level load. |
| The Lord of the Rings: The Battle for Middle-earth | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Mostly | Needs UI shader fix. |
| The Movies (2005) | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| The Settlers II: 10th Anniversary | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| The Sims 2: University | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | Targets "Sims2EP1.exe". |
| The Sims 3 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| The Sims Medieval | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| The Witcher | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working | Place files in `/System/`. |
| TimeShift | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Titan Quest | `Stereo-Injector/DX9/32-bit` | ✓ | 🟡 Mostly | Needs shadow shader fix. |
| TOCA Race Driver 3 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Tom Clancy's Ghost Recon Advanced Warfighter 2 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Tom Clancy's Rainbow Six: Vegas | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Tom Clancy's Rainbow Six: Vegas 2 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Tom Clancy's Splinter Cell: Double Agent | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Tom Clancy's Splinter Cell: Conviction | `Stereo-Injector/DX9/32-bit` | ✓ | 🟡 Mostly | Needs shader fix. |
| Tomb Raider: Legend | `Stereo-Injector/DX9/32-bit` | ✓ | 🟡 Mostly | Needs shadow shader fix. |
| Tomb Raider: Anniversary | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Tomb Raider: Underworld | `Stereo-Injector/DX9/32-bit` | ✓ | ✅ Working |  |
| Torchlight | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| TrackMania Nations Forever | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Trine (2009) | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Two Worlds | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Unreal Tournament 3 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Valkyria Chronicles | `Stereo-Injector/DX9/{32,64}-bit` | ✕ | 🟡 Partial | Needs UI, reflections and shadow shader fix. |
| Vanquish | `Stereo-Injector/DX9/32-bit` | ✕ | 🟡 Mostly | Needs crosshair shader fix. |
| Virtua Tennis 2009 | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Virtua Tennis 4 | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | Might need shader fix. |
| Wallace & Gromit's Grand Adventures | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested | Episodes 1-4. |
| Wanted: Weapons of Fate | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Warhammer 40,000: Dawn of War | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Warhammer 40,000: Dawn of War: Dark Crusade | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Warhammer 40,000: Dawn of War: Soulstorm | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Warhammer 40,000: Dawn of War II | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Watchmen: The End is Nigh | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Wheelman | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working | May need shader fix. |
| Wings of Prey | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| Wolfenstein (2009) | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| WorldShift | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |
| X-Men Origins: Wolverine | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working |  |
| Yaiba: Ninja Gaiden Z | `Stereo-Injector/DX9/32-bit` | ✕ | ✅ Working |  |
| Zeno Clash | `Stereo-Injector/DX9/32-bit` | ✓ | 🧪 Untested |  |

**Excluded (Game Not Playable):** *Darkspore*. <sub>(Not playable until [Resurrection Capsule](https://github.com/vitor251093/resurrection-capsule) completes.)</sub>

**Excluded (Online Ban Risk):** *Allods Online*, *Dark Age of Camelot*, *Darkfall Online*, *Fury (2007)*, *Global Agenda*, *Guild Wars*, *Monster Hunter Frontier Online*, *Warhammer Online: Age of Reckoning*.

## Games (64-bit)

| Game | Install Path | iZ3D Profile | Testing | Notes |
|---|---|---|---|---|
| Chess Titans | `Stereo-Injector/DX9/64-bit` | ✕ | 🧪 Untested | Freezes on level load. |
| Dragon Ball Xenoverse | `Stereo-Injector/DX9/64-bit` | ✕ | ✅ Working | May need shader fix. |
| Far Cry | `Stereo-Injector/DX9/{32,64}-bit` | ✓ | 🔴 Not Working | Crashes on startup. |
| Final Fantasy IX | `Stereo-Injector/DX9/64-bit` | ✕ | 🟡 Partial | Dual-View Output works, but flat 3D. |
| Pro Evolution Soccer 2016 | `Stereo-Injector/DX9/64-bit` | ✕ | ✅ Working |  |
| REFLEX XTR² | `Stereo-Injector/DX9/{32,64}-bit` | ✓ | 🧪 Untested |  |
| Tales of Berseria | `Stereo-Injector/DX9/64-bit` | ✕ | 🟡 Mostly | Not working in SR Weave output; works in other 3D outputs. |
| Ultimate Marvel vs. Capcom 3 | `Stereo-Injector/DX9/64-bit` | ✕ | 🔴 Not Working |  |
| Unreal Tournament 2004 | `Stereo-Injector/DX9/64-bit` | ✓ | ✅ Working |  |

**Excluded (VAC Ban Risk):** *Counter-Strike: Source*, *Day of Defeat: Source*, *Half-Life 2: Deathmatch*.

**Excluded (Online Ban Risk):** *Dungeons & Dragons Online*, *EVE Online*, *EverQuest*, *EverQuest 2*, *Flyff (Fly For Fun)*, *RIFT*, *Starcraft II*.

## See also

- [3D Vision Automatic-aware games](Injector-3D-Vision-Automatic-Aware) — DX9 titles that had an NVIDIA 3D Vision profile and typically need a specific config-file / cmdline flag to engage stereo properly.
