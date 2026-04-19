# wiz3D "We See 3D"

A universal stereoscopic 3D wrapper for DirectX, OpenGL, AMD HD3D, and Nvidia 3D Vision. 

**iZ3D** meant "**I** See 3D", so **wiz3D** means "**We** See 3D"

<img width="3840" height="1080" alt="vlcsnap-2026-04-05-21h46m56s809" src="https://github.com/user-attachments/assets/a44e887b-cf54-46fd-9b25-ede89bf87167" />
<img width="3840" height="1080" alt="vlcsnap-2026-04-03-22h37m17s146" src="https://github.com/user-attachments/assets/3d49776f-c689-4b6e-b186-b4cb108a20f5" />


---

## What Is This?

Wiz3D is an open-source stereoscopic 3D wrapper that hooks into DirectX, OpenGL, and AMD HD3D native games to generate real-time stereo 3D output (Half Side-by-Side, Top-and-Bottom, Anaglyph, etc.) without requiring kernel drivers or proprietary hardware.

**The History:** iZ3D was a commercial product (~2002–2010) and one of the pioneers in modding games for stereoscopic 3D using kernel-level driver injection. The original developers kindly open-sourced the code under the MIT license, hosted by [bo3b/iZ3D](https://github.com/bo3b/iZ3D).

**This Project:** wiz3D modernizes that source code, replaces kernel-level hooks with a proxy DLL loader, and expands the scope to enable native stereoscopic 3D within AMD HD3D and Nvidia 3D Vision games.

## Current Status

### Released Build:

* **AMD HD3D:** ✅ **Mostly Working!** Proxy chain successfully intercepts and converts Top-and-Bottom quad-buffer output to universal Half Side-by-Side (HSBS). Still needs Ego engine games and Tomb Raider 2013 to work fully. Mouse doubling needs implementing, more outputs need to be supported, and need to make sure it works on all GPUs and on Linux.

### Unreleased Builds:

* **DirectX 9:** ✅ **Mostly Working!** `d3d9.dll` proxy loader works! Left 4 Dead 2 and many others run in full stereo 3D, outputs in all originally supported formats, and the profile system loads shader fixes and stereo settings for all originally supported games.
* **DirectX 10/11:** ⚠️ **Partial.** The DX10/11 wrapper was never completely finished by iZ3D Inc. Some games work, many crash. Current build works in some games, needs further testing.
* **DirectX 7/8:** ⚠️ **In Progress.** iZ3D used wrappers for DX7/8 to then run them in DX9's stereoization. Currently I've made basic test builds, feel free to test them! 
* **OpenGL:** ⚠️ **In Progress.** Basic build has been made, untested. Feel free to test it and report back your findings! 
* **Nvidia 3D Vision:** ⚠️ **In Progress.** Still in very early stages of WIP, kind of a long shot. I'm pretty confident native "3D Vision Ready" games can be supported by this project in the future using a hybrid 3D Vision + iZ3D + AMD Quad-Buffer approach.

---

## Getting Started Playing

1. Download the latest release from the **Releases** tab.
2. Find the executable (`.exe`) of the game you want to play, and check the API and bitness, and if it supports AMD HD3D support for the game you want to play ([PCGamingWiki](https://www.pcgamingwiki.com/wiki/Home) usually works if not listed here).
3. Copy the **contents** of the appropriate `wiz3D` subfolder (e.g., the contents of the `dx9` > `x86` folder for a DX9 32-bit game) directly into the folder containing the game's `.exe`.
4. Launch the game. Stereo 3D should activate automatically! If it's an HD3D game, you may need to activate `stereoscopic 3D` or `HD3D` in the in-game menu.

Configure your output mode (Half Side-by-Side, Anaglyph, etc.) and any other settings you'd like to modify using the included `Config.xml` file.

---

## wiz3D Game Test Results

*Legend: **Working** = Stereo output activated and playable. **Partial** = Stereo activated but with issues (crash, wrong settings, shader problems). **Not loading** = Wrapper not activating.*

### AMD HD3D Native Game List

| Game | API | Bits | Result | Notes |
|------|-----|------|--------|-------|
| Battlefield 3 | DX11 | x86 | ✅ Half SBS working | Only tested in Campaign. Wary of testing in multiplayer, I would recommend only trying in Campaign. |
| Deus Ex: Human Revolution | DX11 | x86 | ✅ Half SBS working | Cursor doubled correctly. |
| Deus Ex: Human Revolution Director's Cut | DX11 | x86 | ✅ Half SBS working | Undocumented native AMD HD3D support. Cursor doubled correctly. |
| DiRT 2 | DX11 | x86 | Untested | `hardware_settings_config.xml` needs `stereo enabled="true"` |
| DiRT 3 | DX11 | x86 | Untested | `hardware_settings_config.xml` needs `stereo enabled="true"` |
| DiRT 3 Complete Edition | DX11 | x86 | ⚠️ Partial | Only top half of Half SBS visible. `hardware_settings_config.xml` needs `stereo enabled="true"` |
| DiRT Showdown | DX11 | x86 | ⚠️ Partial | Only top half of Half SBS visible. `hardware_settings_config.xml` needs `stereo enabled="true"` |
| DiRT Rally | DX11 | x86 | ⚠️ Partial | Only top half of Half SBS visible. `hardware_settings_config.xml` needs `stereo enabled="true"` |
| F1 2010 | DX11 | x86 | Untested | Untested |
| F1 2011 | DX11 | x86 | Untested | Untested |
| F1 2012 | DX11 | x86 | Untested | Untested |
| F1 2013 | DX11 | x86 | Untested | Untested |
| GRID 2 | DX11 | x86 | ⚠️ Partial | Only top half of Half SBS visible. `hardware_settings_config.xml` needs `stereo enabled="true"` |
| GRID Autosport | DX11 | x86 | ⚠️ Partial | Only top half of Half SBS visible. `hardware_settings_config.xml` needs `stereo enabled="true"` |
| Hitman: Absolution | DX11 | x86 | ✅ Half SBSB working | Mouse not doubling. |
| Sleeping Dogs | DX11 | x86 | ✅ Half SBS working | Mouse not doubling. |
| Sleeping Dogs: Definitive Edition | DX11 | x64 | ✅ Half SBS working | Undocumented native AMD HD3D support. |
| Sniper Elite V2 | DX11 | x86 | ✅ Half SBS working | Minor color shifting, and mouse not doubling. |
| Sniper Elite III | DX11| x86| ✅ Half SBS working | `customersupportlogging` beta branch only. Main branch skips HD3D driver loading. |
| Sniper Elite 4 | DX11/12| x64| ✅ Half SBS working | DX11 only. DX12 might be possible in future. |
| Sniper Elite: Nazi Zombie Army | DX11 | x86 | ✅ Half SBS working | Minor color shifting, and mouse not doubling. |
| Sniper Elite: Nazi Zombie Army 2 | DX11 | x86 | ✅ Half SBS working | Minor color shifting, and mouse not doubling. |
| Zombie Army Trilogy | DX11 | x86 | ✅ Half SBS working | Colors appear correct. |
| Thief (2014) | DX11 | x86/x64| ✅ Half SBS working | Both x86 and x64 versions working. |
| Tomb Raider (2013) | DX11 | x86 | ⚠️ Partial | Only top half of Half SBS. Most stubborn HD3D game to get working. |
| ~~Crysis 2~~ | DX11 | x86 | 🚫 N/A | Native Depth-Map Reprojection SBS output. |
| ~~Crysis 3~~ | DX11 | x86 | 🚫 N/A | Native Depth-Map Reprojection SBS output. |
| ~~Rise of the Tomb Raider~~ | DX11 | x64 | 🚫 N/A | Native SBS output. |
| ~~Shadwen~~ | DX11 | x64 | 🚫 N/A | Native SBS output. |
| ~~Two Worlds II~~ | DX10 | x86 | 🚫 N/A | Native TAB output. |
| ~~Trine 1~~ | DX11 | x86 | 🚫 N/A | Native SBS output. |
| ~~Trine 2~~ | DX11 | x86 | 🚫 N/A | Native SBS output. |
| ~~Trine 3~~ | DX11 | x64 | 🚫 N/A | Native SBS output. |
| ~~World of Warcraft~~ | DX11 | x86 | 🚫 N/A | MMO. Stereo3D support was removed in the game's 2018 DX12 update. |

### DirectX 7/8 Games

| Game | API | Bits | iZ3D Profile | Result | Notes |
|------|-----|---------|--------|-------|-------|
| Ballance | DX8 | x86 | ✓ | - | - |
| Empire Earth II | DX8 | x86 | ✓ | - | - |
| FATE | DX8 | x86 | ✕ | - | - |
| Command & Conquer: Renegade | DX8 | x86 | ✕ | - | - |
| Command & Conquer: Generals | DX8 | x86 | ✕ | - | - |
| Freelancer | DX8 | x86 | ✓ | - | - |
| GTR - FIA GT Racing Game | DX8 | x86 | ✓ | - | - |
| Mega Man X8 | DX8 | x86 | ✓ | - | - |
| Mercedes-Benz World Racing | DX8 | x86 | ✓ | - | - |
| NASCAR Racing 2003 Season | OpenGL/DX8 | x86 | ✓ | - | - |
| Sniper Elite | DX8 | x86 | ✓ | - | - |
| The Lord of the Rings: The Return of the King | DX8 | x86 | ✓ | - | - |
| Thief: Deadly Shadows | DX8 | x86 | ✓ | - | - |
| Tom Clancy's Rainbow Six 3: Raven Shield | DX8 | x86 | ✓ | - | - |
| Tony Hawk's Pro Skater 3 | DX8 | x86 | ✓ | - | - |


## DirectX 9 32bit Games

| Game | API | Bits | iZ3D Profile | Result | Notes |
|------|-----|---------|--------|-------|-------|
| A.R.E.S.: Extinction Agenda | DX9 | x86 | ✓ | - | - |
| AaAaAA!!! A Reckless Disregard for Gravity | DX9 | x86 | ✓ | - | - |
| Aion: The Tower of Eternity | DX9 | x86 | ✓ | - | Might include Shader Fix. See if shader fix can be applied to the 3D Vision native version. |
| Alien Breed 2: Assault | DX9 | x86 | ✓ | - | - |
| Alien Swarm | DX9 | x86 | ✓ | - | - |
| Allods Online | DX9 | x86 | ✓ | - | - |
| Alone in the Dark (2008) | DX9 | x86 | ✓ | - | - |
| America's Army | DX8/DX9 | x86 | ✓ | - | - |
| Anomaly Warzone Earth | DX9 | x86 | ✓ | - | - |
| AquaNox 2: Revelation | DX8/DX9 | x86 | ✓ | - | - |
| Arma: Armed Assault | DX9 | x86 | ✓ | - | aka ARMA: Combat Operations  |
| Armies of Exigo | DX9 | x86 | ✓ | - | - |
| Assassin's Creed II | DX9 | x86 | ✓ | - | - |
| Assassin's Creed: Brotherhood | DX9 | x86 | ✓ | - | - |
| Audiosurf | DX9 | x86 | ✓ | - | - |
| Back to the Future: The Game | DX9 | x86 | ✓ | - | Episode 1-5 |
| Batman: Arkham Asylum | DX9 | x86 | ✓ | - | Might include Shader Fix. See if shader fix can be applied to the 3D Vision native version. |
| Battlefield 2 | DX9 | x86 | ✓ | - | - |
| Battlefield 2142 | DX9 | x86 | ✓ | - | - |
| Battlestations: Pacific | DX9 | x86 | ✓ | - | - |
| Beowulf: The Game | DX9 | x86 | ✓ | - | - |
| Bet on Soldier | DX9 | x86 | ✓ | - | - |
| Beyond Good & Evil | DX9 | x86 | ✓ | - | - |
| Bionic Commando (2009) | DX9 | x86 | ✕ | - | - |
| Bionic Commando Rearmed | DX9 | x86 | ✓ | - | - |
| Black & White 2 | DX9 | x86 | ✓ | - | - |
| BlazBlue: Calamity Trigger | DX9 | x86 | ✕ | - | - |
| Borderlands | DX9 | x86 | ✓ | - | - |
| Brothers in Arms: Road to Hill 30 | DX9 | x86 | ✓ | - | - |
| Brothers in Arms: Earned in Blood | DX9 | x86 | ✓ | - | - |
| Brothers in Arms: Hell's Highway | DX9 | x86 | ✓ | - | - |
| Bulletstorm | DX9 | x86 | ✓ | - | - |
| Burnout Paradise | DX9 | x86 | ✓ | - | - |
| Cabela's Big Game Hunter: 10th Anniversary Edition | DX9 | x86 | ✓ | - | Not sure exactly which game, profile just says "Cabela Big Game Hunter BGH10.exe" |
| Call of Cthulhu: Dark Corners of the Earth | DX9 | x86 | ✓ | - | - |
| Call of Duty 2 | DX9 | x86 | ✓ | - | - |
| Call of Duty: World at War | DX9 | x86 | ✓ | - | - |
| Call of Duty: Black Ops | DX9 | x86 | ✓ | - | Might install Shader Fix. See if shader fix can be applied to the 3D Vision native version. |
| Call of Duty 4: Modern Warfare (2007) | DX9 | x86 | ✓ | - | - |
| Call of Duty: Modern Warfare 2 | DX9 | x86 | ✓ | - | - |
| Cars | DX9 | x86 | ✓ | - | - |
| Chromadrome 2 | DX9 | x86 | ✓ | - | - |
| Command & Conquer 3: Tiberium Wars | DX9 | x86 | ✓ | - | - |
| Command & Conquer: Red Alert 3 | DX9 | x86 | ✓ | - | - |
| Command & Conquer 4: Tiberian Twilight | DX9 | x86 | ✓ | - | - |
| Condemned: Criminal Origins | DX9 | x86 | ✓ | - | - |
| Demigod | DX9 | x86 | ✕ | - | - |
| Damnation | DX9 | x86 | ✓ | - | - |
| Dark Age of Camelot | DX9 | x86 | ✕ | - | - |
| Dark Messiah of Might and Magic | DX9 | x86 | ✓ | - | - |
| Dark Void | DX9 | x86 | ✓ | - | - |
| Dead Space | DX9 | x86 | ✓ | - | - |
| Dead Space 2 | DX9 | x86 | ✓ | - | - |
| Darkfall Online | DX9 | x86 | ✕ | - | - |
| Defense Grid: The Awakening | DX9 | x86 | ✓ | - | - |
| Delta Force: Xtreme | DX9 | x86 | ✓ | - | - |
| Devil May Cry 3: Special Edition | DX9 | x86 | ✓ | - | - |
| DiRT | DX9 | x86 | ✓ | - | - |
| Disciples III: Renaissance | DX9 | x86 | ✓ | - | - |
| Divinity II: Ego Draconis  | DX9 | x86 | ✓ | - | - |
| Dragon Age: Origins | DX9 | x86 | ✓ | - | - |
| Drakensang: The Dark Eye | DX7/DX9 | x86 | ✓ | - | - |
| Dungeons (2011) | DX9 | x86 | ✕ | - | - |
| Dungeon Siege II | DX9 | x86 | ✓ | - | - |
| Dungeon Siege III | DX9 | x86 | ✓ | - | - |
| Empire: Total War | DX9 | x86 | ✓ | - | - |
| Enemy Engaged 2 | DX9 | x86 | ✕ | - | - |
| Eragon | DX9 | x86 | ✓ | - | - |
| Fable: The Lost Chapters | DX9 | x86 | ✓ | - | - |
| Fable III | DX9 | x86 | ✓ | - | - |
| Fahrenheit | DX9 | x86 | ✓ | - | aka Indigo Prophecy |
| Fallout 3 | DX9 | x86 | ✓ | - | - |
| F.E.A.R. | DX9 | x86 | ✓ | - | - |
| F.E.A.R. Perseus Mandate | DX9 | x86 | ✓ | - | - |
| F.E.A.R. 2: Project Origin | DX9 | x86 | ✓ | - | - |
| FIFA 10 | DX9 | x86 | ✕ | - | - |
| FlatOut | DX9 | x86 | ✓ | - | - |
| FlatOut 2 | DX9 | x86 | ✕ | - | - |
| FlatOut: Ultimate Carnage | DX9 | x86 | ✓ | - | - |
| Foreign Legion: Buckets of Blood | DX9 | x86 | ✕ | - | - |
| Frontlines: Fuel of War | DX9 | x86 | ✓ | - | - |
| Front Mission Evolved | DX9 | x86 | ✕ | - | - |
| Fuel | DX9 | x86 | ✕ | - | - |
| G-Force | DX9 | x86 | ✓ | - | - |
| Garshasp: The Monster Slayer | DX9 | x86 | ✓ | - | - |
| Ghostbusters: The Video Game | DX9 | x86 | ✓ | - | - |
| Ghostbusters: Sanctum of Slime | DX9 | x86 | ✓ | - | - |
| Google Earth | OpenGL/DX9 | x86 | ✓ | - | [Google Earth Pro 7.1.5.1557](https://web.archive.org/web/20171014110844/https://dl.google.com/earth/client/GE7/release_7_1_8/googleearth-win-pro-7.1.8.3036.exe) |
| Grand Theft Auto: San Andreas | DX9 | x86 | ✓ | - | - |
| Grand Theft Auto IV | DX9 | x86 | ✓ | - | - |
| GRID | DX9 | x86 | ✓ | - | - |
| Guild Wars | DX9 | x86 | ✓ | - | - |
| Guitar Hero III: Legends of Rock | DX9 | x86 | ✓ | - | - |
| Half-Life 2 | DX9 | x86 | ✓ | ⚠️ Not loading | Suspect issue with needing `-game` command line argument  |
| Heroes of Might and Magic V | DX9 | x86 | ✓ | - | - |
| Hitman: Blood Money | DX9 | x86 | ✓ | - | - |
| Hunted: The Demon's Forge | DX9 | x86 | ✓ | - | - |
| Kane & Lynch 2: Dog Days | DX9 | x86 | ✓ | - |  |
| Killing Floor | DX8/DX9 | x86 | ✓ | - | - |
| King Arthur: The Role-Playing Wargame | DX9 | x86 | ✓ | - | Specifies 'King Arthur: The Druids' expansion pack |
| King's Bounty: The Legend | DX9 | x86 | ✓ | - | - |
| Left 4 Dead | DX9 | x86 | ✓ | ✅ Working | - |
| Left 4 Dead 2 | DX9 | x86 | ✓ | ✅ Working | - |
| Lego Star Wars: The Video Game | DX9 | x86 | ✓ | - | - |
| Lego Star Wars III: The Clone Wars | DX9 | x86 | ✓ | - | - |
| Madden NFL 08 | DX9 | x86 | ✓ | - | - |
| Mafia II | DX9 | x86 | ✓ | - | Might include Shader Fix. See if shader fix can be applied to the 3D Vision native version. |
| Majesty 2: The Fantasy Kingdom Sim | DX9 | x86 | ✓ | - | - |
| Mass Effect | DX9 | x86 | ✓ | - | - |
| Mass Effect 2 | DX9 | x86 | ✓ | - | - |
| Medal of Honor: Airborne | DX9 | x86 | ✓ | - | - |
| Mercenaries 2: World in Flames | DX9 | x86 | ✓ | - | - |
| Microsoft Flight Simulator 2004: A Century of Flight | DX9 | x86 | ✓ | - | - |
| Mini Ninjas | DX9 | x86 | ✓ | - | - |
| Mirror's Edge | DX9 | x86 | ✓ | - | - |
| Monday Night Combat | DX9 | x86 | ✓ | - | - |
| MTX: Mototrax | DX9 | x86 | ✓ | - | - |
| Mythos | DX9 | x86 | ✓ | - | Mythos (2009) maybe? Not sure which game this is. |
| Need for Speed: ProStreet | DX9 | x86 | ✓ | - | - |
| Need for Speed: Undercover | DX9 | x86 | ✓ | - | - |
| Need for Speed: Hot Pursuit (2010) | DX9 | x86 | ✓ | - | - |
| Need for Speed: Shift | DX9 | x86 | ✓ | - | - |
| Need for Speed: Shift 2 Unleashed | DX9 | x86 | ✓ | - | - |
| Ninja Blade | DX9 | x86 | ✓ | - | - |
| Operation Flashpoint: Dragon Rising | DX9 | x86 | ✓ | - | - |
| OutRun 2006: Coast 2 Coast | DX9 | x86 | ✓ | - | - |
| Overlord | DX9 | x86 | ✓ | - | - |
| Overlord II | DX9 | x86 | ✓ | - | - |
| Painkiller | DX9 | x86 | ✓ | - | - |
| Painkiller: Overdose | DX9 | x86 | ✓ | - | - |
| Portal | DX9 | x86 | ✓ | ⚠️ Not loading | Suspect issue with needing `-game portal` command line argument  |
| Portal 2 | DX9 | x86 | ✓ | - | - |
| Prince of Persia: The Forgotten Sands | DX9 | x86 | ✓ | - | - |
| ProtoGalaxy | DX9 | x86 | ✓ | - | - |
| Resident Evil 5 | DX9 | x86 | ✓ | - | Might include Shader Fix. See if shader fix can be applied to the 3D Vision native version. |
| Richard Burns Rally | DX9 | x86 | ✓ | - | - |
| Rise and Fall: Civilizations at War | DX9 | x86 | ✓ | - | - |
| Rise of Flight | DX9 | x86 | ✓ | - | - |
| Rise of the Argonauts | DX9 | x86 | ✓ | - | - |
| Risen | DX9 | x86/x64 | ✓ | - | - |
| S.T.A.L.K.E.R.: Shadow of Chernobyl | DX9 | x86 | ✓ | - | - |
| Saints Row 2 | DX9 | x86 | ✓ | - | - |
| Sam & Max Save the World | DX8/DX9 | x86 | ✓ | - | Specifically notes Episode 4: Abe Lincoln Must Die! |
| Sam & Max: The Devil's Playhouse | DX9 | x86 | ✓ | - | Specifically notes Episode 1: The Penal Zone |
| Samurai Warriors 2 | DX9 | x86 | ✓ | - | - |
| Sanctum | DX9 | x86 | ✓ | - | - |
| Section 8 | DX9 | x86 | ✓ | - | - |
| Serious Sam 2 | DX9 | x86 | ✓ | - | - |
| Sexy Beach 3 | DX9 | x86 | ✓ | - | - |
| Shadowgrounds | DX9 | x86 | ✓ | - | - |
| Sid Meier's Civilization IV | DX9 | x86 | ✓ | - | - |
| Sid Meier's Railroads! | DX9 | x86 | ✓ | - | - |
| Silent Hunter 3 | DX9 | x86 | ✓ | - | - |
| Silent Hunter 4: Wolves of the Pacific | DX9 | x86 | ✓ | - | - |
| Silent Hunter 5: Battle of the Atlantic | DX9 | x86 | ✓ | - | - |
| Singularity | DX9 | x86 | ✓ | - | - |
| Sins of a Solar Empire | DX9 | x86 | ✓ | - | - |
| Spore | DX9 | x86 | ✓ | - | - |
| Star Trek: Legacy | DX9 | x86 | ✓ | - | - |
| Star Wars: Battlefront (2004) | DX9 | x86 | ✓ | - | - |
| Star Wars: Battlefront II (2005) | DX9 | x86 | ✓ | - | - |
| Starship Troopers | DX9 | x86 | ✓ | - | - |
| Street Fighter IV | DX9 | x86 | ✓ | - | - |
| Super Street Fighter IV Arcade Edition | DX9 | x86 | ✓ | - | Includes Shader Fix. See if shader fix can be applied to the 3D Vision native version. |
| Supreme Commander | DX9 | x86 | ✓ | - | - |
| Supreme Commander: Forged Alliance | DX9 | x86 | ✓ | - | - |
| Supreme Commander 2 | DX9 | x86 | ✓ | - | - |
| Test Drive Unlimited | DX9 | x86 | ✓ | - | - |
| The Ball | DX9 | x86 | ✕ | ✅ Working | Ball shadow diffrent in both eyes. |
| The Chronicles of Narnia: The Lion, the Witch and the Wardrobe | DX9 | x86 | ✓ | - | (Guess based on 'Narnia' and 'Narnia.exe') |
| The Elder Scrolls IV: Oblivion | DX9 | x86 | ✓ | - | - |
| The Movies (2005) | DX9 | x86 | ✓ | - | - |
| The Settlers II: 10th Anniversary | DX9 | x86 | ✓ | - | - |
| The Sims 2: University | DX9 | x86 | ✓ | - | Targets "Sims2EP1.exe", so not sure if it's only this expansion pack or all of The Sims 2. |
| The Sims 3 | DX9 | x86 | ✓ | - | - |
| The Sims Medieval | DX9 | x86 | ✓ | - | - |
| The Witcher | DX9 | x86 | ✓ | - | - |
| The Witcher 2: Assassins of Kings | DX9 | x86 | ✓ | - | Might includes Shader Fix. See if shader fix can be applied to the 3D Vision native version. |
| TimeShift | DX9 | x86 | ✓ | - | - |
| Titan Quest | DX9 | x86 | ✓ | - | - |
| TOCA Race Driver 3 | DX9 | x86 | ✓ | - | - |
| Tom Clancy's Ghost Recon Advanced Warfighter 2 | DX9 | x86 | ✓ | - | - |
| Tom Clancy's Rainbow Six: Vegas | DX9 | x86 | ✓ | - | - |
| Tom Clancy's Rainbow Six: Vegas 2 | DX9 | x86 | ✓ | - | - |
| Tom Clancy's Splinter Cell: Double Agent | DX9 | x86 | ✓ | - | - |
| Tom Clancy's Splinter Cell: Conviction | DX9 | x86 | ✓ | - | - |
| Tomb Raider: Legend | DX9 | x86 | ✓ | - | - |
| Tomb Raider: Anniversary | DX9 | x86 | ✓ | - | - |
| Tomb Raider: Underworld | DX9 | x86 | ✓ | - | - |
| Torchlight | DX9 | x86 | ✓ | - | - |
| TrackMania Nations Forever | DX9 | x86 | ✓ | - | - |
| Trine (2009) | DX9 | x86 | ✓ | - | - |
| Two Worlds | DX9 | x86 | ✓ | - | - |
| Unreal Tournament 3 | DX9 | x86 | ✓ | - | - |
| Virtua Tennis 2009 | DX9 | x86 | ✓ | - | - |
| Wallace & Gromit's Grand Adventures | DX9 | x86 | ✓ | - | Episodes 1-4 |
| Wanted: Weapons of Fate | DX9 | x86 | ✓ | - | - |
| Warhammer 40,000: Dawn of War | DX9 | x86 | ✓ | - | - |
| Warhammer 40,000: Dawn of War: Dark Crusade | DX9 | x86 | ✓ | - | - |
| Warhammer 40,000: Dawn of War: Soulstorm | DX9 | x86 | ✓ | - | - |
| Warhammer 40,000: Dawn of War II | DX9 | x86 | ✓ | - | - |
| Watchmen: The End is Nigh | DX9 | x86 | ✓ | - | - |
| Wings of Prey | DX9 | x86 | ✓ | - | - |
| Wolfenstein (2009) | DX9 | x86 | ✓ | - | - |
| WorldShift | DX9 | x86 | ✓ | - | - |
| Zeno Clash | DX9 | x86 | ✓ | - | - |

## DirectX 9 64bit Game List

| Game | API | Bits | iZ3D Profile | Result | Notes |
|------|-----|---------|--------|-------|-------|
| Chess Titans | DX9 | x86/x64 | ✕ | - | - |
| Counter-Strike: Source | OpenGL/DX9 | x86/x64 | ✓ | - | - |
| Day of Defeat: Source | DX9 | x86/x64 | ✓ | - | - |
| Dungeons & Dragons Online | DX9 | x86/x64 | ✓ | - | - |
| EVE Online | DX9 | x86/x64 | ✓ | - | - |
| EverQuest | DX9 | x64 | ✓ | - | - |
| EverQuest 2 | DX9 | x64 | ✓ | - | - |
| Evolution GT | DX9 | x64 | ✕ | - | - |
| Far Cry | DX9 | x86/x64 | ✓ | - | - |
| Flyff (Fly For Fun) | DX9 | x86/x64 | ✓ | - | - |
| Half-Life 2: Deathmatch | DX9 | x86/x64 | ✓ | - | - |
| REFLEX XTR² | DX9 | x86/x64 | ✓ | - | - |
| RIFT | DX9 | x86/x64 | ✓ | - | - |
| Starcraft II | DX9 | x86/x64 | ✓ | - | - |
| Team Fortress 2 | DX9 | x86/x64 | ✓ | - | - |
| Unreal Tournament 2004 | DX9 | x64 | ✓ | ✅ Working | - |

## DirectX 10/11 Game List

| Game | API | Bits | iZ3D Profile | Result | Notes |
|------|-----|---------|--------|-------|-------|
| Age of Conan: Unchained | DX9/DX10 | x86 | ✓ | - | - |
| Aliens vs. Predator (2010) | DX9/DX11 | x86 | ✓ | - | - |
| Assassins Creed | DX9/DX10 | x86 | ✓ | - | - |
| Battlefield: Bad Company 2 | DX9/DX10/DX11 | x86 | ✓ | - | Might include Shader Fix. See if shader fix can be applied to the 3D Vision native version. |
| BioShock | DX9/DX10 | x86 | ✓ | - | - |
| BioShock 2 | DX9/DX10 | x86 | ✓ | - | - |
| Call of Juarez: Bound in Blood | DX9/DX10 | x86 | ✓ | - | - |
| Champions Online | DX9/DX11 | x86 | ✕ | - | - |
| Company of Heroes | DX9/DX10 | x86 | ✓ | - | - |
| Cryostasis | DX9/DX10 | x86 | ✓ | - | - |
| Crysis | DX9/DX10 | x86/x64 | ✓ | - | - |
| Crysis: Warhead | DX9/DX10 | x86/x64 | ✓ | - | - |
| Crysis 2 | DX9/DX11 | x86/x64 | ✓ | - | - |
| DC Universe Online | DX9/DX11 | x86 | ✓ | - | - |
| DCS: Black Shark | DX9/DX11 | x86 | ✓ | - | - |
| De Blob | DX11 | x86 | ✓ | - | - |
| Deus Ex: Human Revolution | DX9/DX11 | x86 | ✓ | - | Includes Shader Fix. See if shader fix can be applied to the HD3D native version and Directors Cut version. |
| Devil May Cry 4 (2008) | DX9/DX10 | x86/x64 | ✓ | - | - |
| Dirt 3 | DX9/DX11 | x86 | ✓ | - | Includes Shader Fix. See if shader fix can be applied to the HD3D native version and Complete Edition version. |
| Dragon Age II | DX9/DX11 | x86 | ✓ | - | - |
| Entropia Universe | DX11 | x64 | ✕ | - | - |
| Far Cry 2 | DX9/DX10 | x86 | ✓ | - | - |
| F.E.A.R. 3 | DX9/DX10 | x86 | ✓ | - | - |
| Final Fantasy 14 | DX11 | x86 | ✕ | - | - |
| Gears of War | DX9/DX10 | x86 | ✓ | - | - |
| Homefront | DX9/DX11 | x86 | ✓ | - | - |
| Just Cause 2 | DX10 | x86 | ✓ | - | - |
| Lost Planet | DX9/DX10 | x86 | ✓ | - | - |
| Lost Planet 2 | DX9/DX11 | x86 | ✓ | - | - |
| Medal of Honor (2010) | DX9/DX11 | x86 | ✓ | - | Might includes Shader Fix. See if shader fix can be applied to the 3D Vision native version. |
| Metro 2033 | DX9/DX11 | x86 | ✓ | - | - |
| Microsoft Flight Simulator X | DX9/DX10 | x86 | ✓ | - | - |
| NecroVisioN | DX9/DX10 | x86 | ✓ | - | - |
| Red Faction: Guerrilla | DX9/DX10/DX11 | x86 | ✓ | - | - |
| S.T.A.L.K.E.R.: Clear Sky | DX9/DX10 | x86 | ✓ | - | - |
| S.T.A.L.K.E.R.: Call of Pripyat | DX9/DX10/DX11 | x86/x64 | ✓ | - | - |
| Serious Sam HD: The First Encounter | DX9/DX11/DX12 | x86 | ✓ | - | - |
| Sid Meier's Civilization V | DX9/DX11 | x86 | ✓ | - | - |
| The Lord of the Rings Online: Shadows of Angmar | DX9/DX10/DX11 | x86/x64 | ✓ | - | MMO. Not sure if this convers the modern The Lord of the Rings Online version. |
| Tom Clancy's H.A.W.X | DX9/DX10 | x86 | ✓ | - | - |
| Tom Clancy's H.A.W.X. 2 | DX9/DX11 | x86 | ✓ | - | - |
| Warcraft III: Reign of Chaos | OpenGL/DX8/DX9/DX11 | x86/x64 | ✓ | - | - |
| World in Conflict | DX9/DX10 | x86 | ✓ | - | - |
| World of Tanks | DX11 | x64 | ✓ | - | - |

## iZ3D Games Not Supported

| Game | API | Bits | Profile | Result | Notes |
| ~~Darkspore~~ | DX9 | x86 | ✓ | - | Game not playable until [Resurrection Capsule](https://github.com/vitor251093/resurrection-capsule) project completes. |
| ~~Fury (2007)~~ | DX9 | x86 | ✓ | - | MMO shut down 2008 |
| ~~Global Agenda~~ | DX9 | x86 | ✓ | - | - |
| ~~Monster Hunter Frontier Online~~ | DX9 | x86 | ✓ | - | - |
| ~~TERA Online~~ | DX11 | x86/x64 | ✓ | - | - |
| ~~Warhammer Online: Age of Reckoning~~ | - | x86 | ✓ | - | - |

### Nvidia 3D Vision "Ready" Native Game List

| Game | API | Bits | Result | Notes |
|------|-----|------|--------|-------|
| Aion: The Tower of Eternity | DX9 | x86 | - | MMO. |
| Batman: Arkham Asylum | DX9 | x86 | - | - |
| Batman: Arkham City | - | x86/x64 | - | - |
| Batman: Arkham Origins | - | x86/x64 | - | - |
| Battlefield: Bad Company 2 | DX9/DX10/DX11 | x86 | - | - |
| Battlefield: Bad Company 2: Vietnam | - | x86/x64 | - | - |
| Brave | - | x86/x64 | - | - |
| Call of Duty: Black Ops | DX9 | x86 | - | - |
| Carrier Command: Gaea Mission | - | x86/x64 | - | - |
| Civilization V | - | x86/x64 | - | - |
| Diablo III | DX9/DX11 | x86/x64 | - | Native NVAPI cursor/UI support, relies on driver for stereo. |
| Dead Rising 2 | - | x86/x64 | - | - |
| Deep Black | - | x86/x64 | - | - |
| Deep Black: Reloaded | - | x86/x64 | - | - |
| Depth Hunter | - | x86/x64 | - | - |
| DiRT 2 | DX11 | x86 | - | Also HD3D, but not yet fully working. |
| DiRT 3 | DX11 | x86 | - | Also HD3D, but not yet fully working. |
| DiRT 3 Complete Edition | DX11 | - | Also HD3D, but not yet fully working. |
| DiRT Showdown | DX11 | x86 | - | Also HD3D, but not yet fully working. |
| DiRT Rally | DX11 | x86 | - | Also HD3D, but not yet fully working. |
| Duke Nukem Forever | - | x86/x64 | - | - |
| Google Earth | OpenGL/DX9 | x86 | - | [Google Earth Pro 7.1.5.1557](https://web.archive.org/web/20171014110844/https://dl.google.com/earth/client/GE7/release_7_1_8/googleearth-win-pro-7.1.8.3036.exe) |
| GRID 2 | DX11 | x86 | - | Also HD3D, but not yet fully working. |
| GRID Autosport | DX11 | - | Also HD3D, but not yet fully working. |
| GT Legends | - | x86/x64 | - | - |
| Hard Reset | - | x86/x64 | - | - |
| Hawken | - | x86/x64 | - | - |
| Inversion | - | x86/x64 | - | - |
| Just Cause 2 | - | x86/x64 | - | - |
| L.A. Noire | - | x86/x64 | - | - |
| Mafia II | - | x86/x64 | - | - |
| Max Payne 3 | - | x86/x64 | - | - |
| Medal of Honor (2010) | - | x86/x64 | - | - |
| Metro 2033 | - | x86/x64 | - | - |
| Nvidia Demo: Aliens vs. Triangles | - | x86/x64 | - | - |
| Nvidia Demo: Endless City | - | x86/x64 | - | - |
| Nvidia Demo: Stone Giant | - | x86/x64 | - | - |
| Nvidia Demo: Supersonic Sled | - | x86/x64 | - | - |
| Oil Rush | - | x86/x64 | - | - |
| Passion Leads Army Benchmark  | - | x86/x64 | - | MMO. |
| Pirate101 | - | x86/x64 | - | MMO. |
| Resident Evil 5 | - | x86/x64 | - | aka Biohazard 5 |
| rFactor 2 | - | x86/x64 | - | - |
| Roller Coaster Rampage | - | x86/x64 | - | - |
| Rusty Hearts | - | x86/x64 | - | MMO. |
| StarCraft II | DX9/DX11 | x86/x64 | - | Native NVAPI cursor/UI support, relies on driver for stereo. |
| Super Street Fighter IV Arcade Edition | DX9 | x86 | - | - |
| Street Fighter X Tekken | - | x86/x64 | - | - |
| Tom Clancy's H.A.W.X 2 | - | x86/x64 | - | - |
| Tomb Raider (2013) | DX11 | x86 | - | Also HD3D, but not yet fully working. |
| Unigine: Heaven Benchmark | - | x86/x64 | - | 3.0 |
| The Witcher 2: Assassins of Kings | DX9 | x86 | - | - |
| Wizard101 | - | x86/x64 | - | MMO. |
| ~~Battlefield 3~~ | DX11 | x86 | 🚫 N/A | Working via HD3D output. |
| ~~Crysis 2~~ | DX11 | x86 | 🚫 N/A | Native Depth-Map Reprojection SBS output. |
| ~~Crysis 3~~ | DX11 | x86 | 🚫 N/A | Native Depth-Map Reprojection SBS output. |
| ~~Deus Ex: Human Revolution~~ | DX11 | x86 | 🚫 N/A | Working via HD3D output. |
| ~~Deus Ex: Human Revolution Director's Cut~~ | DX11 | x86 | 🚫 N/A | Working via HD3D output. |
| ~~Deus Ex: Mankind Divided~~ | DX11 | x64 | 🚫 N/A | Native SBS output. |
| ~~DOOM 3: BFG Edition~~ | OpenGL | x86 | 🚫 N/A | Native SBS output. |
| ~~Hitman: Absolution~~ | DX11 | x86 | 🚫 N/A | Working via HD3D output. |
| ~~James Cameron's Avatar: The Game~~ | DX9/DX10 | x86 | 🚫 N/A | Native SBS output. |
| ~~Rise of the Tomb Raider~~ | DX11 | x64 | 🚫 N/A | Native SBS output. |
| ~~Sonic Generations~~ | DX9 | x86 | 🚫 N/A | Native SBS output. |
| ~~Sniper Elite V2~~ | DX11 | x86 | 🚫 N/A | Working via HD3D output. |
| ~~Sniper Elite III~~ | DX11| x86| 🚫 N/A | Working via HD3D output. |
| ~~Thief (2014)~~ | DX11 | x86/x64|  🚫 N/A | Working via HD3D output. |
| ~~Trine 1~~ | DX11 | x86 | 🚫 N/A | Native SBS output. |
| ~~Trine 2~~ | DX11 | x86 | 🚫 N/A | Native SBS output. |
| ~~Trine 3~~ | DX11 | x86 | 🚫 N/A | Native SBS output. |
| ~~World of Warcraft~~ | DX11 | x86 | 🚫 N/A | MMO, not advised to sue wrapper. Stereo3D support was removed in the game's 2018 DX12 update. |


---

## License & Commercial Use

The original iZ3D legacy code included in this repository remains under its original **MIT License**. 

All new modifications, proxy DLLs, HD3D hooks, and modernizations introduced by the **wiz3D** project are licensed under the **GNU General Public License v3.0 (GPLv3)**. 

wiz3D is free for individuals, modders, and the community to use, modify, and distribute, provided all derivative works remain open-source under the GPLv3.

### Commercial Licensing (Hardware Vendors & OEMs)
If you are a hardware manufacturer, display vendor, or software company wishing to integrate the wiz3D/HD3D stereoscopic wrapper into a proprietary, closed-source product or launcher without the restrictions of the GPLv3, **Commercial B2B Licenses are available.** Please contact the repository owner to discuss a commercial exemption license, custom hardware integration, or Service Level Agreements (SLAs).
