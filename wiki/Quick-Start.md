# Quick-Start

## 1. Download

Grab the latest release zip from the [GitHub Releases page](https://github.com/Stereopticon-Org/wiz3D/releases). Extract it anywhere — the top-level `wiz3D/` folder contains one subfolder per path.

## 2. Figure out which folder to copy

If you don't already know the game's graphics API:

- The game ships a `d3d9.dll` next to the exe (unusual, but a tell). → **DX9**.
- Steam/launcher lets you pick "DX11" or "Vulkan" at launch. → use whichever you plan to run.
- Game is pre-2008. → probably **DX7**, **DX8**, or **DX9**.
- Game is 2015+ and a big-budget title. → probably **DX11** or **Vulkan**.
- 32-bit vs 64-bit exe — right-click the game exe → Properties → Compatibility; or just try one and if the game crashes immediately switch to the other.

See [Compatibility](Compatibility) for known titles.

## 3. Copy the DLLs next to the game exe

Copy **every file** from the matching folder into the folder that contains the game's `.exe`:

```
wiz3D/Stereo-Injector/<API>/<arch>/*  →  <game folder>/
```

For DX10/11 and DX9, also copy the matching **OutputMethods/** subfolder.

## 4. Admin install (important)

If the game is installed under `C:\Program Files` or `C:\Program Files (x86)` (or any Steam library on the C: drive by default), Windows blocks writes to the game folder from non-elevated processes. wiz3D writes `UserProfile.xml` and logs there at runtime, so without elevation:

- Numpad 4 (save profile) appears to work but silently fails.
- Ctrl+F5 (reload profile) then resets everything to defaults — because the save never made it to disk.

**Workarounds** (pick one):

1. Right-click the game's launcher shortcut → **Run as administrator**. Easiest.
2. Move the Steam library to a non-Program-Files location (e.g. `D:\Games\SteamLibrary`).
3. Grant your user write permission on the game folder: right-click folder → Properties → Security → Edit → add your user, tick "Modify".

A future wiz3D release (0.4.4) will fall back to `%AppData%\wiz3D\<ExeBaseName>\UserProfile.xml` when the game folder isn't writable, with a loud on-screen toast explaining what happened.

## 5. Launch the game

Stereo turns on by default. You'll see the wiz3D OSD briefly in the top-left.

**First-run checklist**:

- See the wiz3D banner at startup → ✅ wrapper loaded.
- Press **`** (backtick) → toggles OSD on/off. If OSD comes up, hotkeys are live.
- Press **Numpad +** / **Numpad -** → separation increases/decreases.
- Press **Numpad 4** → saves current settings to `UserProfile.xml` next to the game exe.
- See the "Saved to …" toast → ✅ write succeeded.
- No toast (or a "save failed" toast in 0.4.4+) → re-read step 4.

Full hotkey reference: [Hotkeys](Hotkeys).

## 6. Pick an output mode

Default output varies per build. To change:

- Edit `wiz3D_Config.xml` next to the DLLs — look for the `<OutputMode>` entry.
- Full list and when-to-use: [Output Modes](Output-Modes).

For Xreal / Nreal glasses: **Full-SBS** or **TAB**.
For 3D TVs: **SBS** or **TAB** (match what the TV expects).
For Leia / SR panels: **SR Weave**.
For NVIDIA shutter glasses (modern path): **NV3D** (0.5.0+).

## 7. If something doesn't work

Head to [Troubleshooting](Troubleshooting). Common cases:

- Game crashes on launch → wrong architecture (32/64) or wrong API folder.
- Game runs but no stereo depth → check `wiz3D_Config.xml` for `<StereoActive Value="1"/>`.
- Stereo wrong eye / inverted depth → set `SwapEyes` in `UserProfile.xml` or via hotkey.
- UI floats at wrong depth → per-game ShaderFix, open an issue with logs.
