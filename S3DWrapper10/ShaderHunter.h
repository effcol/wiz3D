// -----------------------------------------------------------------------------
// wiz3D ShaderHunter — DX11
//
// Interactive OSD mode for identifying which shader draws a specific visually-
// wrong effect, modelled on Helix/3DMigoto/Geo-11 shader hunting but keyed by
// wiz3D's own CRC32 and plugged into Akshay's PR#31 Feature engine.
//
// Public surface, called from four places:
//   - Device11Proxy::Create{Vertex,Pixel}Shader -> NoteCreated(crc, stage, info)
//   - Context11Proxy::ApplyFeatures entry       -> NoteDraw(vsCrc, psCrc)
//                                                  + ShouldSkipDraw(vsCrc, psCrc)
//   - SwapChain11Proxy::Present (pre-replay)    -> FrameEnd()
//   - KeyboardHook::OnEvent dispatch            -> OnToggleHunt / OnCyclePrev
//                                                  OnCycleNext / OnCapture
//
// Fast path when hunt mode is off: one relaxed atomic load (IsActive() returns
// false immediately). No locks, no map lookups, no file I/O.
//
// Thread safety: hotkey handlers run on the KeyboardHook thread; NoteDraw +
// ShouldSkipDraw run on the game thread (sometimes multiple game threads for
// deferred-context recording, though we treat deferred contexts as not-hunted
// per the design doc risk #3). All mutable state behind s_lock. The fast-path
// read of m_active uses a dedicated atomic so the common case stays lock-free.
//
// See docs/SHADER_HUNTER_DESIGN.md for the full plan + risks + Akshay-review
// notes. This header is the API contract that the design doc describes.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace wiz3d
{

struct ShaderAnalysis11Result;   // forward: ShaderAnalyzer11.h

enum class HuntStage : uint8_t
{
    VS = 0,
    PS = 1,
    _Count
};

class ShaderHunter
{
public:
    // Fast-path gate used by hot callers. Returns false when gInfo.EnableShaderHunter
    // is 0 OR when hunt mode has not been toggled on this session. One relaxed
    // atomic load — no lock. Use this to early-out before any other ShaderHunter
    // call on the game thread so the common case stays free.
    static bool IsActive();

    // Record a newly created shader + its analyser result. Called from
    // Device11Proxy::AnalyzeAndCreate after the Feature engine's OnShaderCreated
    // pass but before the shader is returned to the game. Does nothing when
    // IsActive() is false, so safe to always call.
    //
    // The analyser result is captured by value (shallow copy of the small POD
    // bits; bytecode blob kept by shared_ptr so no deep copy). It's the sole
    // source the capture-report path uses to populate the suggested
    // ShaderPatch skeleton.
    static void NoteCreated(uint32_t crc, HuntStage stage,
                            const ShaderAnalysis11Result& info);

    // Record that a draw was issued with these two bound shaders. Called from
    // Context11Proxy::ApplyFeatures entry. Both CRCs may be 0 if a stage is
    // unbound. Does nothing when IsActive() is false.
    static void NoteDraw(uint32_t vsCrc, uint32_t psCrc);

    // Returns true if the currently hunted CRC matches either bound shader.
    // Called from Context11Proxy::ApplyFeatures entry — if true the context
    // records a no-op closure instead of the real draw closure, both for the
    // left-eye immediate path and the right-eye replay sweep. Design doc risk
    // #1 — the no-op must be symmetric across eyes to keep replay in sync.
    //
    // Returns false immediately when IsActive() is false.
    static bool ShouldSkipDraw(uint32_t vsCrc, uint32_t psCrc);

    // Called from SwapChain11Proxy::Present before the right-eye replay. Takes
    // the drawn-this-frame CRCs, rebuilds the stable cycle ordering, resets
    // the per-frame counters. Does nothing when IsActive() is false.
    static void FrameEnd();

    // -------------------------------------------------------------------------
    // Hotkey handlers (KeyboardHook thread). All take the s_lock.
    // -------------------------------------------------------------------------

    // Numpad 0. Toggle hunt mode on/off. First activation also arms the fast-
    // path atomic. Deactivation clears m_hiddenCRC so no draws stay hidden.
    static void OnToggleHunt();

    // Numpad 1. Cycle m_hiddenCRC to the previous CRC in the current frame's cycle.
    // No-op if the cycle is empty (no draws seen yet) or hunt mode is off.
    static void OnCyclePrev();

    // Numpad 2. Mirror of OnCyclePrev going forward.
    static void OnCycleNext();

    // Numpad 3. Dump the currently hidden shader's bytecode disassembly + CB layout
    // + suggested ShaderPatch skeleton to:
    //     DX11ShaderDataCache/<Game>/Hunter/<CRC>_<timestamp>.txt
    // The <ts> timestamp is YYYYMMDD_HHMMSS so sorting by name sorts by time.
    // No-op if m_hiddenCRC is 0 or hunt mode is off.
    static void OnCapture();

    // -------------------------------------------------------------------------
    // OSD read API (SwapChain11Proxy / OSDRenderer thread). Takes the s_lock
    // only for the GetState snapshot; returns a POD copy so the OSD renderer
    // doesn't hold the lock across GDI.
    // -------------------------------------------------------------------------

    struct State
    {
        bool     active;         // hunt mode on
        uint32_t hiddenCRC;      // 0 if nothing hidden
        HuntStage hiddenStage;   // relevant only if hiddenCRC != 0
        uint32_t drawsForHidden; // draws observed for hiddenCRC this frame
        size_t   cycleIdx;       // 1-based position in cycle (0 if none)
        size_t   cycleSize;      // total CRCs drawn last frame
        // 0.4.1 save-toast: non-zero flashDeadline means an OSD "Shader
        // saved: <leaf>" toast should render bottom-right until the
        // deadline. flashLeaf carries the written file's basename so the
        // toast can tell the user what landed on disk.
        uint64_t flashDeadline;
        char     flashLeaf[128];
    };

    static State GetState();
};

}  // namespace wiz3d
