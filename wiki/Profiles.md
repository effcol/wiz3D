# Profiles

wiz3D reads three XML files on start:

| File | Lives next to | What it holds | Who edits it |
|---|---|---|---|
| **BaseProfile.xml** | wiz3D DLLs (shared install) | Shipped per-game ShaderFixes + default knobs for every known title | wiz3D project (PRs welcome) |
| **UserProfile.xml** | the **game's exe** (0.4.3+) | Your personal overrides for this game — tuned StereoBase / One_div_ZPS / AutoFocusEnable, plus `<Mono>` entries saved via Numpad 4 | You, in-game (hotkeys auto-save) |
| **NativeProfile.xml** | the **game's exe** (0.4.3+) | Whether a game should use HD3D / NvDirectMode / SR instead of the normal Stereo-Injector | You + wiz3D ships a seed list |

`UserProfile.xml` **always wins** over `BaseProfile.xml` for the same game. ShaderFixes merge (both files' fixes apply); scalar knobs override (UserProfile replaces BaseProfile).

## Location migration (0.4.3)

Up to 0.4.2 both `UserProfile.xml` and `NativeProfile.xml` lived under `%AppData%\wiz3D\`. From 0.4.3 they live **next to each game's exe**. Benefits:

- Per-game isolation — a fix for TinTin can't leak into Crysis.
- Discoverability — users expect profiles alongside the install.
- The Numpad 4 "saved to…" toast points at a path one Alt-Tab away.

Migration: if the game-folder file doesn't exist on first launch but the legacy `%AppData%\wiz3D\` one does, we copy it over automatically. Subsequent saves write to the new location.

## BaseProfile.xml schema (abridged)

```xml
<Profiles>
    <Hotkey name="StereoBaseIncrease" value="107"/>
    ...
    <Profile Name="Default">
        <!-- global defaults, inherited by any game without a specific block -->
    </Profile>
    <Profile Name="MyGame.exe">
        <Pixel>
            <!-- pixel-shader primitives, keyed by CRC -->
            <Shader CRC="0xDEADBEEF" Mono="true"/>
            <Shader CRC="0xCAFEBABE" ConvergenceShift="-0.05"/>
        </Pixel>
        <Vertex>
            <Shader CRC="0x12345678" Multiplier="0.5"/>
        </Vertex>
        <Weapon MaxZ="0.5" Multiplier="0.3"/>
    </Profile>
</Profiles>
```

Full primitive catalogue: see [ShaderFixes](ShaderFixes) and `docs/SHADER_FIXES.md` in the repo.

## UserProfile.xml schema

Written by the wrapper. Hand-editing is fine (useful for backing up a good set of knobs), but keep the shape.

```xml
<UserProfile>
    <Profile Name="MyGame.exe">
        <ShowWizardAtStartup Value="0"/>
        <SwapEyes Value="0"/>
        <LaserSightEnable Value="0"/>
        <StereoActive Value="1"/>
        <Presets Active="1">
            <Preset Index="0">
                <One_div_ZPS Value="0.5"/>
                <StereoBase Value="0.12"/>
                <AutoFocusEnable Value="0"/>
            </Preset>
            <!-- Preset Index="1" and "2" similarly -->
        </Presets>
        <ConvergenceShift Value="0.0"/>
    </Profile>
</UserProfile>
```

### What auto-saves vs what doesn't

| Knob | Auto-saves when you change it? | Keybind |
|---|---|---|
| Preset StereoBase | ✅ (deferred write ~5s) | Num +/- |
| Preset One_div_ZPS | ✅ | Shift+Num +/- |
| AutoFocusEnable | ✅ | Num / |
| StereoActive | ✅ | Num * |
| SwapEyes | ✅ | config only (no hotkey) |
| Active preset | ✅ | Num 7/8/9 |
| ConvergenceShift (global) | ✅ (0.4.3+) | Alt+Num +/- |
| ShaderHunter Mono CRCs | ✅ on Numpad 4 (that *is* the save) | Num 4 while in Hunter |

## NativeProfile.xml schema

```xml
<NativeProfiles Version="1">
    <Profile Name="MaxPayne3.exe">
        <StereoActive Value="1"/>
        <NativePath Value="NvDirectMode"/>
    </Profile>
    <Profile Name="DXHRDC.exe">
        <StereoActive Value="1"/>
        <NativePath Value="HD3D"/>
    </Profile>
</NativeProfiles>
```

`NativePath` values: `HD3D`, `NvDirectMode`, `SR` (0.5.0+). Matched by exe basename.

Used by the HD3D and NvDirectMode proxies to decide whether to engage for a given game — avoids them trying to do anything on titles that don't support those native paths.

## Hot reload (Ctrl+F5)

Both `BaseProfile.xml` and `UserProfile.xml` can be edited in a text editor while the game is running. Hit **Ctrl+F5** and the wrapper re-parses both at the next frame boundary.

**Caution**: Ctrl+F5 also restores `Preset[]` + `ConvergenceShift` from the snapshot captured at profile-load time. So any in-game hotkey tweak that hasn't yet auto-saved to `UserProfile.xml` is lost. See [Hotkeys → Profile reload](Hotkeys#profile-reload) for the exact semantics.

## Where logs go

Separate from profiles but useful to know:

- `wiz3D_Logs/` — all runtime logs (next to the game exe).
- `wiz3D_Logs/Shaders/Dump/` — DumpVS / DumpPS disassembly captures.
- `wiz3D_Logs/Shaders/Hunter/` — ShaderHunter Numpad 3 captures.
- `wiz3D_Logs/Screenshots/` — PrintScreen stereo screenshots.
