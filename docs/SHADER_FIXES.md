# Shader fixes in wiz3D

wiz3D authors stereo correctness per game via two complementary layers:

1. **CB-math features** (DX10/11, declarative XML in `BaseProfile.xml`) — the engine edits constant-buffer bytes before each eye's draws so the game's untouched shaders naturally produce per-eye output. One profile tunes a whole engine family (UE3/UE4, CryEngine, RAGE, Dunia, etc.). This is the primary system; most games get full stereo this way with no shader-level work.
2. **Shader-level fixes** (DX9 today, DX11 next release) — surgical per-shader bytecode replacement keyed by CRC32. For the ~15-30% of effects that can't be expressed as a CB edit: shaders that bypass CBs for stereo-relevant math (HUDs, crosshairs), write custom depth into output registers, or construct their own transforms from loose scalars.

This doc covers the shader-level fix layer. For CB-math features, see the inline comments in `S3DWrapper10/Feature*.cpp` (37 features, each with its XML tag documented in the file header) and the Profile entries in `S3DDriverSetup/Content/BaseProfile.xml`.

---

## DX9 — iZ3D-native profile XML

Fix authoring uses the iZ3D-era declarative profile vocabulary in `BaseProfile.xml`: `<Mono>` for shader-level stereo-off, `<Multiplier>` / `<ConvergenceShift>` for per-CRC stereo-param overrides, and `<Matrices>` for projection-matrix identification. Hunter captures the CRC — authors paste it into the right tag.

```xml
<Profile Name="Example Game">
    <File Name="game.exe"/>
    <Mono>
        <VertexShader CRC="0xA3B5F218"/>
    </Mono>
</Profile>
```

The DX9 HelixMod-compatible user-HLSL side-door (external `ShaderOverride/<CRC>.txt` loader + `texldl` detection + stereo-params texture) was removed in 0.4.2. See the Hunter report's authoring suggestion for the inline iZ3D-native alternative.

## DX10 / DX11 — shipped and shipping

**0.4.0 (shipped):**
- **CB-math features** (Akshay's PR #31). 37 declarative tags in `BaseProfile.xml` cover most engine families (UE3, UE4, CryEngine, RAGE, Dunia).

**0.4.1 (shipped):**
- **ShaderHunter** — `Ctrl+Numpad 0` enters hunt mode, Numpad 1 / Numpad 2 cycle through CRCs drawn this frame (symmetric left/right skip to keep replay in sync), Numpad 3 captures a mini-report with disasm + CB layout + suggested `<ShaderPatch>` skeleton to `wiz3D_Shaders/<Game>/Hunter/<CRC>_<stage>_<ts>.txt`. OSD strip bottom-centre while active. No config knob — the hotkey is itself the entry point. Numpad bindings follow 3Dmigoto / HelixMod convention so shaderhackers coming from those tools don't learn a new layout.
- **`<ShaderPatch>` shorthand** (CRC-keyed, declarative). First primitive: `<SkipStereo/>` — forces a draw mono-both-eyes by short-circuiting `Context11Proxy::EditRightCBs` for the right eye when the bound VS or PS matches. Covers HUD leaks, specific crosshairs, screen-space shaders that shouldn't shift.

**Example:**
```xml
<Profile Name="Example Game">
    <File Name="game.exe"/>
    <!-- Broad engine coverage -->
    <DetectInverseMatrix Value="1"/>
    <RightCamera Buffer="ViewParams" View="r8" .../>
    <!-- Per-shader fix: this HUD vertex shader leaks into stereo depth -->
    <ShaderPatch CRC="0xA3B5F218" Stage="VS">
        <SkipStereo/>
    </ShaderPatch>
</Profile>
```

The hunter's capture report pre-fills a `<ShaderPatch>` skeleton with sensible defaults for the shader it captured, so authoring is often one copy-paste from the Hunter/ output into BaseProfile.xml.

**0.4.3 / 0.5.0 (current branch — new primitives):**

Eight new `<ShaderPatch>` primitives + a sub-filter. All are declarative XML inside an existing `<ShaderPatch CRC=...>` block — no HLSL required. All are DX11-only unless noted; the DX9 wrapper ships `<SkipStereo/>` + `<When TextureCRC>` with the 0.4.3 line.

- **`<SkipStereo/>`** (DX9 + DX11, cross-wrapper) — force a draw mono-both-eyes. On DX9 this also pins `m_Multiplier=0` so the iZ3D shader-asm path can't re-apply stereo divergence under a different code path.

- **`<SpriteDepth/>`** (DX11) — analyser replaces a shader's constant depth output with the sampled scene depth so 2D sprites sit at the geometry they occlude. XML override fields:
  ```xml
  <ShaderPatch CRC="0x..." Stage="PS">
      <SpriteDepth TexSlot="3" DepthReg="o0" DepthComponent="z"/>
  </ShaderPatch>
  ```

- **`<FixedDepth>`** (DX11) — pin a shader's output to a specific scalar depth (either a fixed value or the current Screen Plane). Twin-shader emits `out.x += cb[0].x` before every `ret`.
  ```xml
  <ShaderPatch CRC="0x..." Stage="VS"><FixedDepth Value="0.5"/></ShaderPatch>
  ```

- **`<DepthFromCB>`** (DX11) — read the depth from a specific CB slot + byte offset. Builds on the FixedDepth twin plan.
  ```xml
  <ShaderPatch CRC="0x..." Stage="VS"><DepthFromCB Slot="1" OffsetBytes="48"/></ShaderPatch>
  ```

- **`<AnimateDepth>`** (DX11) — animate depth over a CB-driven progress value (0..1 lerp from Start to End).
  ```xml
  <ShaderPatch CRC="0x..." Stage="VS">
      <AnimateDepth ProgressCB="2" OffsetBytes="16" Start="0.0" End="1.0"/>
  </ShaderPatch>
  ```

- **`<ShiftUV>`** (DX11) — apply a per-eye UV offset on an output register. Twin-shader emits `out.<component> += cb[0].x` where cb[0].x carries the per-eye sign * amount.
  ```xml
  <ShaderPatch CRC="0x..." Stage="PS">
      <ShiftUV OutputReg="o1" Component="x" Amount="0.004"/>
  </ShaderPatch>
  ```

- **`<ParallaxLayer>`** (DX11) — convenience shorthand around `<ShiftUV>` for 2D parallax layering (Castlevania / DuckTales / sprite-games). Preset amount + component, no asm required.

- **`<PerEyeTexture>`** (DX11) — swap the SRV bound at a slot to a different pre-rendered texture on the right eye. Lazy scan of currently-bound SRVs keyed by FNV-1a 32-bit hash over the `ID3D11Resource*` pointer — no persistent cache.
  ```xml
  <ShaderPatch CRC="0x..." Stage="PS">
      <PerEyeTexture Slot="0" LeftCRC="0x..." RightCRC="0x..."/>
  </ShaderPatch>
  ```

- **`<OverrideCBValue>`** (DX11) — write a specific float into the right-eye CB. Uses `GetRealRight()` preferring the right-eye sibling buffer.
  ```xml
  <ShaderPatch CRC="0x..." Stage="VS">
      <OverrideCBValue Slot="2" OffsetBytes="64" Value="0.25"/>
  </ShaderPatch>
  ```

- **`<When TextureCRC="...">`** sub-filter (DX9 + DX11) — scope any of the above to fire only when a specific texture is bound. Lets one profile handle "mono this HUD shader *except* when it's drawing the mini-map" or "shift this UV only for the sprite sheet". FNV-1a 32-bit hash over `ID3D11Resource*` (DX11) or `IDirect3DBaseTexture9*` (DX9) pointer, captured via Hunter's Numpad 5 draw-level fingerprint.
  ```xml
  <ShaderPatch CRC="0x..." Stage="PS">
      <When TextureCRC="0x8F3A21BC"><SkipStereo/></When>
  </ShaderPatch>
  ```

**New Hunter features (0.4.3 / 0.5.0):**
- **`Numpad 4`** (DX11 + DX9) — in hunt mode, mono-force the selected CRC and persist to `UserProfile.xml`. Press again on the same CRC to un-mono (toggle semantics via `IsMonoShaderCRCInUserProfile` + `RemoveMonoShaderCRCFromUserProfile`).
- **`Numpad 5`** (DX11) — toggle hunt granularity between CRC-level (default) and Draw-level. In Draw mode, Numpad 1/2 cycle individual draws of the selected CRC; the Hunter capture report records per-draw fingerprints (bound textures + bound CB slot values) so you can key a `<When TextureCRC>` filter to the exact draw.
- **Cycle-flash** (DX11 + DX9) — the selected shader/draw flashes on/off at 2 Hz (15/30 frames hidden per period) so you can see at a glance which element is currently suppressed. Atomic `s_hidden` / `s_flashFrame` state, restarts on cycle/toggle.
- **Draw-level hunt** (DX11 + DX9 Hunter parity, 0.5.0) — sub-draw selection within a single CRC's draws. Required for games that reuse one shader CRC across multiple logical elements (UI atlases, decal passes).

**Explicitly out of scope:** inline user-HLSL (CDATA override blocks, stereo-params SRV, HelixMod-style replacement shaders). The CRC-keyed declarative path is the only user-facing fix authoring surface — fix fragmentation and licensing complexity from HLSL side-doors (see Geo-11) are what we're avoiding.

**Deferred / planned:**
- `<InjectMath>` — bounded DSL for anchored bytecode insertions beyond the Fixed/Shift/CB primitives.
- `<ReplaceShader>` + an LGPL-licensed template library.
- DX9 parity for `<DepthFromCB>`, `<AnimateDepth>`, `<PerEyeTexture>`, `<SpriteDepth>` (dynamic world-pos). XML parses on DX9 today; runtime deferred until a concrete DX9 game surfaces that needs them. Author workaround: use the DX11 wrapper if the game has a DX11 renderer available.

See `S3DWrapper10/ShaderModifier11.cpp` (automatic VS eye-shift token rewrite) and `S3DWrapper10/ShaderPatch11.cpp` (automatic right-eye shader twinning) for the existing engine-internal shader-modification paths. Both run without user authoring.

### Hash function and tooling compatibility

wiz3D uses **IEEE 802.3 CRC32** (polynomial `0xEDB88320`) for shader identification on both DX9 and DX11. The DX11 implementation (`S3DWrapper10/ShaderAnalyzer11.cpp::ComputeCRC32`, originally from Akshay's PR #31) matches the DX9 wrapper's hash and matches the HelixMod DX9 convention, so a CRC captured by Hunter can be cross-referenced against HelixMod `.txt` filenames and community DX9 fix lists without re-hashing.

**Not compatible with geo-11 / 3Dmigoto.** Chiri's tools (3Dmigoto, geo-11) use an FNV-1a 64-bit hash for DX11 shader identification (16-digit hex), not CRC32 (8-digit hex). The two hashes produce different values for the same shader bytecode, so an author building a wiz3D corpus **cannot cross-reference** by shader ID to a geo-11/3Dmigoto corpus. If a tooling bridge is wanted later, a hash-translation utility (compute both hashes from the bytecode, emit a lookup table) is the right place to add it — wiz3D's internal hash stays CRC32 for DX9/DX11 symmetry and HelixMod compatibility.

## DX8 — not applicable

DX8 shaders are SM1.0/1.1 assembly only, and the fix authoring surface is the same iZ3D-native profile XML the DX9 wrapper uses — Hunter + `<Mono>` / `<Multiplier>` / `<Matrices>`.

## DX7 — not applicable

Fixed-function pipeline, no programmable shaders. Stereo correctness is handled by projection-matrix manipulation in the fixed-function state — no shader-level fix layer is needed or possible.

## Authoring switches (`wiz3D_Config.xml`)

Under `<GlobalSettings>`:

- `CollectDebugInformation="1"` — DX10/11 only. Reports CB layouts and matrix candidates to `wiz3D_proxy.log`. Primary authoring aid for CB-math profiles.
- `DumpVS="1"` / `DumpPS="1"` — dump every shader created by the game. Lands in:
  - DX10/11: `wiz3D_Shaders/<GameName>/Dump/{VS,PS,GS,HS,DS,CS}.txt`
  - DX9: `wiz3D_Shaders/<GameName>/Dump/{vs,ps}.{client,converted,modified}.txt`

  Note: the folder name mirrors `wiz3D_Logs/` and sits next to the driver DLL — the historical `%ALLUSERSPROFILE%\DX9ShaderDataCache\<Profile>.vsc`/`.psc` from iZ3D is a separate thing (binary runtime cache for repeat-launch startup; keyed by profile not game) and stays where it is.

  Developer shortcut: set the `WIZ3D_FORCE_DUMP=1` environment variable to force-enable `DumpVS` and `DumpPS` regardless of what the shipped `wiz3D_Config.xml` says. One env var, works for every game, never ships. Useful when you're actively authoring fixes and want dumps on by default without having to flip the Config.xml knob for each install.
- `VerboseFrameTrace="N"` with `FrameTraceStartFrame="M"` — DX10/11 only. Per-frame draw-by-draw timeline into `wiz3D_proxy.log` for `N` frames starting at Present #`M`.

Expensive flags — enable only during authoring, revert to `0` for play.

## Licensing

All in-repo profiles and shader fixes contributed via PR fall under the project's LGPL-2.1 by default (new code). Original iZ3D files retain their MIT headers.

**Do not submit shader fixes that are ports or derivatives of GPL-licensed HelixMod / Geo-11 / 3DMigoto fixes.** Clean-room reimplementations only, with pattern attribution as a courtesy. The repo's shader-fix pipeline is a *consumer* of the standard fix format, not a redistributor of upstream fix corpora.

If a specific community fix would materially help a shipped profile, ask the original author to grant us a fresh version under LGPL-2.1 (or MIT, which composes cleanly). Many will say yes.
