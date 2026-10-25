# ShaderHunter design (DX11 first, DX9 to follow)

**Status:** design draft, pre-implementation. Targeting 0.5.0.
**Author:** Effie + Claude Opus 4.7
**Reviewer:** Akshay (owner of the PR #31 Feature engine + Context11Proxy)

## Problem

Dumping every shader via `DumpVS="1"` / `DumpPS="1"` is the DX10/11 state of the art today. For a typical AAA game that produces a `VS.txt` with 1000+ shaders concatenated. There is no way to identify *which* shader draws a specific visually-wrong effect — the author has to read every disassembly.

Helix / 3DMigoto / Geo-11 solve this with **shader hunting**: a hotkey cycles the currently-drawn shaders at runtime, hiding each one in turn. When the broken effect disappears, you've identified the CRC. The tool then dumps that one shader's bytecode + context to a dedicated file.

wiz3D needs the same thing to make the planned ShaderPatch / inline ShaderOverride system practically authorable.

## Design — three touch points

### 1. No config knob — hotkey is the entry

Earlier revisions of this doc proposed a `<EnableShaderHunter>` config field. Dropped in review: the Numpad 0 hotkey is itself the opt-in. Pressing it enters hunt mode, pressing again exits. All hunter machinery is always compiled in and wired up; `IsActive()` returns false until Numpad 0 is pressed, which short-circuits every hot-path call (`NoteDraw`, `ShouldSkipDraw`) at a relaxed atomic load. Zero overhead when not hunting; one less knob users have to find in XML.

### 2. Hotkey additions (`S3DAPI/GlobalData.h` + `KeyboardHook.cpp`)

Four new `EventId` entries, added before `MAX_EVENTS`:

| EventId                 | Suggested binding | Behavior                           |
|-------------------------|-------------------|------------------------------------|
| `TOGGLE_SHADER_HUNT`    | Numpad 0 (96)     | Enter / exit hunt mode             |
| `HUNT_PREV_SHADER`      | Numpad 1 (97)     | Hide the previous CRC in the frame |
| `HUNT_NEXT_SHADER`      | Numpad 2 (98)     | Hide the next CRC in the frame     |
| `HUNT_CAPTURE_SHADER`   | Numpad 3 (99)     | Dump the current hunted CRC        |

Numpad bindings follow 3Dmigoto / HelixMod convention. iZ3D's hotkey table uses Numpad 7/8/9 + `*`/`+`/`-`/`/` for stereo controls, so Numpad 0-6 slot in without collision. The shaderhacker standard also sidesteps the F-key problem — games routinely grab F1-F12 for their own UI.

Added to the `hotKeyList` initialiser and the KeyboardHook event switch.

Reserve the IDs with inline `#ifndef FINAL_RELEASE` wrapping only on the capture path (dump I/O), not the toggle itself — the toggle is cheap enough to leave live in all configs.

### 3. HuntManager (new `S3DWrapper10/ShaderHunter.{h,cpp}`)

Static-only class, state-owning. Called from four points:

- **Shader create** (`Device11Proxy::CreateVertexShader` / `::CreatePixelShader`, in Akshay's PR#31 code) → `ShaderHunter::NoteCreated(crc, stage)`. Add CRC to a known-shaders set.
- **Draw time** (`Context11Proxy::ApplyFeatures`, same location as the existing CB-math feature pass) → `ShaderHunter::NoteDraw(vsCrc, psCrc)` *and* a check `ShaderHunter::ShouldSkipDraw(vsCrc, psCrc)` returning true if either bound shader is the currently hunted CRC. The context then early-returns from the real `Draw*` call site, substituting a no-op.
- **Frame end** (`SwapChain11Proxy::Present` pre-replay) → `ShaderHunter::FrameEnd()`. Snapshot the drawn-this-frame CRC set, reset the counter.
- **Hotkey handlers** (`KeyboardHook`) → `ShaderHunter::OnToggleHunt()`, `OnCyclePrev()`, `OnCycleNext()`, `OnCapture()`.

State owned by HuntManager:
- `bool m_active`                     — is hunt mode on
- `std::unordered_set<DWORD> m_lastFrameCRCs` — CRCs drawn in the previous frame (iterated at hotkey time)
- `std::vector<DWORD> m_cycle`        — stable ordering for prev/next cycling, rebuilt at FrameEnd
- `size_t m_cycleIdx`                 — current index into `m_cycle`
- `DWORD m_hiddenCRC`                 — the one we're currently suppressing (0 if none)
- `std::unordered_map<DWORD, ShaderMeta> m_known` — per-CRC: stage (VS/PS), signature outputs, last-known CB layout snapshot from the Feature engine's analysis

Thread safety: hotkey handlers run on the KeyboardHook thread; `NoteDraw` / `ShouldSkipDraw` run on the game thread. Protect `m_hiddenCRC` + `m_cycle` + `m_cycleIdx` with a single `std::mutex` — contention is rare (one hotkey hit per keypress, millions of draws per frame on the one atomic read).

### 4. OSD line (`OSDRenderer`)

Add a fourth slot `Slot::Hunter` to `TexSlot m_slots[(int)Slot::_Count]`. Shown when `ShaderHunter::IsActive()`. Content:

```
HUNT  VS:0xABC12345  draws:847  cycle 23/142
Num1 prev   Num2 next   Num3 capture   Num0 exit
```

Reuses the existing `GDIFont m_fontStatus` and `DrawTextLine` plumbing. Position: bottom-centre of the backbuffer. Fade: none — stays visible while hunt mode is active.

### 5. Capture report format (`wiz3D_Shaders/<Game>/Hunter/<CRC>_<ts>.txt`)

Written by `ShaderHunter::DoCapture()` on Numpad 3. Includes everything the author needs to go straight to authoring a `<ShaderPatch>` / `<Mono>` / `<Multiplier>` / `<Matrices>` entry:

```
Shader CRC:    0xABC12345
Stage:         VS
Model:         vs_5_0
Signature:     SV_Position (o0, SV_POSITION), TEXCOORD0 (o1), COLOR0 (o2)
Draws seen:    847 (last frame)
First seen:    frame 142, 00:04:17.332 since game start

Bound CBs on last draw (from Feature engine analysis):
  cb0 "ViewParams" (128 bytes, slot b0)
    r0-r3  : ViewProjection  (matrix row-major, detected)
    r8-r11 : View             (matrix row-major, detected)
    r16-r19: Projection       (matrix row-major, detected)
    r27    : ScreenToWorld    (candidate ScreenRayMatrix)
  cb2 "ObjectParams" (64 bytes, slot b2)
    r0-r3  : WorldMatrix      (matrix, detected)

Bound SRVs on last draw:
  t0: Texture2D DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
  t1: Texture2D DXGI_FORMAT_BC7_UNORM_SRGB

D3DDisassemble:
  vs_5_0
  dcl_globalFlags refactoringAllowed
  dcl_input v0.xyzw
  ...

iZ3D feature engine analysis:
  <RightCamera Buffer="ViewParams" View="r8" Projection="r16"
               ViewProjection="r0" Confidence="0.94"/>

Suggested ShaderPatch skeleton (fill in):
  <ShaderPatch CRC="0xABC12345" Stage="VS" Note="...">
      <!-- Pick one that matches the symptom -->
      <PatchCB Buffer="cb0" Register="r8"  Factor="0.0"/>  <!-- flatten View -->
      <PatchCB Buffer="cb0" Register="r27" Component="x" Value="0.0"/>  <!-- zero screen ray -->
      <SkipStereo/>  <!-- make this draw mono entirely -->
  </ShaderPatch>
```

The "Suggested ShaderPatch skeleton" lines reuse Akshay's existing `FeatureLog` detection output — most games will get something sensible pre-filled.

## Implementation plan — reviewable chunks

1. **New config knob + hotkey IDs** (small, standalone)
   - Add `EnableShaderHunter` field to `GlobalInfo`
   - Add parse in `ReadData.cpp`
   - Add four `EventId` enum entries
   - Add four `hotKeyList` entries
   - No functional change yet; just plumbing.
2. **`ShaderHunter` class skeleton** (header + no-op cpp)
   - Public API fully declared, mostly empty implementations
   - Mutex-protected state
   - `NoteCreated`, `NoteDraw`, `FrameEnd`, hotkey entrypoints — all no-ops that log
3. **Wire the entrypoints** (one commit each)
   - `Device11Proxy` shader-create hooks call `NoteCreated`
   - `Context11Proxy::ApplyFeatures` entry calls `NoteDraw` + `ShouldSkipDraw`
   - `SwapChain11Proxy::Present` calls `FrameEnd`
   - `KeyboardHook::OnEvent` dispatches to `OnToggleHunt` / `OnCyclePrev` / `OnCycleNext` / `OnCapture`
4. **Draw-skip** (small, risky — needs testing)
   - `ShouldSkipDraw` returning true causes the Context11Proxy to short-circuit the real `m_real->Draw*` call. Needs careful placement to avoid breaking replay / CB tracking state.
5. **Capture report** (medium, isolated)
   - `DoCapture` dumps the single-shader mini-report. Reuses `FeatureLog` content.
6. **OSD slot** (small)
   - Fourth OSD slot in `OSDRenderer`, rendered when `ShaderHunter::IsActive()`.

Each chunk is one commit, independently reviewable, independently rollback-able.

## Risks + open questions for Akshay

1. **Draw-skip safety**: can we early-return from `Context11Proxy::Draw*` without corrupting the right-eye replay recording? The `ReplayFrameCommands(Right)` sweep expects the frame's draw list to match left-eye. If we skip a draw left-eye but not right-eye (or vice versa), replay desyncs.
   - **Proposal**: skip draws symmetrically — if `ShouldSkipDraw` returns true at `ApplyFeatures` entry, record a no-op closure instead of the real draw closure, so replay sees the same no-op on the right eye.
2. **CRC collisions**: wiz3D's CRC32 is 32-bit. Collisions across stages (same CRC on a VS and a PS, same CRC on different shader models) are rare but possible. Do we key by `(crc, stage)` tuple, or trust the 1-in-4-billion to not bite?
   - **Proposal**: key by `(crc, stage)`. Minor cost, correct behavior under collisions.
3. **Deferred contexts**: `DeferredContext11` records commands into a command list. ShaderHunter's `NoteDraw` on a deferred context would attribute the draw to the wrong frame. Should hunt mode skip deferred-context recording entirely (safe), or try to backfill when the command list is executed on the immediate context?
   - **Proposal**: skip deferred contexts for the hunt system. Draws through deferred contexts aren't huntable in 0.5.0; document as a known limitation.
4. **Thread safety of `OnCapture`**: the Numpad 3 handler needs to synchronously read the current hunted CRC's analyser state, which lives on `m_known`. Protect with the same mutex; capture is a 1-shot slow operation (file I/O) so lock contention isn't a real concern.

## Testing plan

1. Start with a known-stereo-working game (e.g., BioShock Infinite under the UE3 profile)
2. Verify hunt mode toggles cleanly with no visual regression when toggled off
3. Verify hiding a VS makes its draws disappear (visually)
4. Verify hiding a PS makes its draws disappear (visually)
5. Verify prev/next cycle through the full `m_cycle` and wrap
6. Verify Numpad 3 produces a valid report
7. Verify no replay desync on cycle while a game is active

## Out of scope for 0.5.0

- DX9 port (0.5.x — the DX9 shader-create path is parallel, mirrors mechanically)
- Multiple hidden CRCs at once (0.6.0 if asked for)
- Hunt-mode screenshot capture (press key, grab a frame of each pose) — nice-to-have

