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

This project modernizes that source code, replaces kernel-level hooks with a proxy DLL loader, and expands the scope to re-enable stereoscopic 3D in games that have native AMD HD3D and Nvidia 3D Vision support.

## Current Status

* **AMD HD3D:** ✅ **Mostly Working!** HD3D games render stereo interally, so all that's needed is proxy to enable that rendering, capture the quad buffer output, and display it using modern stereo3D standards. The Proxy chain is successfully triggering stereo3D and capturing the quad buffer output. All that remains is getting that quad buffer output to display corrently in modern formats like Top-and-Bottom and Side-by-Side. Currently Half TAB and Half SBS is supported with about half the games, the other games still need work displaying the output correctly.
* **OpenGL Quad-Buffer Stereo:** ✅ **Partially Worikng.** Similar to HD3D and 3D Vision 'Direct Mode', OpenGL Quad-BufferStereo acts as a ssurface for games that supported OpenGL Quad -buffer to display their stereo 3D onto, and then convert it to display onto any screen. This tends to be legacy older titles such as _Quake_ and _American McGee's Alice_. This does not inject stereo into OpenGL games.
* **Nvidia 3D Vision 'Direct Mode':** ✅ **Partially Working.** DirectX11 games currently working, DX9 and 10 games to com. Tomb Raider not wearing to SR currently.
* **Nvidia 3D Vision 'Automatic Mode':** ✅ **Partially Working.** These are games that rlied on Nvidia's driver stereo injection. The ones listes speciffically have 3D Vision ingame settings, shader fixes and config settings. current aim for these is to inject stereo via iZ3D instead, and make the ingame settings and shaderfixes apply to iZ3D. Currently, enable/disable settings do work. But convergance and seperations liders aren't hooked up to iZ3D, and shader fixes arnt triggering yet.
* **DirectX 9:** ✅ **Mostly Working!** `d3d9.dll` proxy loader works! Left 4 Dead 2 and many others run in full stereo3D, outputs in all originally supported formats, and the profile system loads shader fixes and stereo settings for all originally supported games.
* **DirectX 8:** ✅ **Partially Worikng.** Wrapper to convert DX8 to use DX9's stereoization. One tested game working, needs further testing.
* **DirectX 7:** ⚠️ **In Progress.** Wrapper to convert DX7 to use DX9's stereoization. DX7 to DX9 converstion is working. Here for testing.
* **DirectX 10/11:** ⚠️ **Partial.** The DX10/11 wrapper was never completely finished by iZ3D Inc. Some games work, but implementation in wiz3D still has some way to go and hasn't got any games booting with stereo3D initialised yet.

---

## Getting Started Playing

1. Launch game before downlaoding, and set the resolution and refreshrate, and make sure fullscreen is enabled.
1. Download the latest release from the **Releases** tab.
2. Find the executable (`.exe`) of the game you want to play, and check the API, Bitness and HD3D/3DVision support so you know which files to use.
3. Copy the **contents** of the appropriate `wiz3D` subfolder (e.g., the contents of the `dx9` > `x86` folder for a DX9 32-bit game) directly into the folder containing the game's `.exe`.
4. Launch the game. Stereo 3D should activate automatically! If it's an HD3D game, you may need to activate `stereoscopic 3D` or `HD3D` in the in-game menu.

Configure your output mode (Half Side-by-Side, Anaglyph, etc.) and any other settings you'd like to modify using the included `Config.xml` file.

---

## wiz3D Game Test Results

*Legend:* ✅ ***Working** = Stereo output activated and playable.* ⚠️ ***Partial** = Stereo activated but with issues (crash, wrong settings, shader problems).* ❌ ***Not loading** = Wrapper not activating. **Untested** = I haven't tested yet or don't have access to that game*

### Nvidia 3D Vision "Direct Mode" Games 
Games that render Stereoscipic 3D themselves and display that via 3D Vision's 'Direct Mode'.

| Game | API | Bits | Testing | Notes |
|------|-----|------|--------|-------|
| Hard Reset | DX9 | x86 | Untested | Only DX11 build released as of now. |
| Hitman: Absolution | DX11 | x86 | ✅ Working | Game supports 3D Vision Direct Mode and HD3D. |
| Oil Rush | OpenGL/DX9/DX11 | x86 | ✅ Working | DX11 output only. |
| Tomb Raider (2013) | DX11 | x86 | ✅ Mostly Working | SR weave output not working, other 3D outputs working. Game supports 3D Vision Direct Mode and HD3D. |

- **Excluded (Depth Map not loading, better support on HD3D):** *Battlefield 3*, *DiRT 2*, *DiRT 3*, *DiRT 3 Complete Edition*, *DiRT Showdown*, *DiRT Rally*, *F1 2010*, *F1 2011*, *F1 2012*, *F1 2013*, *GRID 2*, *GRID Autosport*.
- **Excluded (Works on HD3D and 3D Vision Direct Mode, HD3D reccomended):** *World of Warcraft*. <sub>(Stereo3D removed in 2018 DX12 update)</sub>

### AMD HD3D Native Games

| Game | API | Bits | Testing | Notes |
|------|-----|------|--------|-------|
| Battlefield 3 | DX11 | x86 | ✅ Mostly Working | Campaign only. Strange depth issue in the game not seeming caused by wiz3D. Might need shader fix. |
| Deus Ex: Human Revolution | DX11 | x86 | ✅ Working | Check iZ3D Shader Fix. |
| Deus Ex: Human Revolution Director's Cut | DX11 | x86 | ✅ Working | Undocumented AMD HD3D support.<br>Skybox at wrong depth, check iZ3D Shader Fix. |
| DiRT 2 | DX11 | x86 | Untested | `hardware_settings_config.xml` needs `stereo enabled="true"`. |
| DiRT 3 | DX11 | x86 | Untested | `hardware_settings_config.xml` needs `stereo enabled="true"`.<br>Check iZ3D Shader Fix. |
| DiRT 3 Complete Edition | DX11 | x86 | ⚠️ Partial | `hardware_settings_config.xml` needs `stereo enabled="true"`.<br>Only top half of Half SBS visible. |
| DiRT Showdown | DX11 | x86 | ⚠️ Partial | `hardware_settings_config.xml` needs `stereo enabled="true"`.<br>Only top half of Half SBS visible. |
| DiRT Rally | DX11 | x86 | ⚠️ Partial | `hardware_settings_config.xml` needs `stereo enabled="true"`.<br>Only top half of Half SBS visible. |
| F1 2010 | DX11 | x86 | Untested |  |
| F1 2011 | DX11 | x86 | Untested |  |
| F1 2012 | DX11 | x86 | Untested |  |
| F1 2013 | DX11 | x86 | Untested |  |
| GRID 2 | DX11 | x86 | ⚠️ Partial | `hardware_settings_config.xml` needs `stereo enabled="true"`.<br>Only top half of Half SBS visible. |
| GRID Autosport | DX11 | x86 | ⚠️ Partial | `hardware_settings_config.xml` needs `stereo enabled="true"`.<br>Only top half of Half SBS visible. |
| Sleeping Dogs | DX11 | x86 | ✅ Working |  |
| Sleeping Dogs: Definitive Edition | DX11 | x64 | ✅ Working | Undocumented AMD HD3D support. |
| Sniper Elite V2 | DX11 | x86 | ✅ Working | Color shifting when seperation increased.<br>[Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Sniper Elite III | DX11| x86| ✅ Working | `customersupportlogging` beta branch only.<br>[Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Sniper Elite 4 | DX11/12| x64| ✅ Working | DX11 only. DX12 doens't seem to trigger HD3D stereo.<br>[Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Sniper Elite: Nazi Zombie Army | DX11 | x86 | ✅ Working | Color shifting when seperation increased.<br>[Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Sniper Elite: Nazi Zombie Army 2 | DX11 | x86 | ✅ Working | Color shifting when seperation increased.<br>[Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Zombie Army Trilogy | DX11 | x86 | ✅ Working | [Depth Map Reprojection](https://www.pcgamingwiki.com/wiki/Glossary:Native_3D#Depth_Map_Reprojection). |
| Thief (2014) | DX11 | x86/x64| ✅ Working | Both x86 and x64 working. |

- **Excluded (Works on 3D Vision Direct Mode):** *Hitman: Absolution*, *Tomb Raider (2013)*.
- **Excluded (Native SBS/TAB):** *Crysis 2*, *Crysis 3*, *Rise of the Tomb Raider*, *Shadwen*, *Two Worlds II*, *Trine 1*, *Trine 2*, *Trine 3*.
- **Excluded (Online Ban Risk):** *World of Warcraft*. <sub>(Stereo3D removed in 2018 DX12 update)</sub>

### DirectX 7/8 Games

| Game | API | Bits | iZ3D Profile | Testing | Notes |
|------|-----|---------|--------|-------|-------|
| AquaNox | DX8 | x86 | ✕ | ✅ Working |  |
| Ballance | DX8 | x86 | ✓ | Untested |  |
| Command & Conquer: Generals | DX8/DX9 | x86 | ✓ | ⚠️ Partial | Single player only. Partially works on DX9 with Tiberian Technologies patch, but needs shaderfix. |
| Command & Conquer: Renegade | DX8 | x86 | ✓ | Untested | Single player only. |
| Deus Ex | OpenGL/DX7 | x86 | ✕ | ❌ Not Working | listed for DX7 testing. |
| Deus Ex: Invisible War | DX8 | x86 | ✕ | ⚠️ Partial | Startsup, gets into menu on Linux. |
| Duke Nukem: Manhattan Project | OpenGL/DX8 | x86 | ✕ | ✅ Working | May need convergance and seperation adjusting for best settings. |
| FATE | DX8 | x86 | ✕ | Untested |  |
| Mega Man X8 | DX8 | x86 | ✓ | Untested |  |
| Mercedes-Benz World Racing | DX8 | x86 | ✓ | Untested |  |
| Sniper Elite | DX8 | x86 | ✓ | ❌ Not Working | Game crashes. |
| The Lord of the Rings: The Return of the King | DX8 | x86 | ✓ | Untested |  |
| Thief: Deadly Shadows | DX8 | x86 | ✓ | ⚠️ Partial | In-game, also heavy artifacts |
| Tony Hawk's Pro Skater 3 | DX8 | x86 | ✓ | Untested |  |

- **Excluded (Online Ban Risk):** *Empire Earth II*, *Freelancer*, *GTR - FIA GT Racing Game*, *NASCAR Racing 2003 Season*, *Tom Clancy's Rainbow Six 3: Raven Shield*. <sub>(Active community servers with stringent anti-cheat)</sub>

## DirectX 9 32bit Games

| Game | API | Bits | iZ3D Profile | Testing | Notes |
|------|-----|---------|--------|-------|-------|
| Antichamber | DX9 | x86 | ✕ | ✅ Working | Might need shader fix. |
| A.R.E.S.: Extinction Agenda | DX9 | x86 | ✓ | Untested |  |
| AaAaAA!!! A Reckless Disregard for Gravity | DX9 | x86 | ✓ | Untested |  |
| Alan Wake | DX9 | x86 | ✕ | ❌ Not Working | Crashes on title screen. |
| Alien Breed 2: Assault | DX9 | x86 | ✓ | Untested |  |
| Alien Swarm | DX9 | x86 | ✓ | Untested |  |
| Aliens Versus Predator | DX6/DX9 | x86 | ✕ | ❌ Not Working | Crashes on level load. |
| Alone in the Dark (2008) | DX9 | x86 | ✓ | Untested |  |
| America's Army | DX8/DX9 | x86 | ✓ | Untested |  |
| Anomaly Warzone Earth | DX9 | x86 | ✓ | Untested |  |
| AquaNox 2: Revelation | DX8/DX9 | x86 | ✓ | ✅ Working |  |
| Arma: Armed Assault | DX9 | x86 | ✓ | Untested | aka ARMA: Combat Operations  |
| Armies of Exigo | DX9 | x86 | ✓ | Untested |  |
| Assassin's Creed II | DX9 | x86 | ✓ | Untested |  |
| Assassin's Creed: Brotherhood | DX9 | x86 | ✓ | Untested |  |
| Audiosurf | DX9 | x86 | ✓ | Untested |  |
| Back to the Future: The Game | DX9 | x86 | ✓ | Untested | Episode 1-5 |
| Batman: Arkham Origins Blackgate | DX9 | x86 | ✕ | ✅ Working | Might need shader fix. |
| Battlefield 2 | DX9 | x86 | ✓ | Untested |  |
| Battlefield 2142 | DX9 | x86 | ✓ | Untested |  |
| Battlestations: Pacific | DX9 | x86 | ✓ | Untested |  |
| Beowulf: The Game | DX9 | x86 | ✓ | Untested |  |
| Bet on Soldier | DX9 | x86 | ✓ | Untested |  |
| Beyond Good & Evil | DX9 | x86 | ✓ | ✅ Mostly Working | Artifacts when paused. |
| Bayonetta | DX9 | x86 | ✕ | ✅ Working | Performance issues. Might need shader fix. |
| Bionic Commando (2009) | DX9 | x86 | ✕ | ✅ Partially Working | Needs HUD and shadow shader fix, fullscreen exclusive, crashes on AMD. |
| Bionic Commando Rearmed | DX9 | x86 | ✓ | Untested |  |
| Black & White 2 | DX9 | x86 | ✓ | Untested |  |
| Black Mesa | DX9 | x86 | ✕ |  ❌ Not Working | Crashing. |
| BlazBlue: Calamity Trigger | DX9 | x86 | ✕ | ✅ Mostly Working | Needs sprite depth shader fix, use [Geo-11 Fix instead](https://helixmod.blogspot.com/2026/05/blazblue-calamity-trigger-dx11.html). |
| Blur | DX9 | x86 | ✕ | ✅ Working | May need shader fix. |
| Borderlands | DX9 | x86 | ✓ | Untested |  |
| Brothers in Arms: Road to Hill 30 | DX9 | x86 | ✓ | Untested |  |
| Brothers in Arms: Earned in Blood | DX9 | x86 | ✓ | Untested |  |
| Brothers in Arms: Hell's Highway | DX9 | x86 | ✓ | Untested |  |
| Bulletstorm | DX9 | x86 | ✓ | Untested |  |
| Burnout Paradise | DX9 | x86 | ✓ | ✅ Working |  |
| Cabela's Big Game Hunter: 10th Anniversary Edition | DX9 | x86 | ✓ | Untested | Not sure which game, profile says "Cabela Big Game Hunter BGH10.exe". |
| Call of Cthulhu: Dark Corners of the Earth | DX9 | x86 | ✓ | Untested |  |
| Call of Duty 2 | DX9 | x86 | ✓ | Untested |  |
| Call of Duty: World at War | DX9 | x86 | ✓ | Untested |  |
| Call of Duty 4: Modern Warfare (2007) | DX9 | x86 | ✓ | Untested |  |
| Call of Duty: Modern Warfare 2 | DX9 | x86 | ✓ | Untested |  |
| Cars | DX9 | x86 | ✓ | Untested |  |
| Castlevania: Lords of Shadow 2 | DX9 | x86 | ✕ | ✅ Mostly Working | Needs shadow shader fix. |
| Chromadrome 2 | DX9 | x86 | ✓ | Untested |  |
| Command & Conquer 3: Tiberium Wars | DX9 | x86 | ✓ | ✅ Working | Place in `/RetailExe/1.10`. Game crashes when changing settings. |
| Command & Conquer: Red Alert 3 | DX9 | x86 | ✓ | ✅ Working | Pleace in `/Data/` folder. |
| Command & Conquer 4: Tiberian Twilight | DX9 | x86 | ✓ | Untested |  |
| Condemned: Criminal Origins | DX9 | x86 | ✓ | Untested |  |
| Demigod | DX9 | x86 | ✕ | Untested |  |
| Damnation | DX9 | x86 | ✓ | Untested |  |
| Dark Messiah of Might and Magic | DX9 | x86 | ✓ | ✅ Working | Might need to use [Mod Launcher](https://steamcommunity.com/sharedfiles/filedetails/?id=3378556750). |
| Dark Void | DX9 | x86 | ✓ | Untested |  |
| Dead or Alive 5 Last Round | DX9 | x86 | ✓ | ✅ Working |  |
| Deadpool | DX9 | x86 | ✕ | ✅ Working | Might need shader fix |
| Dead Space | DX9 | x86 | ✓ | ⚠️ Partial | Crashes past Main Menu. |
| Dead Space 2 | DX9 | x86 | ✓ | ✅ Working |  |
| Dead Space 3 | DX9 | x86 | ✕ | ✅ Mostly Working | Needs HUD Shader fix. |
| Defense Grid: The Awakening | DX9 | x86 | ✓ | Untested |  |
| Delta Force: Xtreme | DX9 | x86 | ✓ | Untested |  |
| Devil May Cry 3: Special Edition | DX9 | x86 | ✓ | Untested |  |
| DiRT | DX9 | x86 | ✓ | Untested |  |
| Disciples III: Renaissance | DX9 | x86 | ✓ | Untested |  |
| Divinity II: Ego Draconis  | DX9 | x86 | ✓ | Untested |  |
| Dragon Age: Origins | DX9 | x86 | ✓ | Untested |  |
| Drakensang: The Dark Eye | DX7/DX9 | x86 | ✓ | Untested |  |
| Driver: San Francisco | DX9 | x86 | ✕ | ✅ Working |  |
| Dungeons (2011) | DX9 | x86 | ✕ | Untested |  |
| Dungeon Siege II | DX9 | x86 | ✓ | Untested |  |
| Dungeon Siege III | DX9 | x86 | ✓ | Untested |  |
| Empire: Total War | DX9 | x86 | ✓ | Untested |  |
| Enemy Engaged 2 | DX9 | x86 | ✕ | Untested |  |
| Eragon | DX9 | x86 | ✓ | Untested |  |
| Evolution GT | DX9 | x86 | ✕ | Untested |  |
| Fable: The Lost Chapters | DX9 | x86 | ✓ | ⚠️ Partial | Has artifacts, needs shader fix. |
| Fable III | DX9 | x86 | ✓ | Untested |  |
| Fable Anniversary | DX9 | x86 | ✕ | ⚠️ Partial | Has artifacts, needs shader fix. |
| Fahrenheit | DX9 | x86 | ✓ | Untested | aka Indigo Prophecy |
| Fallout 3 | DX9 | x86 | ✓ | ✅ Working |  |
| Fallout: New Vegas | DX9 | x86 | ✕ | ✅ Mostly Working | Needs Skybox & Pip-Boy UI shader fix |
| F.E.A.R. | DX9 | x86 | ✓ | ❌ Not Working | Crashes on launch. |
| F.E.A.R. Perseus Mandate | DX9 | x86 | ✓ | Untested |  |
| F.E.A.R. 2: Project Origin | DX9 | x86 | ✓ | Untested |  |
| FIFA 10 | DX9 | x86 | ✕ | Untested |  |
| FIFA 14 | DX9 | x86 | ✕ | ✅ Working |  |
| Flashback (2013) | DX9 | x86 | ✕ | ✅ Working | May need shader fix. |
| FlatOut | DX9 | x86 | ✓ | Untested |  |
| FlatOut 2 | DX9 | x86 | ✕ | Untested |  |
| FlatOut: Ultimate Carnage | DX9 | x86 | ✓ | Untested |  |
| Foreign Legion: Buckets of Blood | DX9 | x86 | ✕ | Untested |  |
| Frontlines: Fuel of War | DX9 | x86 | ✓ | Untested |  |
| Front Mission Evolved | DX9 | x86 | ✕ | Untested |  |
| Fuel | DX9 | x86 | ✕ | ✅ Working | Might need shader fix |
| G-Force | DX9 | x86 | ✓ | Untested |  |
| Garshasp: The Monster Slayer | DX9 | x86 | ✓ | Untested |  |
| Ghostbusters: The Video Game | DX9 | x86 | ✓ | Untested |  |
| Ghostbusters: Sanctum of Slime | DX9 | x86 | ✓ | Untested |  |
| Google Earth | OpenGL/DX9 | x86 | ✓ | Untested | [Google Earth Pro 7.1.5.1557](https://web.archive.org/web/20171014110844/https://dl.google.com/earth/client/GE7/release_7_1_8/googleearth-win-pro-7.1.8.3036.exe) |
| Gothic 3 | DX9 | x86 | ✕ | ✅ Mostly Working | Sky at screen depth, needs shader fix. |
| Grand Theft Auto: San Andreas | DX9 | x86 | ✓ | ✅ Working |  |
| GRID | DX9 | x86 | ✓ | Untested |  |
| Guitar Hero III: Legends of Rock | DX9 | x86 | ✓ | Untested |  |
| Half-Life 2 | DX9 | x86 | ✓ | ✅ Mostly Working | `steam_legacy` beta branch. Use `-game ` command line argument. Shadows have issues. |
| Halo: Combat Evolved | DX9 | x86 | ✕ | ❌ Not Working | Crashes on load. |
| Heroes of Might and Magic V | DX9 | x86 | ✓ | Untested |  |
| Hitman: Blood Money | DX9 | x86 | ✓ | ✅ Working | Needs shadow and water shader fix |
| Hunted: The Demon's Forge | DX9 | x86 | ✓ | Untested |  |
| Injustice: Gods Among Us | DX9 | x86 | ✕ | ✅ Working | Might need shader fix. |
| Insane 2 | DX9 | x86 | ✕ | ❌ Not Working |  |
| Jet Set Radio | DX9 | x86 | ✕ | ❌ Not Working | Only works in mono. |
| Kane & Lynch 2: Dog Days | DX9 | x86 | ✓ | ✅ Working |  |
| Killing Floor | DX8/DX9 | x86 | ✓ | Untested |  |
| King Arthur: The Role-Playing Wargame | DX9 | x86 | ✓ | Untested | Specifies 'King Arthur: The Druids' expansion pack |
| King's Bounty: The Legend | DX9 | x86 | ✓ | Untested |  |
| Left 4 Dead | DX9 | x86 | ✓ | ✅ Working | Only tested single player. Use `-insecure` command line argument to avoid VAC ban. |
| Left 4 Dead 2 | DX9 | x86 | ✓ | ✅ Working | Only tested single player. Use `-insecure` command line argument to avoid VAC ban. |
| Legacy of Kain: Defiance | DX9 | x86 | ✕ | ✅ Working | Needs shadow shader fix |
| Lego Star Wars: The Video Game | DX9 | x86 | ✓ | Untested |  |
| Lego Star Wars III: The Clone Wars | DX9 | x86 | ✓ | Untested |  |
| Madden NFL 08 | DX9 | x86 | ✓ | Untested |  |
| Mad Riders | DX9 | x86 | ✕ | ✅ Working | Might need shader fix. |
| Majesty 2: The Fantasy Kingdom Sim | DX9 | x86 | ✓ | Untested |  |
| Mass Effect | DX9 | x86 | ✓ | Untested |  |
| Mass Effect 2 | DX9 | x86 | ✓ | ✅ Working |  |
| Medal of Honor: Airborne | DX9 | x86 | ✓ | Untested |  |
| Mercenaries 2: World in Flames | DX9 | x86 | ✓ | Untested |  |
| Metal Gear Rising: Revengeance | DX9 | x86 | ✕ | ✅ Working | Might need shader fix. |
| Microsoft Flight Simulator 2004: A Century of Flight | DX9 | x86 | ✓ | Untested |  |
| Mini Ninjas | DX9 | x86 | ✓ | Untested |  |
| Mirror's Edge | DX9 | x86 | ✓ | ✅ Working |  |
| Monday Night Combat | DX9 | x86 | ✓ | Untested |  |
| Mortal Kombat Komplete Edition | DX9 | x86 | ✕ | ✅ Working |  |
| MTX: Mototrax | DX9 | x86 | ✓ | Untested |  |
| Mythos | DX9 | x86 | ✓ | Untested | Mythos (2009) maybe? Not sure which game this is. |
| Naruto Shippuden: Ultimate Ninja Storm 3 Full Burst (2013) | DX9 | x86 | ✕ | ✅ Mostly Working |  Needs shader fix and swap eye. |
| Need for Speed: ProStreet | DX9 | x86 | ✓ | Untested |  |
| Need for Speed: Undercover | DX9 | x86 | ✓ | Untested |  |
| Need for Speed: Hot Pursuit (2010) | DX9 | x86 | ✓ | Untested |  |
| Need for Speed: Shift | DX9 | x86 | ✓ | Untested |  |
| Need for Speed: Shift 2 Unleashed | DX9 | x86 | ✓ | ✅ Working |  |
| Ninja Blade | DX9 | x86 | ✓ | ❌ Not Working | Black screen on AMD. |
| Oddworld: New 'n' Tasty! | DX9 | x86 | ✕ | ⚠️ Partial | Heavy artifacts, needs shader fix. |
| Operation Flashpoint: Dragon Rising | DX9 | x86 | ✓ | Untested |  |
| OutRun 2006: Coast 2 Coast | DX9 | x86 | ✓ | Untested |  |
| Overlord | DX9 | x86 | ✓ | Untested |  |
| Overlord II | DX9 | x86 | ✓ | Untested |  |
| Painkiller | DX9 | x86 | ✓ | Untested |  |
| Painkiller: Overdose | DX9 | x86 | ✓ | Untested |  |
| Portal | DX9 | x86 | ✓ | ✅ Working | Use `-game Portal -dx9` command line argument. Place files in `/portal/bin/` folder. |
| Portal 2 | DX9 | x86 | ✓ | ✅ Mostly Working | Place files in `/Portal 2/bin/` folder. Seperation initially too high, needs wall display shader mod. |
| Portal Stories: Mel | DX9 | x86 | ✕ | ✅ Working | Needs partical glow, glass and portal shader fix. |
| Prince of Persia (2008) | DX9 | x86 | ✕ | ✅ Working | Might need shader fix. |
| Prince of Persia: The Forgotten Sands | DX9 | x86 | ✓ | Untested |  |
| Pro Evolution Soccer 2017 | DX9 | x86 | ✕ | ✅ Working | Might need shader fix. |
| ProtoGalaxy | DX9 | x86 | ✓ | Untested |  |
| Prototype 2 | DX9 | x86 | ✕ | ✅ Mostly Working | Needs shadows shader fix. |
| Rayman Legends | DX9 | x86 | ✕ | ✅ Working | Might need shader fix. |
| RefRain - prism memories - | DX9/DX9 | x86 | ✕ | ✅ Partially Working | Play with depth set low. Needs higher depth shader fix and issues with Simulated Reality output. |
| Richard Burns Rally | DX9 | x86 | ✓ | Untested |  |
| Rise and Fall: Civilizations at War | DX9 | x86 | ✓ | Untested |  |
| Rise of Flight | DX9 | x86 | ✓ | Untested |  |
| Rise of the Argonauts | DX9 | x86 | ✓ | Untested |  |
| Risen | DX9 | x86/x64 | ✓ | Untested |  |
| Saints Row 2 | DX9 | x86 | ✓ | Untested |  |
| Sam & Max Save the World | DX8/DX9 | x86 | ✓ | Untested | Notes Episode 4: Abe Lincoln Must Die! |
| Sam & Max: The Devil's Playhouse | DX9 | x86 | ✓ | Untested | Notes Episode 1: The Penal Zone |
| Samurai Warriors 2 | DX9 | x86 | ✓ | Untested |  |
| Sanctum | DX9 | x86 | ✓ | Untested |  |
| Section 8 | DX9 | x86 | ✓ | Untested |  |
| Sega Rally Revo | DX9 | x86 | ✕ | ✅ Working | Needs shadow shader fix. |
| Serious Sam 2 | DX9 | x86 | ✓ | Untested |  |
| Sexy Beach 3 | DX9 | x86 | ✓ | Untested |  |
| Shadowgrounds | DX9 | x86 | ✓ | Untested |  |
| Sid Meier's Civilization IV | DX9 | x86 | ✓ | Untested |  |
| Sid Meier's Railroads! | DX9 | x86 | ✓ | Untested |  |
| Silent Hunter 3 | DX9 | x86 | ✓ | Untested |  |
| Silent Hunter 4: Wolves of the Pacific | DX9 | x86 | ✓ | ❌ Not Working | Crashes on startup. |
| Silent Hunter 5: Battle of the Atlantic | DX9 | x86 | ✓ | Untested |  |
| Sine Mora EX | DX9 | x86 | ✕ | ❌ Not Working |  |
| Singularity | DX9 | x86 | ✓ | ✅ Working |  |
| Sins of a Solar Empire | DX9 | x86 | ✓ | ✅ Mostly Working | Background at wrong depth. |
| SkyDrift | DX9 | x86 | ✕ | ❌ Not Working |  |
| Sonic Adventure 2 | DX9 | x86 | ✕ | ❌ Not Working | Black screen |
| Sonic & Sega All-Stars Racing | DX9 | x86 | ✕ | ✅ Working |  |
| Sonic R (2004) | DX9 | x86 | ✕ | ✅ Mostly Working | Needs coin depth shader fix. Use with [Sonic R Updater](https://github.com/cheatfreak47/SRUpdater). |
| Spec Ops: The Line | DX9 | x86 | ✕ | ✅ Working |  |
| Split/Second: Velocity | DX9 | x86 | ✕ | ✅ Mostly Working | Needs shader fix. Aka Split/Second. |
| Spore | DX9 | x86 | ✓ | Untested |  |
| S.T.A.L.K.E.R.: Shadow of Chernobyl | DX9 | x86 | ✓ | ✅ Working |  |
| Star Trek: Legacy | DX9 | x86 | ✓ | Untested |  |
| Star Wars: Battlefront (2004) | DX9 | x86 | ✓ | Untested |  |
| Star Wars: Battlefront II (2005) | DX9 | x86 | ✓ | Untested |  |
| Star Wars: Episode I - Racer | DX6/DX9 | x86 | ✕ | ❌ Not Working | Crashes on startup. |
| Star Wars: The Force Unleashed II | DX9 | x86 | ✕ | ⚠️ Partial | Needs shader fix. |
| Starship Troopers | DX9 | x86 | ✓ | Untested |  |
| Street Fighter IV | DX9 | x86 | ✓ | Untested |  |
| Supreme Commander | DX9 | x86 | ✓ | ✅ Working |  |
| Supreme Commander: Forged Alliance | DX9 | x86 | ✓ | Untested |  |
| Supreme Commander 2 | DX9 | x86 | ✓ | Untested |  |
| Test Drive Unlimited | DX9 | x86 | ✓ | Untested |  |
| The Ball | DX9 | x86 | ✕ | ✅ Mostly Working | Ball shadow diffrent in both eyes. |
| The Chronicles of Narnia: The Lion, the Witch and the Wardrobe | DX9 | x86 | ✓ | Untested | (Guess based on 'Narnia' and 'Narnia.exe') |
| The Elder Scrolls IV: Oblivion | DX9 | x86 | ✓ | ✅ Working |  |
| The Elder Scrolls V: Skyrim | DX9 | x86 | ✕ | ❌ Not Working | Enters main menu, freezes on level load. |
| The Lord of the Rings: The Battle for Middle-earth | DX9 | x86 | ✕ | ✅ Mostly Working | Needs UI shader fix. |
| The Movies (2005) | DX9 | x86 | ✓ | Untested |  |
| The Settlers II: 10th Anniversary | DX9 | x86 | ✓ | Untested |  |
| The Sims 2: University | DX9 | x86 | ✓ | Untested | Targets "Sims2EP1.exe", might just be expansion pack or all of The Sims 2. |
| The Sims 3 | DX9 | x86 | ✓ | Untested |  |
| The Sims Medieval | DX9 | x86 | ✓ | Untested |  |
| The Witcher | DX9 | x86 | ✓ | ✅ Working | Place files in `/System/` folder. |
| TimeShift | DX9 | x86 | ✓ | Untested |  |
| Titan Quest | DX9 | x86 | ✓ | ✅ Mostly Working | Needs shadow shader fix. |
| TOCA Race Driver 3 | DX9 | x86 | ✓ | Untested |  |
| Tom Clancy's Ghost Recon Advanced Warfighter 2 | DX9 | x86 | ✓ | Untested |  |
| Tom Clancy's Rainbow Six: Vegas | DX9 | x86 | ✓ | Untested |  |
| Tom Clancy's Rainbow Six: Vegas 2 | DX9 | x86 | ✓ | Untested |  |
| Tom Clancy's Splinter Cell: Double Agent | DX9 | x86 | ✓ | Untested |  |
| Tom Clancy's Splinter Cell: Conviction | DX9 | x86 | ✓ | ✅ Mostly Working | Needs shader fix. |
| Tomb Raider: Legend | DX9 | x86 | ✓ | ✅ Mostly Working | Needs shadow shader fix. |
| Tomb Raider: Anniversary | DX9 | x86 | ✓ | ✅ Working |  |
| Tomb Raider: Underworld | DX9 | x86 | ✓ | ✅ Working |  |
| Torchlight | DX9 | x86 | ✓ | Untested |  |
| TrackMania Nations Forever | DX9 | x86 | ✓ | Untested |  |
| Trine (2009) | DX9 | x86 | ✓ | Untested |  |
| Two Worlds | DX9 | x86 | ✓ | Untested |  |
| Unreal Tournament 3 | DX9 | x86 | ✓ | Untested |  |
| Valkyria Chronicles | DX9 | x86/x64 | ✕ | ⚠️ Partial | Needs UI, reflections and shadow shader fix |
| Vanquish | DX9 | x86 | ✕ | ✅ Mostly Working | Needs crosshair shader fix. |
| Virtua Tennis 2009 | DX9 | x86 | ✓ | Untested |  |
| Virtua Tennis 4 | DX9 | x86 | ✕ | ✅ Working | Might need shader fix |
| Wallace & Gromit's Grand Adventures | DX9 | x86 | ✓ | Untested | Episodes 1-4 |
| Wanted: Weapons of Fate | DX9 | x86 | ✓ | Untested |  |
| Warhammer 40,000: Dawn of War | DX9 | x86 | ✓ | Untested |  |
| Warhammer 40,000: Dawn of War: Dark Crusade | DX9 | x86 | ✓ | Untested |  |
| Warhammer 40,000: Dawn of War: Soulstorm | DX9 | x86 | ✓ | Untested |  |
| Warhammer 40,000: Dawn of War II | DX9 | x86 | ✓ | Untested |  |
| Watchmen: The End is Nigh | DX9 | x86 | ✓ | Untested |  |
| Wheelman | DX9 | x86 | ✕ | ✅ Working | May need shader fix. |
| Wings of Prey | DX9 | x86 | ✓ | Untested |  |
| Wolfenstein (2009) | DX9 | x86 | ✓ | Untested |  |
| WorldShift | DX9 | x86 | ✓ | Untested |  |
| X-Men Origins: Wolverine | DX9 | x86 | ✕ | ✅ Working |  |
| Yaiba: Ninja Gaiden Z | DX9 | x86 | ✕ | ✅ Working |  |
| Zeno Clash | DX9 | x86 | ✓ | Untested |  |

- **Excluded (Game Not Playable):** *Darkspore*. <sub>(Game not playable til [Resurrection Capsule](https://github.com/vitor251093/resurrection-capsule) project completes.)</sub>
- **Excluded (Online Ban Risk):** *Allods Online*, *Dark Age of Camelot*, *Darkfall Online*, *Fury (2007)*, *Global Agenda*, *Guild Wars*, *Monster Hunter Frontier Online*, *Warhammer Online: Age of Reckoning*.

## DirectX 9 64bit Games

| Game | API | Bits | iZ3D Profile | Testing | Notes |
|------|-----|---------|--------|-------|-------|
| Chess Titans | DX9 | x86/x64 | ✕ | Untested | Freezes on level load. |
| Dragon Ball Xenoverse | DX9 | x64 | ✕ | ✅ Working | May need shader fix. |
| Far Cry | DX9 | x86/x64 | ✓ | ❌ Not Working | Crashes on startup. |
| Final Fantasy IX | DX9 | x64 | ✕ | ⚠️ Partial | Dual-View Output works, flat 3D though. |
| Pro Evolution Soccer 2016 | DX9 | x64 | ✕ | ✅ Working |  |
| REFLEX XTR² | DX9 | x86/x64 | ✓ | Untested |  |
| Tales of Berseria | DX9 | x64 | ✕ | ✅ Mostly Working | Not working in SR Weave outpuut, working in other 3D outputs. |
| Ultimate Marvel vs. Capcom 3 | DX9 | x64 | ✕ | ❌ Not Working |  |
| Unreal Tournament 2004 | DX9 | x64 | ✓ | ✅ Working |  |

- **Excluded (VAC Ban Risk):** *Counter-Strike: Source*, *Day of Defeat: Source*, *Half-Life 2: Deathmatch*.
- **Excluded (Online Ban Risk):** *Dungeons & Dragons Online*, *EVE Online*, *EverQuest*, *EverQuest 2*, *Flyff (Fly For Fun)*, *RIFT*, *Starcraft II*.

## DirectX 10/11 Games (Testing Release)

| Game | API | Bits | iZ3D Profile | Testing | Notes |
|------|-----|---------|--------|-------|-------|
| Aliens vs. Predator (2010) | DX9/DX11 | x86 | ✓ | ✅ DX9 Working | Needs shadow shader fix on DX9. |
| Assassins Creed | DX9/DX10 | x86 | ✓ | Untested |  |
| BioShock | DX9/DX10 | x86 | ✓ | Untested |  |
| BioShock 2 | DX9/DX10 | x86 | ✓ | Untested |  |
| Call of Juarez: Bound in Blood | DX9/DX10 | x86 | ✓ | Untested |  |
| Company of Heroes | DX9/DX10 | x86 | ✓ | Untested |  |
| Cryostasis | DX9/DX10 | x86 | ✓ | Untested |  |
| Crysis | DX9/DX10 | x86/x64 | ✓ | Untested |  |
| Crysis: Warhead | DX9/DX10 | x86/x64 | ✓ | Untested |  |
| Crysis 2 | DX9/DX11 | x86/x64 | ✓ | Untested | Includes Shader Fix. See if can be applied to the game's native SBS. |
| DCS: Black Shark | DX9/DX11 | x86 | ✓ | Untested | Single Player may be okay. Multiplayer not recommended. |
| De Blob | DX11 | x86 | ✓ | Untested |  |
| Dragon Age II | DX9/DX11 | x86 | ✓ | Untested |  |
| Far Cry 2 | DX9/DX10 | x86 | ✓ | Untested |  |
| F.E.A.R. 3 | DX9/DX10 | x86 | ✓ | Untested |  |
| Gears of War | DX9/DX10 | x86 | ✓ | Untested |  |
| Homefront | DX9/DX11 | x86 | ✓ | Untested |  |
| Lost Planet | DX9/DX10 | x86 | ✓ | Untested |  |
| Microsoft Flight Simulator X | DX9/DX10 | x86 | ✓ | Untested |  |
| NecroVisioN | DX9/DX10 | x86 | ✓ | Untested |  |
| Red Faction: Guerrilla | DX9/DX10/DX11 | x86 | ✓ | Untested |  |
| S.T.A.L.K.E.R.: Clear Sky | DX9/DX10 | x86 | ✓ | Untested |  |
| S.T.A.L.K.E.R.: Call of Pripyat | DX9/DX10/DX11 | x86/x64 | ✓ | Untested |  |
| Serious Sam HD: The First Encounter | DX9/DX11/DX12 | x86 | ✓ | Untested |  |
| Tom Clancy's H.A.W.X | DX9/DX10 | x86 | ✓ | Untested |  |
| Tom Clancy's Splinter Cell: Blacklist | DX9/DX11 | x86 | ✕ | ✅ Mostly Working | DX9 only. Needs shader fix. |
| World in Conflict | DX9/DX10 | x86 | ✓ | Untested |  |

- **Excluded (Online Ban Risk):** *Age of Conan: Unchained*, *Champions Online*, *DC Universe Online*, *Entropia Universe*, *Final Fantasy 14*, *TERA Online*, *The Lord of the Rings Online*, *Warcraft III: Reign of Chaos*, *World of Tanks*.

### Nvidia 3D Vision "Automatic Mode" Games 
Games that used 3D Vision "Automatic Mode" driver stereo injection, but communicate with 3D vision with a ingame settings, built-in shader fixes, or config options. For our release, these use wiz3D's stereo injector instead, with the aim of making any shaders fixes and seperationa nd convergance sliders compatible with our injector in the future. Enable/disable settings should work currently.

| Game | API | Bits | iZ3D Profile | Testing | Notes |
|------|-----|------|--------|-------|-------|
| Alice: Madness Returns | DX9 | x86 | ✕ | ✅ Mostly Working | `AllowNvidiaStereo3d=True` in `AliceEngine.ini`. Needs windows and shadow shader fix. |
| Assassin's Creed: Revelations | DX9 | x86 | ✕ | Untested | `3D Vision Fog` in game settings |
| Batman: Arkham Asylum | DX9 | x86 | ✓ | ✅ Working | Enables `Nvidia Stereoscopic 3D` in game setting. |
| Batman: Arkham City | DX9/DX11 | x86 | ✕ | ✅ Partially Working | DX9 working, DX11 not working. Might need to implement more 3D Vison features. |
| Batman: Arkham Origins | DX9/DX11 | x86 | ✕ | ❌ Not Working | DX9 needs `-force-d3d9`. Not working in DX9 or DX10. |
| Battlefield: Bad Company 2 | DX9/DX10/DX11 | x86 | ✓ | Untested | Including 'Vietnam' Expansion Pack. |
| Brave: The Video Game | DX9 | x86 | ✕ | Untested |  |
| Call of Duty: Black Ops | DX9 | x86 | ✓ | ✅ Working | Might need to implement more 3D Vison features. |
| Carrier Command: Gaea Mission | DX9/DX11 | x86 | ✕ | Untested |  |
| Dead Rising 2 | DX9 | x86 | ✕ | ✅ Mostly Working | Needs mirrors and fade-out zombies shader fix. |
| Deep Black: Reloaded | DX9 | x86 | ✕ | Untested |  |
| Depth Hunter | DX9 | x86 | ✕ | Untested |  |
| Devil May Cry 4 | DX9/10 | x86 | ✓ | Untested | `Stereo=ON` in `config.ini`. |
| Devil May Cry 4 Special Edition | DX9 | x86 | ✓ | Untested | `Stereo=ON` in `config.ini`. |
| Dragon's Dogma: Dark Arisen | DX9 | x86 | ✕ | Untested | `Stereo=ON` in `config.ini`. |
| Duke Nukem Forever | DX9/DX10 | x86 | ✕ | Untested |  |
| Grand Theft Auto IV | DX9 | x32 | ✓ | ✅ Mostly Working | `-stereo` command line argument.<br>Needs shadows shader fix. |
| Grand Theft Auto V (Legacy) | DX11 | x64 | ✕ | Untested | `Stereo 3D` option in `Graphics` menu. |
| GT Legends | DX9 | x86 | ✕ | Untested |  |
| Inversion | DX9/DX11 | x86 | ✕ | ⚠️ Partial | Might need to implement more 3D Vison features or shader fix. |
| Just Cause 2 | DX10 | x86 | ✓ | Untested |  |
| L.A. Noire | DX9/DX11 | x86 | ✕ | Untested | Game tells Automatic Mode what depth to use per frame. |
| Lost Planet 2 | DX9/DX11 | x86 | ✓ | Untested | `Stereo=ON` in `config.ini`. |
| Mafia II | DX9 | x86 | ✓ | Untested |  |
| Max Payne 3 | DX9/DX11 | x86 | ✕ | ⚠️ Partial | Needs 3D Vision shader fixes. `3D Vision` not showing in `Graphics` menu. `-stereo 1` command line argument. |
| Medal of Honor (2010) | DX9/DX11 | x86 | ✓ | Untested | Use Single Player only. |
| Metro 2033 | DX9/DX11 | x86 | ✓ | ✅ Mostly Working | Can't see weapon in hand. |
| Metro: Last Light | DX9/DX11 | x86 | ✕ | Untested | Lists 3D Vision support in [Official PC Requirements](https://www.reddit.com/r/Games/comments/1cjh4l/metro_last_light_official_pc_requirements/). |
| [Orbiter Space Flight Simulator](https://github.com/orbitersim/orbiter) | DX9 | x64 | ✕ | Untested | `Stereoscopic 3D` in settings. |
| Resident Evil 5 | DX9/DX10 | x86 | ✓ | ✅ Working | `Stereo=ON` in `config.ini`.<br>DX9 only on Steam, only tested DX9. |
| Resident Evil 6 | DX9 | x86 | ✕ | ✅ Working | `Stereo=ON` in `config.ini`. Might need shader fix. |
| rFactor 2 | DX9/DX11 | x64 | ✕ | Untested | Use Single-player only. |
| Roller Coaster Rampage | DX9 | x86 | ✕ | Untested |  |
| Sid Meier's Civilization V | DX9/DX11 | x86 | ✓ | Untested |  |
| Super Street Fighter IV Arcade Edition | DX9 | x86 | ✓ | ✅ Working | Might need to implement more 3D Vison features. |
| Street Fighter X Tekken | DX9 | x86 | ✕ | ✅ Working | Might need to implement more 3D Vison features and shader fixes. |
| Tom Clancy's H.A.W.X 2 | DX9/DX11 | x86 | ✓ | Untested |  |
| The Witcher 2: Assassins of Kings | DX9 | x86 | ✓ | Untested |  |

- **Excluded (Native SBS/TAB):** *Deus Ex: Mankind Divided*, *DOOM 3: BFG Edition*, *Avatar: The Game*, *Sonic Generations*. 
- **Excluded (Native AMD HD3D):** *Battlefield 3*, *DiRT 2*, *DiRT 3*, *DiRT Showdown*, *DiRT Rally*, *GRID 2*, *GRID Autosport*, *Tomb Raider (2013)*. 
- **Excluded (Online Ban Risk):** *Aion: The Tower of Eternity*, *Diablo III*, *Hawken*, *Pirate101*, *Rusty Hearts*, *Wizard101*, *StarCraft II*.
- **Excluded (Demo or Benchmark):** *Aliens vs. Triangles*, *Endless City*, *Stone Giant*, *Supersonic Sled*, *Passion Leads Army Benchmark*, *Unigine: Heaven Benchmark*.

## OpenGL Quad-Buffer Stereo Games

| Game | API | Bits | Testing | Notes |
|------|-----|---------|--------|-------|
| American McGee's Alice | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rec1TwuooyYEpOvAb) to enable the game's native stereo 3D. |
| Beyond Sunset | OpenGL 3.3/4.0 | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Darkest Dungeon | OpenGL 3.2 | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rec7BJH9upc90nvsi) to enable the game's native stereo 3D. |
| Captain Wayne - Vacation Desperation | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Final Fantasy IV (3D Remake) | OpenGL | x86 | Untested |  |
| Final Fantasy IV: The After Years | OpenGL | x86 | Untested |  |
| FlightGear | OpenGL | x86 | Untested | Use instructions on [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recNbODkwtnnqlQqO) and [flightgear wiki](https://wiki.flightgear.org/Anaglyph_(3D)) to enable the game's native stereo 3D. |
| Goodbye Deponia | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recJuY1XRDu9lF9gB) to enable the game's native stereo 3D. |
| Hands of Necromancy | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Hands of Necromancy II | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Heavy Metal: F.A.K.K.² | OpenGL 1.1 | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recx8IhEW9qN2VC6t) to enable the game's native stereo 3D. |
| Hedon | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Hotline Miami | DX8/OpenGL 2.1 | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recW9i65jXiVue5TQ) to enable the game's native stereo 3D. |
| I Am Sakuya: Touhou FPS Game | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Prey (2006) | OpenGL 2.0 | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rech3exawnWvSNm31) to enable the game's native stereo 3D. |
| Quake | OpenGL 1.0 | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=reciOCmaXBxKj4N3D) to enable the game's native stereo 3D. |
| Quake II | OpenGL 1.1 | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recY4PQQVRFPDYUh7) to enable the game's native stereo 3D. |
| Quake III Arena | OpenGL 1.1 | x86 | ✅ Working | Use instructions on [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rec1zJdFTfKoty2ZO) and [ioquake3 wiki](https://web.archive.org/web/20190507130336/https://wiki.ioquake3.org/Stereo_Rendering) to enable the game's native stereo 3D. |
| Project Absentia | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| REKKR: Sunken Land | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Return to Castle Wolfenstein | OpenGL 1.1 | x86 | ✅ Working | Use instructions on [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recIgh2WvNLp0VdKe) to enable the game's native stereo 3D. |
| Selaco | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| SpaceEngine | OpenGL 4.4 | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recg8wr1aqpc4BoFH) to enable the game's native stereo 3D. |
| Star Wars: Jedi Knight - Jedi Academy | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=reclLl1NuWWFtyD73) to enable the game's native stereo 3D. |
| Star Wars: Jedi Knight II: Jedi Outcast | OpenGL 1.1 | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recg8qqxNUmWEcMHS) to enable the game's native stereo 3D. |
| SuperTuxKart | OpenGL 3.3 | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rectLIRCA4uYSpMnk) to enable the game's native stereo 3D. |
| Supplice | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| The Binding of Isaac: Rebirth | OpenGL 2.0 | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recYlJ9YTgmF7wMc5) to enable the game's native stereo 3D. |
| The Forestale | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Twilight Town: A Cyberpunk FPS | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Twilight Manor: Roguelite FPS | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| Vomitoreum | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recp8rMIlwRlwDYzX) to enable the game's native stereo 3D. |
| World of Padman  | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=rec5wq4OBzfTqGSJq) to enable the game's native stereo 3D. |
| Wrath: Aeon of Ruin | OpenGL | x86 | Untested | Check [3D/VR Compatibility Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?uMgeK=recVVJQxrjuGFbsxA) to enable the game's native stereo 3D. |

---

## Keyboard Shortcuts

In-game hotkeys for adjusting stereo on the fly. All bindings are remappable in `wiz3D_Config.xml` under `<HotKeyList>` — see the header comment in that file for the key-code format.

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
| `Ctrl+Numpad /` | Toggle laser sight (crosshair anchored at screen depth) |
| `PrintScreen` | Save a stereo screenshot to `My Pictures\wiz3D Screenshots\<Game>\` (default format `.png`; pick PNG/PNS/JPEG/JPS/BMP/DDS via `<ScreenshotType>` in `wiz3D_Config.xml`) |
| `Ctrl+PrintScreen` | Save a stereo screenshot without applying the display's gamma ramp |

---

## Building from Source

### Prerequisites

- **Visual Studio 2026** with the "Desktop development with C++" workload (v145 toolset)
- **Windows 11 SDK** (10.0.26100 or later)
- Git

### External SDKs (not included — download separately)

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
2. Copy the **contents** of the appropriate `releases/wiz3D/` subfolder (e.g. `dx9/x86/`) directly into the game's `.exe` folder.
3. Set `wiz_Config.xml` or `HD3D_Config.xml` to match your display specifications
4. For some games (L4D2, Portal, HL2), copy into the `bin/` subfolder instead.
5. Launch the game. stereo should activate automatically.
6. Toggle Stereo on and off using `*`.

---

## Contributing

Contributions are very welcome. Key areas where help is most needed:

1. **Game testing** — test with DX7-11 and OpenGL games and add results to the tables above
2. **DX10/11 completion** — the DX10/11 wrapper was never fully finished by iZ3D, they closed first year of DX11's release. Debugging and completing it is a major effort but very valuable
3. **DX7/8 stereo** — basic wrappers pass through to DX9 but stereo initialisation isn't working yet
4. **OpenGL stereo** — Wrapper needsfurther building and fleshing out
5. **Output modes** — test the various stereo output plugins (SBS, anaglyph, interlaced, shutter)

Please open an issue before starting large changes. For game test results, edit the tables in `README.md` directly and open a PR.

---

## License & Commercial Use

The original legacy iZ3D code included in this repository remains under its original **MIT License**.

All new modifications, proxy DLLs, hooks, and modernizations introduced by the **wiz3D** project are licensed under the **GNU Lesser General Public License v2.1 (LGPLv2.1)** (see `LICENSE`).

**What this means for the community:** wiz3D is free for gamers, modders, and the open-source community to use, modify, and distribute. Thanks to the LGPLv2.1, this wrapper can be legally and safely injected into proprietary, closed-source games. However, any modifications made directly to the wiz3D wrapper codebase itself must remain open-source and be shared back with the community under the same license.
