# wiz3D "We See 3D"

A universal stereoscopic 3D wrapper for DirectX 7-11, OpenGL, AMD HD3D, and Nvidia 3D Vision. Based on iZ3D. 

**iZ3D** meant "**I** See 3D", so **wiz3D** means "**We** See 3D"

<img width="5760" height="1080" alt="vlcsnap-2026-04-05-21h46m56s809_Parralel _and_Cross" src="https://github.com/user-attachments/assets/b8d2c574-2962-4bfa-b15d-380956552a32" />
<img width="5760" height="1080" alt="vlcsnap-2026-04-03-22h37m17s146_Parralel _and_Cross" src="https://github.com/user-attachments/assets/89859e2f-915f-499f-a307-b8738d0e561f" />
<img width="5760" height="1080" alt="Screenshot 2026-03-26 190057_Parralel _and_Cross" src="https://github.com/user-attachments/assets/493f2e32-1c3b-42c6-984d-ef9eacf96620" />

---

## What Is This?

wiz3D is an open-source stereoscopic 3D wrapper that hooks into DirectX, OpenGL, and AMD HD3D native games to generate real-time stereo 3D output (Half Side-by-Side, Top-and-Bottom, Anaglyph, etc.) without requiring kernel drivers or proprietary hardware.

iZ3D was a commercial product (~2002–2010) and one of the pioneers in modding games for stereoscopic 3D using kernel-level driver injection. The original developers kindly open-sourced the code under the MIT license, hosted by [bo3b/iZ3D](https://github.com/bo3b/iZ3D).

This project modernizes that source code, replaces kernel-level hooks with a proxy DLL loader, and expands the scope to re-enable stereoscopic 3D in games that have native AMD HD3D and Nvidia 3D Vision support.

## Current Status

### Released Build:

* **AMD HD3D:** ✅ **Mostly Working!** HD3D games render stereo interally, so all that's needed is proxy to enable that rendering, capture the quad buffer output, and display it using modern stereo3D standards. The Proxy chain is successfully triggering stereo3D and capturing the quad buffer output. All that remains is getting that quad buffer output to display corrently in modern formats like Top-and-Bottom and Side-by-Side. Currently Half TAB and Half SBS is supported with about half the games, the other games still need work displaying the output correctly.
* **DirectX 9:** ✅ **Mostly Working!** `d3d9.dll` proxy loader works! Left 4 Dead 2 and many others run in full stereo3D, outputs in all originally supported formats, and the profile system loads shader fixes and stereo settings for all originally supported games.
* **DirectX 8:** ⚠️ **In Progress.** Wrapper to convert DX8 to use DX9's stereoization. DX8 to DX9 converstion is working. Here for testing.
* **DirectX 7:** ⚠️ **In Progress.** Wrapper to convert DX7 to use DX9's stereoization. DX7 to DX9 converstion is working. Here for testing.

### Unreleased Builds:

* **DirectX 10/11:** ⚠️ **Partial.** The DX10/11 wrapper was never completely finished by iZ3D Inc. Some games work, but implementation in wiz3D still has some way to go and hasn't got any games booting with stereo3D initialised yet.
* **OpenGL:** ⚠️ **In Progress.** Basic build has been made, untested. 
* **Nvidia 3D Vision 'Ready':** ⚠️ **In Progress.** Still in very early stages, kind of a long shot. Most 3D Vision Ready games don't have internal stereo rendering, they just make their shaders and UI more compatible with Nvidia's stereo injector. We will aim to get 3DV shaders and UI working with iZ3D as the injector instead.

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

*Legend: ✅ **Working** = Stereo output activated and playable. ⚠️ **Partial** = Stereo activated but with issues (crash, wrong settings, shader problems). **Not loading** = Wrapper not activating. **Untested** = I haven't tested yet or don't have access to that game*

### AMD HD3D Native Games

| Game | API | Bits | Testing | Notes |
|------|-----|------|--------|-------|
| Battlefield 3 | DX11 | x86 | ✅ Working | Campaign only. Don't use with Multiplayer due to ban risks. |
| Deus Ex: Human Revolution | DX11 | x86 | ✅ Working | Cursor doubled correctly. Check iZ3D Shader Fix. |
| Deus Ex: Human Revolution Director's Cut | DX11 | x86 | ✅ Working | Undocumented AMD HD3D support. Cursor doubled correctly. |
| DiRT 2 | DX11 | x86 | Untested | `hardware_settings_config.xml` needs `stereo enabled="true"` |
| DiRT 3 | DX11 | x86 | Untested | `hardware_settings_config.xml` needs `stereo enabled="true"`. Check iZ3D Shader Fix. |
| DiRT 3 Complete Edition | DX11 | x86 | ⚠️ Partial | Only top half of Half SBS visible. `hardware_settings_config.xml` needs `stereo enabled="true"` |
| DiRT Showdown | DX11 | x86 | ⚠️ Partial | Only top half of Half SBS visible. `hardware_settings_config.xml` needs `stereo enabled="true"` |
| DiRT Rally | DX11 | x86 | ⚠️ Partial | Only top half of Half SBS visible. `hardware_settings_config.xml` needs `stereo enabled="true"` |
| F1 2010 | DX11 | x86 | Untested | Untested |
| F1 2011 | DX11 | x86 | Untested | Untested |
| F1 2012 | DX11 | x86 | Untested | Untested |
| F1 2013 | DX11 | x86 | Untested | Untested |
| GRID 2 | DX11 | x86 | ⚠️ Partial | Only top half of Half SBS visible. `hardware_settings_config.xml` needs `stereo enabled="true"` |
| GRID Autosport | DX11 | x86 | ⚠️ Partial | Only top half of Half SBS visible. `hardware_settings_config.xml` needs `stereo enabled="true"` |
| Hitman: Absolution | DX11 | x86 | ⚠️ Partial | UI,  menus and videos correct, gameplay is the top half of the view-port. Mouse not doubling. |
| Sleeping Dogs | DX11 | x86 | ✅ Working | Mouse not doubling. |
| Sleeping Dogs: Definitive Edition | DX11 | x64 | ✅ Working | Undocumented AMD HD3D support. |
| Sniper Elite V2 | DX11 | x86 | ✅ Working | Minor color shifting, and mouse not doubling. |
| Sniper Elite III | DX11| x86| ✅ Working | `customersupportlogging` beta branch only. Main branch skips HD3D driver loading. |
| Sniper Elite 4 | DX11/12| x64| ✅ Working | DX11 only. DX12 might be possible in future. |
| Sniper Elite: Nazi Zombie Army | DX11 | x86 | ✅ Working | Minor color shifting, and mouse not doubling. |
| Sniper Elite: Nazi Zombie Army 2 | DX11 | x86 | ✅ Working | Minor color shifting, and mouse not doubling. |
| Zombie Army Trilogy | DX11 | x86 | ✅ Working | Colors appear correct. |
| Thief (2014) | DX11 | x86/x64| ✅ Working | Both x86 and x64 versions working. |
| Tomb Raider (2013) | DX11 | x86 | ⚠️ Partial | Only top half of Half SBS. Most stubborn HD3D game to get working. |

- **Excluded (Native SBS/TAB):** *Crysis 2*, *Crysis 3*, *Rise of the Tomb Raider*, *Shadwen*, *Two Worlds II*, *Trine 1*, *Trine 2*, *Trine 3*.
- **Excluded (Online Ban Risk):** *World of Warcraft*. <sub>(Stereo3D removed in 2018 DX12 update)</sub>

### DirectX 7/8 Games

| Game | API | Bits | Profile | Testing | Notes |
|------|-----|---------|--------|-------|-------|
| Ballance | DX8 | x86 | ✓ | Untested | - |
| Deus Ex | OpenGL/DX7 | x86 | ✕ | ❌ No Stereo | listed for DX7 testing. |
| FATE | DX8 | x86 | ✕ | Untested | - |
| Mega Man X8 | DX8 | x86 | ✓ | Untested | - |
| Mercedes-Benz World Racing | DX8 | x86 | ✓ | Untested | - |
| Sniper Elite | DX8 | x86 | ✓ | Untested | - |
| The Lord of the Rings: The Return of the King | DX8 | x86 | ✓ | Untested | - |
| Thief: Deadly Shadows | DX8 | x86 | ✓ | Untested | - |
| Tony Hawk's Pro Skater 3 | DX8 | x86 | ✓ | Untested | - |

- **Excluded (Online Ban Risk):** *Command & Conquer: Generals*, *Command & Conquer: Renegade*, *Empire Earth II*, *Freelancer*, *GTR - FIA GT Racing Game*, *NASCAR Racing 2003 Season*, *Tom Clancy's Rainbow Six 3: Raven Shield*. <sub>(Active community servers with stringent anti-cheat)</sub>

## DirectX 9 32bit Games

| Game | API | Bits | Profile | Testing | Notes |
|------|-----|---------|--------|-------|-------|
| A.R.E.S.: Extinction Agenda | DX9 | x86 | ✓ | Untested | - |
| AaAaAA!!! A Reckless Disregard for Gravity | DX9 | x86 | ✓ | Untested | - |
| Aion: The Tower of Eternity | DX9 | x86 | ✓ | Untested | Includes Shader Fix, see if compatible with 3D Vision in future. |
| Alien Breed 2: Assault | DX9 | x86 | ✓ | Untested | - |
| Alien Swarm | DX9 | x86 | ✓ | - | - |
| Alone in the Dark (2008) | DX9 | x86 | ✓ | Untested | - |
| America's Army | DX8/DX9 | x86 | ✓ | Untested | - |
| Anomaly Warzone Earth | DX9 | x86 | ✓ | Untested | - |
| AquaNox 2: Revelation | DX8/DX9 | x86 | ✓ | Untested | - |
| Arma: Armed Assault | DX9 | x86 | ✓ | Untested | aka ARMA: Combat Operations  |
| Armies of Exigo | DX9 | x86 | ✓ | Untested | - |
| Assassin's Creed II | DX9 | x86 | ✓ | Untested | - |
| Assassin's Creed: Brotherhood | DX9 | x86 | ✓ | Untested | - |
| Audiosurf | DX9 | x86 | ✓ | Untested | - |
| Back to the Future: The Game | DX9 | x86 | ✓ | Untested | Episode 1-5 |
| Batman: Arkham Asylum | DX9 | x86 | ✓ | Untested | Includes Shader Fix, see if compatible with 3D Vision in future. |
| Battlefield 2 | DX9 | x86 | ✓ | Untested | - |
| Battlefield 2142 | DX9 | x86 | ✓ | Untested | - |
| Battlestations: Pacific | DX9 | x86 | ✓ | Untested | - |
| Beowulf: The Game | DX9 | x86 | ✓ | Untested | - |
| Bet on Soldier | DX9 | x86 | ✓ | Untested | - |
| Beyond Good & Evil | DX9 | x86 | ✓ | Untested | - |
| Bionic Commando (2009) | DX9 | x86 | ✕ | Untested | - |
| Bionic Commando Rearmed | DX9 | x86 | ✓ | Untested | - |
| Black & White 2 | DX9 | x86 | ✓ | Untested | - |
| BlazBlue: Calamity Trigger | DX9 | x86 | ✕ | Untested | - |
| Borderlands | DX9 | x86 | ✓ | Untested | - |
| Brothers in Arms: Road to Hill 30 | DX9 | x86 | ✓ | Untested | - |
| Brothers in Arms: Earned in Blood | DX9 | x86 | ✓ | Untested | - |
| Brothers in Arms: Hell's Highway | DX9 | x86 | ✓ | Untested | - |
| Bulletstorm | DX9 | x86 | ✓ | Untested | - |
| Burnout Paradise | DX9 | x86 | ✓ | Untested | - |
| Cabela's Big Game Hunter: 10th Anniversary Edition | DX9 | x86 | ✓ | Untested | Not sure which game, profile says "Cabela Big Game Hunter BGH10.exe" |
| Call of Cthulhu: Dark Corners of the Earth | DX9 | x86 | ✓ | Untested | - |
| Call of Duty 2 | DX9 | x86 | ✓ | Untested | - |
| Call of Duty: World at War | DX9 | x86 | ✓ | Untested | - |
| Call of Duty: Black Ops | DX9 | x86 | ✓ | Untested | Includes Shader Fix, see if compatible with 3D Vision in future. |
| Call of Duty 4: Modern Warfare (2007) | DX9 | x86 | ✓ | Untested | - |
| Call of Duty: Modern Warfare 2 | DX9 | x86 | ✓ | Untested | - |
| Cars | DX9 | x86 | ✓ | Untested | - |
| Chromadrome 2 | DX9 | x86 | ✓ | Untested | - |
| Command & Conquer 3: Tiberium Wars | DX9 | x86 | ✓ | Untested | - |
| Command & Conquer: Red Alert 3 | DX9 | x86 | ✓ | Untested | - |
| Command & Conquer 4: Tiberian Twilight | DX9 | x86 | ✓ | Untested | - |
| Condemned: Criminal Origins | DX9 | x86 | ✓ | Untested | - |
| Demigod | DX9 | x86 | ✕ | Untested | - |
| Damnation | DX9 | x86 | ✓ | Untested | - |
| Dark Messiah of Might and Magic | DX9 | x86 | ✓ | Untested | - |
| Dark Void | DX9 | x86 | ✓ | Untested | - |
| Dead Space | DX9 | x86 | ✓ | Untested | - |
| Dead Space 2 | DX9 | x86 | ✓ | Untested | - |
| Defense Grid: The Awakening | DX9 | x86 | ✓ | Untested | - |
| Delta Force: Xtreme | DX9 | x86 | ✓ | Untested | - |
| Devil May Cry 3: Special Edition | DX9 | x86 | ✓ | Untested | - |
| DiRT | DX9 | x86 | ✓ | Untested | - |
| Disciples III: Renaissance | DX9 | x86 | ✓ | Untested | - |
| Divinity II: Ego Draconis  | DX9 | x86 | ✓ | Untested | - |
| Dragon Age: Origins | DX9 | x86 | ✓ | Untested | - |
| Drakensang: The Dark Eye | DX7/DX9 | x86 | ✓ | Untested | - |
| Dungeons (2011) | DX9 | x86 | ✕ | Untested | - |
| Dungeon Siege II | DX9 | x86 | ✓ | Untested | - |
| Dungeon Siege III | DX9 | x86 | ✓ | Untested | - |
| Empire: Total War | DX9 | x86 | ✓ | Untested | - |
| Enemy Engaged 2 | DX9 | x86 | ✕ | Untested | - |
| Eragon | DX9 | x86 | ✓ | Untested | - |
| Evolution GT | DX9 | x86 | ✕ | Untested | - |
| Fable: The Lost Chapters | DX9 | x86 | ✓ | Untested | - |
| Fable III | DX9 | x86 | ✓ | Untested | - |
| Fahrenheit | DX9 | x86 | ✓ | Untested | aka Indigo Prophecy |
| Fallout 3 | DX9 | x86 | ✓ | Untested | - |
| F.E.A.R. | DX9 | x86 | ✓ | Untested | - |
| F.E.A.R. Perseus Mandate | DX9 | x86 | ✓ | Untested | - |
| F.E.A.R. 2: Project Origin | DX9 | x86 | ✓ | Untested | - |
| FIFA 10 | DX9 | x86 | ✕ | Untested | - |
| FlatOut | DX9 | x86 | ✓ | Untested | - |
| FlatOut 2 | DX9 | x86 | ✕ | Untested | - |
| FlatOut: Ultimate Carnage | DX9 | x86 | ✓ | Untested | - |
| Foreign Legion: Buckets of Blood | DX9 | x86 | ✕ | Untested | - |
| Frontlines: Fuel of War | DX9 | x86 | ✓ | Untested | - |
| Front Mission Evolved | DX9 | x86 | ✕ | Untested | - |
| Fuel | DX9 | x86 | ✕ | Untested | - |
| G-Force | DX9 | x86 | ✓ | Untested | - |
| Garshasp: The Monster Slayer | DX9 | x86 | ✓ | Untested | - |
| Ghostbusters: The Video Game | DX9 | x86 | ✓ | Untested | - |
| Ghostbusters: Sanctum of Slime | DX9 | x86 | ✓ | Untested | - |
| Google Earth | OpenGL/DX9 | x86 | ✓ | Untested | [Google Earth Pro 7.1.5.1557](https://web.archive.org/web/20171014110844/https://dl.google.com/earth/client/GE7/release_7_1_8/googleearth-win-pro-7.1.8.3036.exe) |
| Grand Theft Auto: San Andreas | DX9 | x86 | ✓ | Untested | - |
| Grand Theft Auto IV | DX9 | x86 | ✓ | Untested | - |
| GRID | DX9 | x86 | ✓ | Untested | - |
| Guitar Hero III: Legends of Rock | DX9 | x86 | ✓ | Untested | - |
| Half-Life 2 | DX9 | x86 | ✓ | ⚠️ Not loading | Suspect issue with needing `-game` command line argument  |
| Heroes of Might and Magic V | DX9 | x86 | ✓ | Untested | - |
| Hitman: Blood Money | DX9 | x86 | ✓ | Untested | - |
| Hunted: The Demon's Forge | DX9 | x86 | ✓ | Untested | - |
| Kane & Lynch 2: Dog Days | DX9 | x86 | ✓ | Untested | - |
| Killing Floor | DX8/DX9 | x86 | ✓ | Untested | - |
| King Arthur: The Role-Playing Wargame | DX9 | x86 | ✓ | Untested | Specifies 'King Arthur: The Druids' expansion pack |
| King's Bounty: The Legend | DX9 | x86 | ✓ | Untested | - |
| Left 4 Dead | DX9 | x86 | ✓ | ✅ Working | Only tested single player. Use `-insecure` command line argument to avoid VAC ban. |
| Left 4 Dead 2 | DX9 | x86 | ✓ | ✅ Working | Only tested single player. Use `-insecure` command line argument to avoid VAC ban. |
| Lego Star Wars: The Video Game | DX9 | x86 | ✓ | Untested | - |
| Lego Star Wars III: The Clone Wars | DX9 | x86 | ✓ | Untested | - |
| Madden NFL 08 | DX9 | x86 | ✓ | Untested | - |
| Mafia II | DX9 | x86 | ✓ | Untested | Includes Shader Fix, see if compatible with 3D Vision in future. |
| Majesty 2: The Fantasy Kingdom Sim | DX9 | x86 | ✓ | Untested | - |
| Mass Effect | DX9 | x86 | ✓ | Untested | - |
| Mass Effect 2 | DX9 | x86 | ✓ | Untested | - |
| Medal of Honor: Airborne | DX9 | x86 | ✓ | Untested | - |
| Mercenaries 2: World in Flames | DX9 | x86 | ✓ | Untested | - |
| Microsoft Flight Simulator 2004: A Century of Flight | DX9 | x86 | ✓ | Untested | - |
| Mini Ninjas | DX9 | x86 | ✓ | Untested | - |
| Mirror's Edge | DX9 | x86 | ✓ | Untested | - |
| Monday Night Combat | DX9 | x86 | ✓ | Untested | - |
| MTX: Mototrax | DX9 | x86 | ✓ | Untested | - |
| Mythos | DX9 | x86 | ✓ | Untested | Mythos (2009) maybe? Not sure which game this is. |
| Need for Speed: ProStreet | DX9 | x86 | ✓ | Untested | - |
| Need for Speed: Undercover | DX9 | x86 | ✓ | Untested | - |
| Need for Speed: Hot Pursuit (2010) | DX9 | x86 | ✓ | Untested | - |
| Need for Speed: Shift | DX9 | x86 | ✓ | Untested | - |
| Need for Speed: Shift 2 Unleashed | DX9 | x86 | ✓ | Untested | - |
| Ninja Blade | DX9 | x86 | ✓ | Untested | - |
| Operation Flashpoint: Dragon Rising | DX9 | x86 | ✓ | Untested | - |
| OutRun 2006: Coast 2 Coast | DX9 | x86 | ✓ | Untested | - |
| Overlord | DX9 | x86 | ✓ | Untested | - |
| Overlord II | DX9 | x86 | ✓ | Untested | - |
| Painkiller | DX9 | x86 | ✓ | Untested | - |
| Painkiller: Overdose | DX9 | x86 | ✓ | Untested | - |
| Portal | DX9 | x86 | ✓ | ⚠️ Not loading | Suspect issue with needing `-game portal` command line argument  |
| Portal 2 | DX9 | x86 | ✓ | Untested | - |
| Prince of Persia: The Forgotten Sands | DX9 | x86 | ✓ | Untested | - |
| ProtoGalaxy | DX9 | x86 | ✓ | Untested | - |
| Resident Evil 5 | DX9 | x86 | ✓ | Untested | Includes Shader Fix, see if compatible with 3D Vision in future. |
| Richard Burns Rally | DX9 | x86 | ✓ | Untested | - |
| Rise and Fall: Civilizations at War | DX9 | x86 | ✓ | Untested | - |
| Rise of Flight | DX9 | x86 | ✓ | Untested | - |
| Rise of the Argonauts | DX9 | x86 | ✓ | Untested | - |
| Risen | DX9 | x86/x64 | ✓ | Untested | - |
| S.T.A.L.K.E.R.: Shadow of Chernobyl | DX9 | x86 | ✓ | Untested | - |
| Saints Row 2 | DX9 | x86 | ✓ | Untested | - |
| Sam & Max Save the World | DX8/DX9 | x86 | ✓ | Untested | Notes Episode 4: Abe Lincoln Must Die! |
| Sam & Max: The Devil's Playhouse | DX9 | x86 | ✓ | Untested | Notes Episode 1: The Penal Zone |
| Samurai Warriors 2 | DX9 | x86 | ✓ | Untested | - |
| Sanctum | DX9 | x86 | ✓ | Untested | - |
| Section 8 | DX9 | x86 | ✓ | Untested | - |
| Serious Sam 2 | DX9 | x86 | ✓ | Untested | - |
| Sexy Beach 3 | DX9 | x86 | ✓ | Untested | - |
| Shadowgrounds | DX9 | x86 | ✓ | Untested | - |
| Sid Meier's Civilization IV | DX9 | x86 | ✓ | Untested | - |
| Sid Meier's Railroads! | DX9 | x86 | ✓ | Untested | - |
| Silent Hunter 3 | DX9 | x86 | ✓ | Untested | - |
| Silent Hunter 4: Wolves of the Pacific | DX9 | x86 | ✓ | Untested | - |
| Silent Hunter 5: Battle of the Atlantic | DX9 | x86 | ✓ | Untested | - |
| Singularity | DX9 | x86 | ✓ | Untested | - |
| Sins of a Solar Empire | DX9 | x86 | ✓ | Untested | - |
| Spore | DX9 | x86 | ✓ | Untested | - |
| Star Trek: Legacy | DX9 | x86 | ✓ | Untested | - |
| Star Wars: Battlefront (2004) | DX9 | x86 | ✓ | Untested | - |
| Star Wars: Battlefront II (2005) | DX9 | x86 | ✓ | Untested | - |
| Starship Troopers | DX9 | x86 | ✓ | Untested | - |
| Street Fighter IV | DX9 | x86 | ✓ | Untested | - |
| Super Street Fighter IV Arcade Edition | DX9 | x86 | ✓ | Untested | Includes Shader Fix, see if compatible with 3D Vision in future. |
| Supreme Commander | DX9 | x86 | ✓ | Untested | - |
| Supreme Commander: Forged Alliance | DX9 | x86 | ✓ | Untested | - |
| Supreme Commander 2 | DX9 | x86 | ✓ | Untested | - |
| Test Drive Unlimited | DX9 | x86 | ✓ | Untested | - |
| The Ball | DX9 | x86 | ✕ | ✅ Working | Ball shadow diffrent in both eyes. |
| The Chronicles of Narnia: The Lion, the Witch and the Wardrobe | DX9 | x86 | ✓ | Untested | (Guess based on 'Narnia' and 'Narnia.exe') |
| The Elder Scrolls IV: Oblivion | DX9 | x86 | ✓ | Untested | - |
| The Movies (2005) | DX9 | x86 | ✓ | Untested | - |
| The Settlers II: 10th Anniversary | DX9 | x86 | ✓ | Untested | - |
| The Sims 2: University | DX9 | x86 | ✓ | Untested | Targets "Sims2EP1.exe", might just be expansion pack or all of The Sims 2. |
| The Sims 3 | DX9 | x86 | ✓ | Untested | - |
| The Sims Medieval | DX9 | x86 | ✓ | Untested | - |
| The Witcher | DX9 | x86 | ✓ | Untested | - |
| The Witcher 2: Assassins of Kings | DX9 | x86 | ✓ | Untested | Includes Shader Fix, see if compatible with 3D Vision in future. |
| TimeShift | DX9 | x86 | ✓ | Untested | - |
| Titan Quest | DX9 | x86 | ✓ | Untested | - |
| TOCA Race Driver 3 | DX9 | x86 | ✓ | Untested | - |
| Tom Clancy's Ghost Recon Advanced Warfighter 2 | DX9 | x86 | ✓ | Untested | - |
| Tom Clancy's Rainbow Six: Vegas | DX9 | x86 | ✓ | Untested | - |
| Tom Clancy's Rainbow Six: Vegas 2 | DX9 | x86 | ✓ | Untested | - |
| Tom Clancy's Splinter Cell: Double Agent | DX9 | x86 | ✓ | Untested | - |
| Tom Clancy's Splinter Cell: Conviction | DX9 | x86 | ✓ | Untested | - |
| Tomb Raider: Legend | DX9 | x86 | ✓ | Untested | - |
| Tomb Raider: Anniversary | DX9 | x86 | ✓ | Untested | - |
| Tomb Raider: Underworld | DX9 | x86 | ✓ | Untested | - |
| Torchlight | DX9 | x86 | ✓ | Untested | - |
| TrackMania Nations Forever | DX9 | x86 | ✓ | Untested | - |
| Trine (2009) | DX9 | x86 | ✓ | Untested | - |
| Two Worlds | DX9 | x86 | ✓ | Untested | - |
| Unreal Tournament 3 | DX9 | x86 | ✓ | Untested | - |
| Virtua Tennis 2009 | DX9 | x86 | ✓ | Untested | - |
| Wallace & Gromit's Grand Adventures | DX9 | x86 | ✓ | Untested | Episodes 1-4 |
| Wanted: Weapons of Fate | DX9 | x86 | ✓ | Untested | - |
| Warhammer 40,000: Dawn of War | DX9 | x86 | ✓ | Untested | - |
| Warhammer 40,000: Dawn of War: Dark Crusade | DX9 | x86 | ✓ | Untested | - |
| Warhammer 40,000: Dawn of War: Soulstorm | DX9 | x86 | ✓ | Untested | - |
| Warhammer 40,000: Dawn of War II | DX9 | x86 | ✓ | Untested | - |
| Watchmen: The End is Nigh | DX9 | x86 | ✓ | Untested | - |
| Wings of Prey | DX9 | x86 | ✓ | Untested | - |
| Wolfenstein (2009) | DX9 | x86 | ✓ | Untested | - |
| WorldShift | DX9 | x86 | ✓ | Untested | - |
| Zeno Clash | DX9 | x86 | ✓ | Untested | - |

- **Excluded (Game Not Playable):** *Darkspore*. <sub>(Game not playable til [Resurrection Capsule](https://github.com/vitor251093/resurrection-capsule) project completes.)</sub>
- **Excluded (Online Ban Risk):** *Allods Online*, *Dark Age of Camelot*, *Darkfall Online*, *Fury (2007)*, *Global Agenda*, *Guild Wars*, *Monster Hunter Frontier Online*, *Warhammer Online: Age of Reckoning*.

## DirectX 9 64bit Games

| Game | API | Bits | Profile | Testing | Notes |
|------|-----|---------|--------|-------|-------|
| Chess Titans | DX9 | x86/x64 | ✕ | Untested | - |
| Far Cry | DX9 | x86/x64 | ✓ | Untested | - |
| REFLEX XTR² | DX9 | x86/x64 | ✓ | Untested | - |
| Unreal Tournament 2004 | DX9 | x64 | ✓ | ✅ Working | - |

- **Excluded (VAC Ban Risk):** *Counter-Strike: Source*, *Day of Defeat: Source*, *Half-Life 2: Deathmatch*.
- **Excluded (Online Ban Risk):** *Dungeons & Dragons Online*, *EVE Online*, *EverQuest*, *EverQuest 2*, *Flyff (Fly For Fun)*, *RIFT*, *Starcraft II*.

## DirectX 10/11 Games (Not Released Yet)

| Game | API | Bits | Profile | Testing | Notes |
|------|-----|---------|--------|-------|-------|
| Aliens vs. Predator (2010) | DX9/DX11 | x86 | ✓ | - | - |
| Assassins Creed | DX9/DX10 | x86 | ✓ | - | - |
| Battlefield: Bad Company 2 | DX9/DX10/DX11 | x86 | ✓ | - | Includes Shader Fix, see if compatible with 3D Vision in future. |
| BioShock | DX9/DX10 | x86 | ✓ | - | - |
| BioShock 2 | DX9/DX10 | x86 | ✓ | - | - |
| Call of Juarez: Bound in Blood | DX9/DX10 | x86 | ✓ | - | - |
| Company of Heroes | DX9/DX10 | x86 | ✓ | - | - |
| Cryostasis | DX9/DX10 | x86 | ✓ | - | - |
| Crysis | DX9/DX10 | x86/x64 | ✓ | - | - |
| Crysis: Warhead | DX9/DX10 | x86/x64 | ✓ | - | - |
| Crysis 2 | DX9/DX11 | x86/x64 | ✓ | - | Includes Shader Fix. See if can be applied to the game's native SBS. |
| DCS: Black Shark | DX9/DX11 | x86 | ✓ | - | Single Player may be okay. Multiplayer not recommended. |
| De Blob | DX11 | x86 | ✓ | - | - |
| Devil May Cry 4 (2008) | DX9/DX10 | x86/x64 | ✓ | - | - |
| Dragon Age II | DX9/DX11 | x86 | ✓ | - | - |
| Far Cry 2 | DX9/DX10 | x86 | ✓ | - | - |
| F.E.A.R. 3 | DX9/DX10 | x86 | ✓ | - | - |
| Gears of War | DX9/DX10 | x86 | ✓ | - | - |
| Homefront | DX9/DX11 | x86 | ✓ | - | - |
| Just Cause 2 | DX10 | x86 | ✓ | - | - |
| Lost Planet | DX9/DX10 | x86 | ✓ | - | - |
| Lost Planet 2 | DX9/DX11 | x86 | ✓ | - | - |
| Medal of Honor (2010) | DX9/DX11 | x86 | ✓ | - | Use Single Player only. Includes Shader Fix, see if compatible with 3D Vision in future. |
| Metro 2033 | DX9/DX11 | x86 | ✓ | - | - |
| Microsoft Flight Simulator X | DX9/DX10 | x86 | ✓ | - | - |
| NecroVisioN | DX9/DX10 | x86 | ✓ | - | - |
| Red Faction: Guerrilla | DX9/DX10/DX11 | x86 | ✓ | - | - |
| S.T.A.L.K.E.R.: Clear Sky | DX9/DX10 | x86 | ✓ | - | - |
| S.T.A.L.K.E.R.: Call of Pripyat | DX9/DX10/DX11 | x86/x64 | ✓ | - | - |
| Serious Sam HD: The First Encounter | DX9/DX11/DX12 | x86 | ✓ | - | - |
| Sid Meier's Civilization V | DX9/DX11 | x86 | ✓ | - | - |
| Tom Clancy's H.A.W.X | DX9/DX10 | x86 | ✓ | - | - |
| Tom Clancy's H.A.W.X. 2 | DX9/DX11 | x86 | ✓ | - | - |
| World in Conflict | DX9/DX10 | x86 | ✓ | - | - |

- **Excluded (Online Ban Risk):** *Age of Conan: Unchained*, *Champions Online*, *DC Universe Online*, *Entropia Universe*, *Final Fantasy 14*, *TERA Online*, *The Lord of the Rings Online*, *Warcraft III: Reign of Chaos*, *World of Tanks*.

### Nvidia 3D Vision "Ready" Native Games (Not Released Yet)

| Game | API | Bits | Testing | Notes |
|------|-----|------|--------|-------|
| Assassin's Creed: Revelations | DX9 | x86 | - | 3D Vision Fog option in settings |
| Batman: Arkham Asylum | DX9 | x86 | - | - |
| Batman: Arkham City | DX9/DX11 | x86 | - | - |
| Batman: Arkham Origins | DX9/DX11 | x86 | - | - |
| Battlefield: Bad Company 2 | DX9/DX10/DX11 | x86 | - | Including 'Vietnam' Expansion Pack |
| Brave: The Video Game | DX9 | x86 | - | - |
| Call of Duty: Black Ops | DX9 | x86 | - | - |
| Carrier Command: Gaea Mission | DX9/DX11 | x86 | - | - |
| Civilization V | DX9/DX11 | x86 | - | - |
| Dead Rising 2 | DX9 | x86 | - | - |
| Deep Black: Reloaded | DX9 | x86 | - | - |
| Depth Hunter | DX9 | x86 | - | - |
| Devil May Cry 4 | DX9/10 | x86 | - | `Stereo=ON` in `config.ini` needs investigation. |
| Devil May Cry 4 Special Edition | DX9 | x86 | - | `Stereo=ON` in `config.ini` needs investigation. |
| Dragon's Dogma: Dark Arisen | DX9 | x86 | - | `Stereo=ON` in `config.ini` needs investigation. |
| Duke Nukem Forever | DX9/DX10 | x86 | - | - |
| Google Earth | OpenGL/DX9 | x86 | - | [Google Earth Pro 7.1.5.1557](https://web.archive.org/web/20171014110844/https://dl.google.com/earth/client/GE7/release_7_1_8/googleearth-win-pro-7.1.8.3036.exe) |
| GT Legends | DX9 | x86 | - | - |
| Hard Reset | DX9 | x86 | - | - |
| Inversion | DX9/DX11 | x86 | - | - |
| Just Cause 2 | DX10 | x86 | - | - |
| L.A. Noire | DX9/DX11 | x86 | - | - |
| Lost Planet 2 | DX9/DX11 | x86 | - | `Stereo=ON` in `config.ini` needs investigation. |
| Mafia II | DX9 | x86 | - | - |
| Max Payne 3 | DX9/DX11 | x86 | - | - |
| Medal of Honor (2010) | DX9/DX11 | x86 | - | This might be only the multiplayer, if so I'll exclude it. |
| Metro 2033 | DX9/DX11 | x86 | - | - |
| Metro: Last Light | DX9/DX11 | x86 | - | Lists 3D vision support in [Official PC Requirements](https://www.reddit.com/r/Games/comments/1cjh4l/metro_last_light_official_pc_requirements/) |
| Oil Rush | OpenGL/DX9/DX11 | x86 | - | - |
| Resident Evil 5 | DX9/DX10 | x86 | - | - |
| Resident Evil 6 | DX9 | x86 | - | `Stereo=ON` in `config.ini` needs investigation. |
| rFactor 2 | DX9/DX11 | x64 | - | Single-player only recommended. |
| Roller Coaster Rampage | DX9 | x86 | - | - |
| Super Street Fighter IV Arcade Edition | DX9 | x86 | - | - |
| Street Fighter X Tekken | DX9 | x86 | - | - |
| Tom Clancy's H.A.W.X 2 | DX9/DX11 | x86 | - | - |
| The Witcher 2: Assassins of Kings | DX9 | x86 | - | - |

- **Excluded (Native SBS/TAB):** *Deus Ex: Mankind Divided*, *DOOM 3: BFG Edition*, *Avatar: The Game*, *Sonic Generations*. 
- **Excluded (Native AMD HD3D):** *Battlefield 3*, *DiRT 2*, *DiRT 3*, *DiRT Showdown*, *DiRT Rally*, *GRID 2*, *GRID Autosport*, *Tomb Raider (2013)*. 
- **Excluded (Online Ban Risk):** *Aion: The Tower of Eternity*, *Diablo III*, *Hawken*, *Pirate101*, *Rusty Hearts*, *Wizard101*, *StarCraft II*.
- **Excluded (Demo or Benchmark):** *Aliens vs. Triangles*, *Endless City*, *Stone Giant*, *Supersonic Sled*, *Passion Leads Army Benchmark*, *Unigine: Heaven Benchmark*.

---

## License & Commercial Use

The original legacy iZ3D code included in this repository remains under its original **MIT License**.

All new modifications, proxy DLLs, hooks, and modernizations introduced by the **wiz3D** project are licensed under the **GNU Lesser General Public License v2.1 (LGPLv2.1)** (see `LICENSE`).

**What this means for the community:** wiz3D is free for gamers, modders, and the open-source community to use, modify, and distribute. Thanks to the LGPLv2.1, this wrapper can be legally and safely injected into proprietary, closed-source games. However, any modifications made directly to the wiz3D wrapper codebase itself must remain open-source and be shared back with the community under the same license.

**Commercial Licensing (Hardware Vendors & OEMs):** If you are a hardware manufacturer, display vendor, or software company wishing to integrate the wiz3D stereoscopic wrapper into a proprietary product, modify it without releasing your source code, or bypass the copyleft restrictions of the LGPLv2.1, **Commercial B2B Licenses are available.** Please contact the repository owner to discuss a commercial exemption license, custom hardware integration, or Service Level Agreements (SLAs).
