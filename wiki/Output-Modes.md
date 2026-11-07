# Output Modes

wiz3D produces two eye-frames and then **composites** them into a single output image that your display can consume. The composite step is called the **OutputMode**. You pick it in `wiz3D_Config.xml` next to the DLLs:

```xml
<OutputMode Value="SR_Weave"/>
```

Some modes also accept **sub-modes** (variant layouts, 3D-TV-friendly aliases) — those are listed per-mode below.

## Picking a mode by display

| Display | Try first |
|---|---|
| Xreal / Nreal / Rokid glasses (any SBS-capable AR) | **Full-SBS** |
| 3D TVs (Samsung / Sony / LG passive and active) | **SBS** or **TAB** (match what the TV advertises) |
| Checkerboard 3D TV (older DLP sets) | **Checkerboard** |
| Interlaced polarised monitor | **Interlaced** |
| Leia / Simulated-Reality panel (Acer SpatialLabs, Samsung Odyssey 3D) | **SR Weave** |
| NVIDIA shutter glasses + modern wiz3D | **NV3D** (0.5.0+) — drives NV3D-Lib directly |
| AMD HD3D-capable game | **HD3D native path** (lives outside Stereo-Injector; see [Native-HD3D](Native-HD3D)) |
| Monoscopic (debug) | **Mono-left** or **Mono-right** |

## Full list

### SBS (side-by-side, half-width)

Classic 3D-TV SBS. Left eye in left half, right eye in right half. Each eye is horizontally squashed to half the display width, so you need display-side desqueezing.

Aliases: `SBS`, `Side-by-Side`.

### Full-SBS

Side-by-side at **full width per eye** — total output width is 2× the per-eye render. For AR glasses that expect a full-resolution SBS signal.

Aliases: `Full-SBS`, `Full_SBS`.

### TAB (top-and-bottom, half-height)

Left on top, right on bottom, each squashed vertically. 3D-TV-friendly.

Aliases: `TAB`, `Top-and-Bottom`, `OverUnder`.

### Full-TAB

TAB at full per-eye height.

Aliases: `Full-TAB`.

### Interlaced

Odd scanlines = left, even = right. Needs a passive polarised monitor or active-glasses driver that expects line interlace.

### Checkerboard

Alternating pixels. Common on early DLP 3D TVs.

### SR Weave

Lenticular weave for Leia / Simulated-Reality panels. Uses the SRWeave passthrough path (phase-2a, validated with CP2077 on Acer SpatialLabs 2026-10-30).

**Note**: the SR weave is currently sized to the game's back-buffer, not panel-native. For sub-native fullscreen (e.g. UDK/UE3-era games), DWM upscale will misalign the weave. Running the game at panel-native resolution works around it. Fix is tracked as a 0.6.0+ item.

### NV3D (0.5.0+)

Drives NV3D-Lib directly for genuine shutter-glasses output. Available across all six wrappers after the 0.5.0 roll-out.

### HD3D

Not a Stereo-Injector OutputMode — HD3D lives outside the injector as its own set of proxy DLLs. See [Native-HD3D](Native-HD3D).

### 3D Vision Direct (NvDirectMode)

Also outside the Stereo-Injector — see [Native-3D-Vision-Direct](Native-3D-Vision-Direct). The "NvDirectMode" OutputMode inside the injector is a per-eye capture + compositor, not a shutter driver.

### Mono (debug)

- **Mono-left** — only the left eye is shown (use to check reference correctness).
- **Mono-right** — ditto for the right eye.

## Submode mapping (0.4.3)

The 0.4.3 output-mode refactor unified the submode enum across all wrappers (DX7–DX11, OGL). The aliases above all map onto the shared `OUTMODE_*` values, so you can use whichever spelling is most comfortable and the wrappers will normalise. If you spot a case where an alias doesn't resolve, please file a bug — that's a regression we want to fix.
