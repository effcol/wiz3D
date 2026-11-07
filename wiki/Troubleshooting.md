# Troubleshooting

Common problems, in rough order of how often they come up.

## Game crashes immediately on launch

1. **Wrong architecture.** DX10-11 games may be 32-bit even in 2020s titles. Right-click game exe → Properties → Details → look at "Platform" (or try Task Manager → Details). Switch between `DX10-11/32-bit/` and `DX10-11/64-bit/`.
2. **Wrong API folder.** If the game is DX9 and you copied DX10-11 DLLs, the wrong `d3d9.dll` isn't being proxied so the game sees nothing special — but a `d3d11.dll`+`dxgi.dll` next to a DX9 exe can still confuse some anti-cheat / crash handlers.
3. **Antivirus / EAC / BattlEye.** wiz3D is a proxy DLL. Any kernel-level anti-cheat will reject it. There's no workaround — those games are off-limits.
4. **Missing OutputMethods subfolder.** The output compositors live in `OutputMethods/` next to the main wrappers. Must be copied as a sibling folder, not flattened.

## Numpad 4 seems to work but Ctrl+F5 wipes my settings

This is almost always a **write-permission problem**, not a wiz3D bug.

**Diagnosis**: run the game as Administrator once. Tune to your taste. Press Numpad 4. Exit. Open the game's folder in Explorer — is `UserProfile.xml` present with recent timestamp? If no, the write was silently dropped before.

**Cause**: `C:\Program Files*` is write-locked for non-elevated processes. Numpad 4 attempts a save, Windows discards it, the in-memory tune is still live, Ctrl+F5 then reloads defaults from the empty disk file.

**Fix** (pick one):

1. Run the game's launcher **as administrator** (easiest).
2. Move the game to a non-Program-Files location (e.g. `D:\Games`).
3. Grant your Windows user **Modify** permission on the game folder.

0.4.4 will fall back to `%AppData%\wiz3D\<ExeBaseName>\UserProfile.xml` when the game folder isn't writable, with a loud "save fell back to …" toast so the user sees it happen.

## Ctrl+F5 reset something I didn't want reset

Ctrl+F5 reloads **from disk**. Anything you tuned that hasn't been auto-persisted to `UserProfile.xml` is thrown away.

- **StereoBase, One_div_ZPS, ConvergenceShift, preset-switch, StereoActive, AutoFocusEnable** — these auto-save ~5s after you stop adjusting. Ctrl+F5 keeps them. If you Ctrl+F5 immediately after an adjustment, the deferred write may not have fired yet — pause a beat first.
- **ShaderHunter Mono CRCs** — these save when you press Numpad 4 inside Hunter. If you didn't press Numpad 4, the mono-ing doesn't survive Ctrl+F5.

## Stereo looks wrong (inverted / cross-eyed / flat)

- **Inverted depth** — things sticking out of the screen that should be inside, or vice versa. Set `<SwapEyes Value="1"/>` in `UserProfile.xml` for this game, or add the game-specific profile override in `BaseProfile.xml`.
- **Flat / monoscopic** — stereo turned off? Press **Numpad *** to toggle. If it stays flat, check `wiz3D_Config.xml` for `<StereoActive Value="1"/>`.
- **UI floats at wrong depth** — standard per-game ShaderFix territory. Enter ShaderHunter (**Ctrl+Numpad 0**), cycle shaders with Numpad 1/2, press Numpad 4 on the UI one(s) to mark them mono. See [ShaderHunter](ShaderHunter).

## Mouse cursor visible on both eyes / misaligned in SBS or SR

Known gap. The game's OS-level hardware cursor renders *after* the SBS / Weave composite, so it lands over the whole screen width rather than aligned to either eye's half. A per-eye cursor layer is planned for 0.5.0 (basic: hide OS cursor, draw a stereo-correct one at screen depth; advanced: depth-aware so it sits at scene depth under the pointer).

Software cursors (ones drawn as part of the scene by the game itself) are per-game ShaderFix territory.

## "Game runs but no 3D effect at all"

- OSD not showing → wrapper didn't load. Check you copied the right DLLs; some games bundle their own `d3d9.dll` that takes precedence.
- OSD shows but no depth → `<StereoActive Value="0"/>` somewhere; press Numpad *.
- OSD shows, stereo on, but both eyes look identical → some engines bypass the standard constant-buffer layout. This is where per-game ShaderFixes / the DX11 Feature Engine come in.

## HD3D or 3D Vision Direct doesn't engage

- Check `NativeProfile.xml` (next to the game exe) has a matching `<Profile Name="GameExeName.exe">` entry with the right `NativePath`.
- HD3D only works on **AMD GPUs** with HD3D-capable drivers.
- 3D Vision Direct is an NVIDIA path but **does not drive shutter glasses directly** — it's a compositor. For modern NV3D glasses output use the **NV3D** OutputMode (0.5.0+) which drives NV3D-Lib.

## Logs

Logs live next to the game exe under `wiz3D_Logs/`. When filing an issue, include:

- `wiz3D_Logs/*.log` from the run that reproduced the problem.
- Your `UserProfile.xml` (if you have one for the game).
- wiz3D version, GPU, GPU driver, OS, output mode you were using.

## Still stuck?

- GitHub Issues: https://github.com/effcol/WiZ3D/issues
- Include the log files, version info, and a 30-second description of the symptom.
