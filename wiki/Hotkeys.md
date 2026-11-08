# Hotkeys

Default bindings as of **0.4.3**. Every key can be remapped by editing the `<Hotkey>` entries at the top of `BaseProfile.xml` — the names in the "Binding" column below are the XML names to use.

## Stereo tuning

| Keys | Action | Binding name |
|---|---|---|
| **Numpad +** / **Numpad -** | Separation ± (StereoBase) | `StereoBaseIncrease` / `…Decrease` |
| **Shift + Numpad +** / **-** | Convergence ± (One_div_ZPS, the zero-parallax-screen depth) | `ZPSIncrease` / `…Decrease` |
| **Alt + Numpad +** / **-** | Global ConvergenceShift ± (comfort trim on top of the preset) | `ConvergenceShiftIncrease` / `…Decrease` |
| **Ctrl + Alt + Numpad +** / **-** | Fine ConvergenceShift ± (5× smaller step) | `ConvergenceShiftIncreaseFine` / `…Decrease…` |
| **Numpad \*** | Toggle stereo on / off | `ToggleStereo` |
| **Numpad /** | Toggle autofocus on the active preset | `ToggleAutoFocus` |

## Presets

Three per-game camera presets (comfort / default / strong). Each stores its own StereoBase + One_div_ZPS + AutoFocusEnable.

| Keys | Action |
|---|---|
| **Numpad 7** | Select Preset 1 (low / comfort, default 15% separation) |
| **Numpad 8** | Select Preset 2 (default, 20%) |
| **Numpad 9** | Select Preset 3 (strong, 30%) |

## OSD and info

| Keys | Action |
|---|---|
| **Ctrl + F1** | Toggle OSD (status strip + FPS) |
| **Shift + F1** | Toggle hotkey-help overlay |
| **Shift + Numpad \*** | Toggle wizard overlay |

## ShaderHunter (per-game fix discovery)

ShaderHunter is an in-game CRC-browse mode. Enter it, cycle shaders, see what each one draws, mark the ones you want fixed.

| Keys | Action |
|---|---|
| **Ctrl + Numpad 0** | Enter / exit ShaderHunter |
| **Numpad 1** / **Numpad 2** | Previous / next CRC (or previous / next draw in Draw-granularity mode) |
| **Numpad 3** | Capture current CRC to `wiz3D_Logs/Shaders/Hunter/` |
| **Numpad 4** | Force current CRC to mono **and persist it to UserProfile.xml** |
| **Numpad 5** | Toggle hunt granularity (CRC-level ↔ Draw-level within a CRC) |

See [ShaderFixes](ShaderFixes) and [ShaderHunter](ShaderHunter) for the full workflow.

## Profile reload

| Keys | Action |
|---|---|
| **Ctrl + F5** | Re-read `BaseProfile.xml` + `UserProfile.xml` from disk |

**What Ctrl+F5 actually does**: it throws away your in-memory `Preset[]` and `ConvergenceShift` and restores them from the snapshot captured when the profile was last loaded. Then it re-parses `BaseProfile.xml` + `UserProfile.xml` from disk so any text-editor changes to those files take effect live.

This means **any adjustment you made with the hotkeys above that hasn't been written to `UserProfile.xml` is discarded**. Numeric stereo adjustments (StereoBase, One_div_ZPS, ConvergenceShift) all auto-write — a deferred `WriteInputData` fires ~5 seconds after you stop tuning. So if you've been nudging comfort while in-game, waiting a beat before you hit Ctrl+F5 keeps the value.

## Debug / dev-only (NOT in release builds)

These are compiled out of the shipping `Final Release` build but may appear if you're running a `Release` CI smoke build:

- **Numpad 1 / 2 / 3** when not in ShaderHunter — ZNear / Multiplier / page-switch (dev).

## Screenshot

| Keys | Action |
|---|---|
| **Ctrl + F12** | Save stereo screenshot to the install's `wiz3D_Logs/Screenshots/`. Moved off PrtScn in 0.4.3 (issue #49) so Steam / Windows Shift+S / Afterburner don't fight us for the key. |

## Hotkey OSD

In-game, press **Shift + F1** to see a condensed version of the above overlaid on the screen.
