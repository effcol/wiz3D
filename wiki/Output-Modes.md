# Output Modes

wiz3D produces two eye-frames and composites them into a single image your display can consume. The composite step is called the **OutputMode**. Pick one in `wiz3D_Config.xml` next to the DLLs by name or number — both parse:

```xml
<OutputMode Value="HalfSBS"/>
<!-- or -->
<OutputMode Value="1"/>
```

Every wrapper (DX7, DX8, DX9, DX10/11, OpenGL-QB, HD3D, 3D-Vision-Direct) exposes the same enum from 0.5.0 onward.

## Picking a mode by display

| Display | Try first |
|---|---|
| Xreal / Nreal / Rokid glasses (any SBS-capable AR) | `FullSBS` |
| 3D TVs expecting half-width SBS | `HalfSBS` |
| 3D TVs expecting half-height TAB | `HalfTAB` |
| Passive polarised 3D monitor / LG Cinema 3D TV | `RowIL` |
| Lenticular / column-interleaved display | `ColumnIL` |
| Checkerboard 3D TV / DLP-Link projector | `Checker` |
| Red/cyan glasses | `Anaglyph` |
| NVIDIA shutter glasses (modern) | `3DVisionGlasses` |
| Dual-display mirror rig | `Mirror` |
| Sharing stereo to [bo3b's Katanga](https://github.com/bo3b/Katanga) external viewer | `Katanga` |

## Full list

| # | Name | What it does | Hardware / software notes |
|---|---|---|---|
| 0 | `iZ3D` | iZ3D dual-panel (legacy) | iZ3D hardware; modern wrappers fall back to Half SBS |
| 1 | `HalfSBS` | Half side-by-side | Any stereo panel / TV with SBS input |
| 2 | `FullSBS` | Full side-by-side (full width per eye) | Stereo displays expecting full-width per eye |
| 3 | `HalfTAB` | Half top-and-bottom | TVs with "Over/Under" stereo input |
| 4 | `FullTAB` | Full top-and-bottom (full height per eye) | Full-resolution per eye, stacked |
| 5 | `Anaglyph` | Red/cyan Dubois | Any monitor + red/cyan glasses |
| 6 | `RowIL` | Row interleaved | Passive 3D TVs (LG Cinema 3D etc.) |
| 7 | `ColumnIL` | Column interleaved | Lenticular / column-interleaved displays |
| 8 | `Checker` | Checkerboard | DLP-Link projectors, some older DLP 3D TVs |
| 9 | `SRWeave` | Lenticular weave via SimulatedRealityWeaveOutput.dll | Leia / Simulated-Reality panels; LeiaSR runtime required |
| 10 | `Mirror` | StereoMirror | Dual-display mirror rig |
| 11 | `3DVisionGlasses` | NVIDIA 3D Vision shutter via NV3D-Lib | IR emitter + shutter glasses + 120 Hz 3D-Vision-Ready panel; needs legacy R417-or-earlier NVIDIA driver *or* Oscar's [Vision Restoration](https://github.com/oscardelatorre/VisionRestoration) patch on modern drivers. Auto-falls-back to Half SBS when the hardware/driver chain isn't complete. |
| 12 | `Katanga` | Shared wide-SBS DX11 texture for [bo3b's Katanga](https://github.com/bo3b/Katanga) external viewer | Launch Katanga separately *before* the game; it picks up the shared handle automatically. Works across all 6 wrappers — DX7/8/9 go via the DX11 bridge, OpenGL uses GL/DX interop. |

**Default** is `SRWeave` on shipped Config.xml because the SR runtime is the only free-software cross-vendor path to a modern 3D display today. For a Half-SBS TV, flip to `HalfSBS`. For NVIDIA 3D Vision hardware, pick `3DVisionGlasses`. For debugging or sharing to a remote 3D viewer, pick `Katanga`.

`OutputMethodDll` (sibling knob) is the legacy iZ3D plugin filename selector; leave blank to let the modern pipeline backfill from `OutputMode`. Set only to pin a specific legacy DX7-9 output DLL (`SideBySideOutput.dll`, `CMOShutterOutput.dll`, Zalman / VR920 / Z800 / Lenovo hardware drivers, etc.).
