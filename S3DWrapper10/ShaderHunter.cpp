// -----------------------------------------------------------------------------
// wiz3D ShaderHunter implementation. See ShaderHunter.h for the contract and
// docs/SHADER_HUNTER_DESIGN.md for the design + Akshay-review notes.
// -----------------------------------------------------------------------------
#include "StdAfx.h"
#include "ShaderHunter.h"
#include "ShaderAnalyzer11.h"
#include "AdapterFunctions.h"            // DDILog, for hunter traces into wiz3D_proxy.log

#include "..\S3DAPI\GlobalData.h"
#include <Shlwapi.h>     // PathRemoveFileSpec for the Numpad 4 save-path toast
#pragma comment(lib, "Shlwapi.lib")
#include "..\S3DAPI\ReadData.h"          // SaveMonoShaderCRCToUserProfile

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdio>
#include <ctime>
#include <shlwapi.h>
#include <tchar.h>
#include <d3dcompiler.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace wiz3d
{

// -----------------------------------------------------------------------------
// Internal state.
//
// The fast-path atomic g_active is a thin cache of "hunt mode is engaged AND
// gInfo.EnableShaderHunter was 1 at toggle time". NoteDraw / ShouldSkipDraw
// hit this on every single draw across every single frame, so it must not
// touch the mutex.
//
// Everything mutable other than the fast-path flag lives under s_lock.
// Contention is rare: hotkey handlers fire at most a few times per second;
// NoteDraw does one atomic load per draw and only locks when a shader could
// actually be hidden.
// -----------------------------------------------------------------------------
namespace
{

std::atomic<bool>   g_active{ false };

std::mutex          s_lock;

// Per-shader metadata: enough for the capture report to reconstruct what the
// Feature engine saw at creation time without going back to the shader store.
// Keep the bytecode blob as a shared_ptr so the capture path can disassemble
// it on demand without copying each shader into private storage.
struct HuntMeta
{
    HuntStage                                   stage = HuntStage::VS;
    std::shared_ptr<const std::vector<BYTE> >   bytecode;   // nullable
    ShaderAnalysis11Result                      info;       // small POD copy
};
// Keyed on (crc, stage) because 32-bit CRC collisions across stages are rare
// but not impossible (design doc risk #2).
using CrcStageKey = uint64_t;
inline CrcStageKey MakeKey(uint32_t crc, HuntStage stage)
{
    return (uint64_t(crc) << 1) | (uint64_t)stage;
}

std::unordered_map<CrcStageKey, HuntMeta>   s_known;        // all shaders seen so far
std::unordered_set<CrcStageKey>             s_drewThisFrame;// cleared at FrameEnd
std::unordered_map<CrcStageKey, uint32_t>   s_drawCountForFrame;
// 0.4.1 diagnostic: how many NoteDraw calls arrived this frame (regardless of
// whether vsCrc / psCrc were non-zero). A frame with notedDraws > 0 but a
// zero-sized s_drewThisFrame means BoundShaderCRC returned 0 for every draw —
// i.e. the bound shaders aren't registered in Device11Proxy's projection map.
// Printed once per hunt-mode frame-end.
unsigned                                    s_notedDrawsThisFrame = 0;

std::vector<CrcStageKey>                    s_cycle;        // stable order built at FrameEnd
size_t                                      s_cycleIdx = 0;

// 0.5.0: s_hidden promoted to atomic. The hot-path ShouldSkipDraw reads it on
// every draw across every eye on both the immediate and deferred contexts;
// hotkey writers (OnCyclePrev/Next/ToggleHunt/MonoCurrent) all go through
// the s_lock mutex AND update this atomic, so the hot path can skip the
// mutex. 64-bit atomic is lock-free on x64 and on x86 with cmpxchg8b.
std::atomic<uint64_t>                       s_hidden{ 0 };  // 0 => nothing hidden

// 0.5.0 cycle-flash: ShouldSkipDraw returns true for the first half of each
// 30-frame window, so the hunted shader visibly flashes on/off at ~2 Hz. Lets
// the user instantly see WHERE a cycled shader draws. FrameEnd increments it
// once per Present. Reset to 0 on hunt-mode toggle so the flash restarts in
// the "hidden" half (first visual response to Numpad 1/2 is "it disappeared").
std::atomic<uint32_t>                       s_flashFrame{ 0 };
constexpr uint32_t                          kFlashPeriod = 30;   // frames
constexpr uint32_t                          kFlashHidden = 15;   // frames hidden per period

// 0.5.0 <When TextureCRC> capture — fingerprint of the currently-cycled
// draw-in-draw-mode, populated by Context11Proxy via StoreCurrentDrawFingerprint
// when ShouldCaptureFingerprint() returns true. OnCapture prints the hashes
// into the Hunter capture report so the user can paste them into a <When/>
// sub-filter.
constexpr unsigned                          kMaxFingerprintHashes = 16;
uint32_t                                    s_fpHashes[kMaxFingerprintHashes] = {};
unsigned                                    s_fpCount = 0;

// 0.5.0 Numpad 5 hunt-granularity: in CRC mode (default), ShouldSkipDraw flashes
// every draw of the selected CRC. In Draw mode, only ONE specific draw-of-this-
// CRC flashes — s_drawCycleIdx is the 1-based target. Per-draw NoteDraw bumps
// s_currentDrawIdx (lock-free atomic) each time the selected CRC is bound, so
// ShouldSkipDraw can compare "which draw-of-CRC am I" against the target. Frame-
// End snapshots the final count into s_drawCycleSize so Numpad 1/2 can wrap
// against the real count from the previous frame.
enum class HuntGranularity : uint8_t { CRC = 0, Draw = 1 };
std::atomic<uint8_t>                        s_granularity{ (uint8_t)HuntGranularity::CRC };
std::atomic<uint32_t>                       s_currentDrawIdx{ 0 };   // reset per frame
size_t                                      s_drawCycleIdx = 1;      // 1-based; 0 means "not selected yet"
size_t                                      s_drawCycleSize = 0;     // snapshot of previous frame's draw count

// 0.4.1 save-toast: OnCapture stamps these on success so the OSD can show
// a 2 s "Shader saved: <leaf>" toast in the bottom-right. The deadline is
// GetTickCount64() + flashMs; OSDRenderer polls State.flashDeadline every
// frame and renders the toast while now < deadline. s_flashLeaf is the
// written file's basename (no path), ASCII-only — the Hunter path builder
// writes ASCII filenames (CRC + stage + timestamp) so no wide conversion
// is needed on the OSD side.
uint64_t                                    s_flashDeadline = 0;
char                                        s_flashLeaf[128] = {};

bool PathIsOn() { return g_active.load(std::memory_order_relaxed); }

// Build wiz3D_Shaders/<Game>/Hunter/<CRC>_<ts>.txt. Returns false if
// the path can't be built or the dir can't be created. Caller passes
// the final filled TCHAR buffer back for fopen. Shared folder across
// every stereo-injector wrapper (DX7/8/9/10-11).
bool BuildHunterPath(uint32_t crc, HuntStage stage, TCHAR out[MAX_PATH])
{
    _tcscpy(out, gInfo.DriverDirectory);
    PathAppend(out, _T("wiz3D_Shaders"));
    CreateDirectory(out, 0);
    {
        TCHAR game[MAX_PATH];
        _tcscpy(game, gInfo.ApplicationName);
        TCHAR* dot = _tcsrchr(game, _T('.'));
        if (dot) *dot = 0;
        PathAppend(out, game);
    }
    CreateDirectory(out, 0);
    PathAppend(out, _T("Hunter"));
    CreateDirectory(out, 0);

    SYSTEMTIME now;
    GetLocalTime(&now);
    TCHAR leaf[64];
    _sntprintf_s(leaf, _countof(leaf), _TRUNCATE,
                 _T("0x%08X_%s_%04u%02u%02u_%02u%02u%02u.txt"),
                 crc,
                 (stage == HuntStage::VS ? _T("VS") : _T("PS")),
                 now.wYear, now.wMonth, now.wDay,
                 now.wHour, now.wMinute, now.wSecond);
    PathAppend(out, leaf);
    return true;
}

}  // anon namespace

// =============================================================================
// Public API
// =============================================================================

bool ShaderHunter::IsActive()
{
    return PathIsOn();
}

void ShaderHunter::NoteCreated(uint32_t crc, HuntStage stage,
                               const ShaderAnalysis11Result& info)
{
    // We take the lock unconditionally here because this is only ever hit from
    // the game thread at shader-create time — not from the per-draw hot path.
    // Shader creates are rare relative to draws (hundreds-to-thousands per
    // game, not thousands per frame) so the lock cost is irrelevant.
    //
    // We DO still populate s_known when the fast-path is off, so that flipping
    // hunt mode on at any point sees the right set of shaders without having
    // to walk the game to re-create them. A fresh hunt-mode toggle on an
    // already-running game is one of the authoring scenarios we want to work.
    if (!crc) return;
    std::lock_guard<std::mutex> g(s_lock);
    HuntMeta m;
    m.stage    = stage;
    m.bytecode = info.bytecode;    // shared_ptr copy, cheap
    m.info     = info;              // small POD; bytecode re-shared, no deep copy
    s_known[MakeKey(crc, stage)] = m;
}

void ShaderHunter::NoteDraw(uint32_t vsCrc, uint32_t psCrc)
{
    if (!PathIsOn()) return;
    // 0.5.0 Numpad 5: bump the per-draw counter if the bound VS or PS matches
    // the selected CRC. Lock-free atomic; the subsequent ShouldSkipDraw call
    // in the same draw-call reads this to decide if THIS draw is the one to
    // flash in Draw-granularity mode. Writes happen under the main lock below.
    const uint64_t hidden = s_hidden.load(std::memory_order_relaxed);
    if (hidden)
    {
        const uint32_t crc = (uint32_t)(hidden >> 1);
        const bool isVS = ((hidden & 1) == 0);
        const uint32_t boundCrc = isVS ? vsCrc : psCrc;
        if (boundCrc == crc)
            s_currentDrawIdx.fetch_add(1, std::memory_order_acq_rel);
    }

    // We touch the lock here only when the hunt is active. The per-frame set
    // can be big (hundreds of distinct CRCs on a complex game), but the two
    // insert calls amortise to ~constant time against the hashmap.
    std::lock_guard<std::mutex> g(s_lock);
    ++s_notedDrawsThisFrame;
    if (vsCrc)
    {
        auto k = MakeKey(vsCrc, HuntStage::VS);
        s_drewThisFrame.insert(k);
        s_drawCountForFrame[k]++;
    }
    if (psCrc)
    {
        auto k = MakeKey(psCrc, HuntStage::PS);
        s_drewThisFrame.insert(k);
        s_drawCountForFrame[k]++;
    }
}

bool ShaderHunter::ShouldSkipDraw(uint32_t vsCrc, uint32_t psCrc)
{
    // 0.5.0 cycle-flash: the hunted shader visibly flashes on/off at ~2 Hz
    // (15 frames hidden, 15 visible per 30-frame window) so the user can
    // locate it on-screen while cycling with Numpad 1/2. Unity risk: Lara
    // Croft GO's Mono.dll JIT null-deref'd after a hidden shader was dropped
    // for the whole hunt session; flashing (not continuous hide) + state
    // recovery on hunt-exit (s_hidden cleared in OnToggleHunt) keeps the
    // output present for ~half the time, which has been enough in practice.
    // Permanent hide stays off by design — use Numpad 4 to persist a mono
    // via <SkipStereo/>, which is engine-safe.
    const uint64_t hidden = s_hidden.load(std::memory_order_relaxed);
    if (!hidden) return false;
    const uint32_t crc = (uint32_t)(hidden >> 1);
    const bool isVS = ((hidden & 1) == 0);
    const uint32_t boundCrc = isVS ? vsCrc : psCrc;
    if (boundCrc != crc) return false;

    // 0.5.0 Numpad 5: in Draw granularity, only flash the specific draw-of-
    // CRC that matches s_drawCycleIdx. NoteDraw bumped s_currentDrawIdx just
    // before this call, so its current value == this draw's 1-based index.
    const auto gran = (HuntGranularity)s_granularity.load(std::memory_order_relaxed);
    if (gran == HuntGranularity::Draw)
    {
        const uint32_t curIdx = s_currentDrawIdx.load(std::memory_order_relaxed);
        // s_drawCycleIdx is 1-based; 0 means not-yet-selected -> no flash.
        if (s_drawCycleIdx == 0 || curIdx != (uint32_t)s_drawCycleIdx) return false;
    }

    const uint32_t f = s_flashFrame.load(std::memory_order_relaxed);
    return (f % kFlashPeriod) < kFlashHidden;
}

void ShaderHunter::FrameEnd()
{
    if (!PathIsOn()) return;
    std::lock_guard<std::mutex> g(s_lock);

    // Rebuild the cycle from the set of CRCs actually drawn this frame. Sort
    // by CRC so prev/next produces a deterministic walk even across games
    // where draw order shuffles frame-to-frame (deferred rendering, dynamic
    // sort by material).
    s_cycle.assign(s_drewThisFrame.begin(), s_drewThisFrame.end());
    std::sort(s_cycle.begin(), s_cycle.end());

    // Preserve cycle position: if the previously hidden CRC still drew this
    // frame, point cycleIdx at its new index. Otherwise reset to 0 so the
    // user doesn't lose their place.
    const CrcStageKey hiddenNow = s_hidden.load(std::memory_order_relaxed);
    if (hiddenNow)
    {
        auto it = std::lower_bound(s_cycle.begin(), s_cycle.end(), hiddenNow);
        if (it != s_cycle.end() && *it == hiddenNow)
            s_cycleIdx = (size_t)(it - s_cycle.begin());
        else
            s_cycleIdx = 0;
    }
    else
    {
        s_cycleIdx = 0;
    }

    // 0.5.0: advance the flash counter so ShouldSkipDraw alternates the
    // hunted shader on/off at ~2 Hz. This is the whole hot-path write for
    // the flash — no lock in ShouldSkipDraw.
    s_flashFrame.fetch_add(1, std::memory_order_relaxed);

    // 0.5.0 Numpad 5: snapshot this frame's draw-of-selected-CRC count,
    // reset the per-draw counter for the next frame. Even in CRC mode
    // (where draw-level cycling isn't active) the snapshot stays useful
    // for OSD display and lets Numpad 5 immediately present a usable
    // draw count on first toggle.
    s_drawCycleSize = (size_t)s_currentDrawIdx.exchange(0, std::memory_order_acq_rel);
    // Clamp cycle idx to the new size — on cycle-change / toggle it could
    // overshoot an element count that shrank. If the selected CRC drew 0
    // times this frame leave the previous idx alone so the next frame can
    // restore it (transiently empty frames don't lose the user's place).
    if (s_drawCycleSize && s_drawCycleIdx > s_drawCycleSize) s_drawCycleIdx = s_drawCycleSize;

    // Diagnostic once per frame while hunt mode is on. A notedDraws > 0 with
    // drewUnique == 0 means BoundShaderCRC returned 0 for every draw this
    // frame — the shaders aren't registered in Device11Proxy's projection
    // map, so the hunter can't name them. A notedDraws == 0 means our Draw()
    // override isn't on the hot path (deferred-context draws bypass it).
    DDILog("[hunt] frame-end: notedDraws=%u drewUnique=%zu cycleSize=%zu\n",
           s_notedDrawsThisFrame, s_drewThisFrame.size(), s_cycle.size());
    s_notedDrawsThisFrame = 0;
    s_drewThisFrame.clear();
    s_drawCountForFrame.clear();
}

void ShaderHunter::OnToggleHunt()
{
    // The hunter is always wired. Numpad 0 is itself the opt-in; no config
    // knob gates this. Zero overhead while hunt mode is off: NoteDraw /
    // ShouldSkipDraw early-return on the atomic g_active flag.
    std::lock_guard<std::mutex> g(s_lock);
    const bool next = !g_active.load(std::memory_order_relaxed);
    g_active.store(next, std::memory_order_release);
    if (!next)
    {
        // Leaving hunt mode — restore all draws (clear the hidden CRC).
        // Important: Unity Mono.dll JIT crashes if an output was dropped
        // and never recovered, so this reset-on-exit is the primary safety
        // for the cycle-flash path.
        s_hidden.store(0, std::memory_order_release);
    }
    else
    {
        // Entering hunt mode — restart the flash counter so the first press
        // of Numpad 1/2 starts in the "hidden" half (visible first response).
        s_flashFrame.store(0, std::memory_order_relaxed);
    }
    DDILog("[hunt] %s\n", next ? "ENTER" : "EXIT");
}

void ShaderHunter::OnCyclePrev()
{
    // Always log the outcome so a user who sees "nothing happens" when
    // pressing Numpad 1 can tell whether (a) hunt mode was off, (b) the key
    // never reached our hook, or (c) the cycle was transiently empty
    // between Present-POST frames. Pre-0.4.1 these early-returns were
    // silent and made the "Numpad 1 does nothing" complaint undiagnosable.
    if (!PathIsOn()) { DDILog("[hunt] prev: hunt mode is off (Numpad 0 to enter)\n"); return; }
    std::lock_guard<std::mutex> g(s_lock);
    const auto gran = (HuntGranularity)s_granularity.load(std::memory_order_relaxed);
    if (gran == HuntGranularity::Draw)
    {
        // Cycle backwards through draw indices of the selected CRC.
        if (!s_drawCycleSize) { DDILog("[hunt] prev: no draws of selected CRC this frame\n"); return; }
        if (s_drawCycleIdx <= 1) s_drawCycleIdx = s_drawCycleSize;
        else                     s_drawCycleIdx--;
        s_flashFrame.store(0, std::memory_order_relaxed);
        DDILog("[hunt] prev -> draw %zu/%zu of CRC=0x%08X\n",
               s_drawCycleIdx, s_drawCycleSize,
               (uint32_t)(s_hidden.load(std::memory_order_relaxed) >> 1));
        return;
    }
    if (s_cycle.empty()) { DDILog("[hunt] prev: cycle empty (no draws observed this frame yet)\n"); return; }
    if (s_cycleIdx == 0) s_cycleIdx = s_cycle.size() - 1;
    else                 s_cycleIdx--;
    const CrcStageKey k = s_cycle[s_cycleIdx];
    s_hidden.store(k, std::memory_order_release);
    s_flashFrame.store(0, std::memory_order_relaxed);   // restart flash in hidden half
    s_drawCycleIdx = 1;                                 // reset draw selector when CRC changes
    DDILog("[hunt] prev -> CRC=0x%08X stage=%s (%zu/%zu)\n",
           (uint32_t)(k >> 1),
           ((k & 1) ? "PS" : "VS"),
           s_cycleIdx + 1, s_cycle.size());
}

void ShaderHunter::OnCycleNext()
{
    if (!PathIsOn()) { DDILog("[hunt] next: hunt mode is off (Numpad 0 to enter)\n"); return; }
    std::lock_guard<std::mutex> g(s_lock);
    const auto gran = (HuntGranularity)s_granularity.load(std::memory_order_relaxed);
    if (gran == HuntGranularity::Draw)
    {
        if (!s_drawCycleSize) { DDILog("[hunt] next: no draws of selected CRC this frame\n"); return; }
        if (s_drawCycleIdx == 0 || s_drawCycleIdx >= s_drawCycleSize) s_drawCycleIdx = 1;
        else                                                           s_drawCycleIdx++;
        s_flashFrame.store(0, std::memory_order_relaxed);
        DDILog("[hunt] next -> draw %zu/%zu of CRC=0x%08X\n",
               s_drawCycleIdx, s_drawCycleSize,
               (uint32_t)(s_hidden.load(std::memory_order_relaxed) >> 1));
        return;
    }
    if (s_cycle.empty()) { DDILog("[hunt] next: cycle empty (no draws observed this frame yet)\n"); return; }
    s_cycleIdx = (s_cycleIdx + 1) % s_cycle.size();
    const CrcStageKey k = s_cycle[s_cycleIdx];
    s_hidden.store(k, std::memory_order_release);
    s_flashFrame.store(0, std::memory_order_relaxed);   // restart flash in hidden half
    s_drawCycleIdx = 1;                                 // reset draw selector when CRC changes
    DDILog("[hunt] next -> CRC=0x%08X stage=%s (%zu/%zu)\n",
           (uint32_t)(k >> 1),
           ((k & 1) ? "PS" : "VS"),
           s_cycleIdx + 1, s_cycle.size());
}

bool ShaderHunter::ShouldCaptureFingerprint()
{
    if (!PathIsOn()) return false;
    const auto gran = (HuntGranularity)s_granularity.load(std::memory_order_relaxed);
    if (gran != HuntGranularity::Draw) return false;
    // Trigger on the currently-cycled draw's index: the current per-draw
    // counter was just bumped in NoteDraw, so its value = this draw's 1-based
    // index. Match against s_drawCycleIdx (also 1-based).
    const uint32_t curIdx = s_currentDrawIdx.load(std::memory_order_relaxed);
    return s_drawCycleIdx != 0 && curIdx == (uint32_t)s_drawCycleIdx;
}

void ShaderHunter::StoreCurrentDrawFingerprint(const uint32_t* hashes, unsigned count)
{
    std::lock_guard<std::mutex> g(s_lock);
    s_fpCount = count < kMaxFingerprintHashes ? count : kMaxFingerprintHashes;
    for (unsigned i = 0; i < s_fpCount; ++i) s_fpHashes[i] = hashes[i];
}

void ShaderHunter::OnToggleGranularity()
{
    if (!PathIsOn()) { DDILog("[hunt] granularity: hunt mode is off (Ctrl+Numpad 0 to enter)\n"); return; }
    std::lock_guard<std::mutex> g(s_lock);
    const auto cur = (HuntGranularity)s_granularity.load(std::memory_order_relaxed);
    const auto next = (cur == HuntGranularity::CRC) ? HuntGranularity::Draw : HuntGranularity::CRC;
    s_granularity.store((uint8_t)next, std::memory_order_release);
    if (next == HuntGranularity::Draw && s_drawCycleIdx == 0 && s_drawCycleSize) s_drawCycleIdx = 1;
    s_flashFrame.store(0, std::memory_order_relaxed);
    DDILog("[hunt] granularity -> %s (draws of selected CRC last frame: %zu)\n",
           next == HuntGranularity::CRC ? "CRC" : "Draw", s_drawCycleSize);
}

void ShaderHunter::OnMonoCurrent()
{
    if (!PathIsOn()) { DDILog("[hunt] mono: hunt mode is off (Ctrl+Numpad 0 to enter)\n"); return; }
    CrcStageKey hidden;
    {
        std::lock_guard<std::mutex> g(s_lock);
        // Match the capture ergonomics: auto-pick first-in-cycle if the user
        // hits Numpad 4 before cycling with Numpad 1 / 2.
        CrcStageKey cur = s_hidden.load(std::memory_order_relaxed);
        if (!cur && !s_cycle.empty())
        {
            s_cycleIdx = 0;
            cur = s_cycle[0];
            s_hidden.store(cur, std::memory_order_release);
        }
        hidden = cur;
    }
    if (!hidden) { DDILog("[hunt] mono: nothing hidden and no draws seen yet\n"); return; }

    const uint32_t crc = (uint32_t)(hidden >> 1);
    const HuntStage stage = (HuntStage)(hidden & 1);
    const int stageInt = (stage == HuntStage::VS) ? 0 : 1;

    // 0.5.0 toggle semantics: if this CRC is already monoed in the live
    // profile, remove it; otherwise add it. Second press of Numpad 4 on the
    // same CRC un-monos it, so the user can audition fixes without reload.
    bool removed = false;
    const bool wasMonoed = IsMonoShaderCRCInUserProfile((DWORD)crc, stageInt);
    if (wasMonoed)
    {
        removed = RemoveMonoShaderCRCFromUserProfile((DWORD)crc, stageInt);
    }
    const bool added = !wasMonoed && SaveMonoShaderCRCToUserProfile((DWORD)crc, stageInt);
    DDILog("[hunt] mono CRC=0x%08X stage=%s %s\n",
           crc, (stage == HuntStage::VS ? "VS" : "PS"),
           wasMonoed ? (removed ? "removed" : "remove-failed")
                     : (added   ? "added"   : "add-skipped-already-present"));

    // 0.4.3: log the full save-path so users don't have to hunt for
    // UserProfile.xml. The toast shows the leaf path so it fits the OSD
    // width; the proxy log carries the full path for troubleshooting.
    TCHAR exeDirT[MAX_PATH] = {};
    _tcscpy_s<MAX_PATH>(exeDirT, gInfo.ApplicationFileName);
    PathRemoveFileSpec(exeDirT);
    char exeDirA[MAX_PATH];
#ifdef UNICODE
    WideCharToMultiByte(CP_UTF8, 0, exeDirT, -1, exeDirA, MAX_PATH, nullptr, nullptr);
#else
    strncpy_s(exeDirA, exeDirT, _TRUNCATE);
#endif
    DDILog("[hunt] mono -> %s\\UserProfile.xml\n", exeDirA);

    // Toast. Distinct wording from the "Shader saved" capture toast so the
    // user can tell which hotkey landed. Line 2 of the toast gets a short
    // save-path hint so first-time users don't go looking in %AppData%.
    char toast[192];
    _snprintf_s(toast, _TRUNCATE,
                "%s 0x%08X %s -> UserProfile.xml (next to game exe)",
                wasMonoed ? "Un-mono" : "Mono",
                crc, (stage == HuntStage::VS ? "VS" : "PS"));
    std::lock_guard<std::mutex> g(s_lock);
    s_flashDeadline = (uint64_t)GetTickCount64() + 2500;
    strncpy_s(s_flashLeaf, toast, _TRUNCATE);
}

void ShaderHunter::OnCapture()
{
    if (!PathIsOn()) return;
    CrcStageKey  hidden;
    HuntMeta     meta;
    uint32_t     drawsSeen;
    {
        std::lock_guard<std::mutex> g(s_lock);
        // 0.4.1 ergonomics: if the user enters hunt mode and presses Numpad 3
        // before cycling (Numpad 1 / Numpad 2) to pick a shader, auto-select the
        // first shader in the current cycle so the capture still fires.
        // Previously this was a silent no-op that left Shaders/Hunter/ empty
        // and gave the user no feedback — hard to tell from "nothing happened"
        // whether the hotkey, the file I/O, or the hunter itself was broken.
        CrcStageKey cur = s_hidden.load(std::memory_order_relaxed);
        if (!cur && !s_cycle.empty())
        {
            s_cycleIdx = 0;
            cur = s_cycle[0];
            s_hidden.store(cur, std::memory_order_release);
        }
        hidden = cur;
        if (!hidden)
        {
            DDILog("[hunt] capture: nothing hidden and no draws seen yet\n");
            return;
        }
        auto it = s_known.find(hidden);
        if (it == s_known.end())
        {
            DDILog("[hunt] capture: no metadata for CRC=0x%08X stage=%s\n",
                   (uint32_t)(hidden >> 1),
                   ((hidden & 1) ? "PS" : "VS"));
            return;
        }
        meta       = it->second;    // copy — info.bytecode is shared_ptr so cheap
        auto dit   = s_drawCountForFrame.find(hidden);
        drawsSeen  = (dit == s_drawCountForFrame.end()) ? 0 : dit->second;
    }

    uint32_t  crc    = (uint32_t)(hidden >> 1);
    HuntStage stage  = (HuntStage)(hidden & 1);

    TCHAR path[MAX_PATH];
    if (!BuildHunterPath(crc, stage, path))
    {
        DDILog("[hunt] capture: could not build output path\n");
        return;
    }

    FILE* fp = _tfopen(path, _T("w"));
    if (!fp)
    {
        DDILog("[hunt] capture: fopen failed for %ls\n", path);
        return;
    }

    // ------------- Header -------------
    fprintf(fp, "Shader CRC:   0x%08X\n", crc);
    fprintf(fp, "Stage:        %s\n", (stage == HuntStage::VS ? "VS" : "PS"));
    fprintf(fp, "Draws seen:   %u (this frame)\n", drawsSeen);

    // ------------- Feature-engine summary -------------
    // The analyser result captured at create time is a compact representation
    // of what Akshay's PR#31 code learned about this shader. The capture
    // report leans on it for the "suggested ShaderPatch skeleton" section.
    const auto& info = meta.info;
    if (info.parsed)
    {
        // SV_Position is marked by sysValue == 1 (D3D_NAME_POSITION) in the OSGN chunk.
        unsigned posReg = (unsigned)-1;
        for (const auto& o : info.outputs)
            if (o.sysValue == 1) { posReg = o.reg; break; }
        if (posReg != (unsigned)-1)
            fprintf(fp, "Signature:    SV_Position reg=o%u, %zu outputs total\n",
                    posReg, info.outputs.size());
        else
            fprintf(fp, "Signature:    (no SV_Position in outputs — %zu outputs)\n",
                    info.outputs.size());
        if (!info.projection.matrixData.cb.empty())
        {
            fprintf(fp, "Matrix CBs:\n");
            for (const auto& cb : info.projection.matrixData.cb)
            {
                fprintf(fp, "  cb%u:", (unsigned)cb.first);
                for (const auto& md : cb.second)
                {
                    fprintf(fp, " r%u%s%s",
                            (unsigned)md.matrixRegister,
                            md.matrixIsTransposed ? "T" : "",
                            md.incorrectProjection ? "!" : "");
                }
                fprintf(fp, "\n");
            }
        }
        else
        {
            fprintf(fp, "Matrix CBs:   none detected\n");
        }
    }
    else
    {
        fprintf(fp, "Analyser:     not parsed (unusual for a bound shader)\n");
    }
    fprintf(fp, "\n");

    // ------------- D3DDisassemble -------------
    if (meta.bytecode && !meta.bytecode->empty())
    {
        ID3DBlob* pDisasm = nullptr;
        HRESULT hr = D3DDisassemble(meta.bytecode->data(), meta.bytecode->size(),
                                    D3D_DISASM_ENABLE_DEFAULT_VALUE_PRINTS,
                                    nullptr, &pDisasm);
        if (SUCCEEDED(hr) && pDisasm)
        {
            fprintf(fp, "---- D3DDisassemble ----\n");
            fprintf(fp, "%s\n", (const char*)pDisasm->GetBufferPointer());
            pDisasm->Release();
        }
        else
        {
            fprintf(fp, "D3DDisassemble failed hr=0x%08X\n", (unsigned)hr);
        }
    }
    else
    {
        fprintf(fp, "No bytecode retained (shader-keep budget pruned it; raise the\n"
                    "keep budget in Device11Proxy::AnalyzeAndCreate if this shader\n"
                    "needs to be inspected).\n");
    }

    // ------------- Suggested authoring entries -------------
    // Two paths available on DX11, both CRC-keyed in BaseProfile.xml:
    //   1) <ShaderPatch> shorthand with <SkipStereo/>  — Akshay PR#31
    //   2) iZ3D-native <Mono> / <Multiplier> / <Matrices> — pre-dates PR#31
    // The DX11 <ShaderPatch> loader currently consumes <SkipStereo/> only;
    // <PatchCB>/<ScaleCB>/<OverrideFeature> are 0.5.0-planned, so this
    // skeleton deliberately doesn't emit them (would be a silent no-op).
    fprintf(fp, "\n---- Authoring entry (A): declarative <ShaderPatch> ----\n");
    fprintf(fp, "<ShaderPatch CRC=\"0x%08X\" Stage=\"%s\" Note=\"...\">\n",
            crc, (stage == HuntStage::VS ? "VS" : "PS"));
    fprintf(fp, "    <SkipStereo/>  <!-- force mono-both-eyes for this draw -->\n");
    fprintf(fp, "</ShaderPatch>\n");

    fprintf(fp, "\n---- Authoring entry (B): iZ3D-native (equivalent for mono) ----\n");
    fprintf(fp, "<Mono><%sShader CRC=\"0x%08X\"/></Mono>\n",
            (stage == HuntStage::VS ? "Vertex" : "Pixel"), crc);
    if (info.parsed && !info.projection.matrixData.cb.empty())
    {
        fprintf(fp, "\n---- Authoring entry (C): projection matrix hints ----\n");
        fprintf(fp, "<!-- Analyser flagged these matrix locations. If the\n"
                    "     symptom is a mis-stereo projection (world geometry\n"
                    "     doesn't converge), use <Matrices> to describe each\n"
                    "     one; see docs/SHADER_FIXES.md for attribute reference. -->\n");
        for (const auto& cb : info.projection.matrixData.cb)
        {
            for (const auto& md : cb.second)
            {
                fprintf(fp,
                    "<!-- %s matrix in cb%u at r%u -->\n",
                    md.incorrectProjection ? "flagged" : "detected",
                    (unsigned)cb.first, (unsigned)md.matrixRegister);
            }
        }
    }

    // 0.5.0 <When TextureCRC> fingerprint — present when capture fired in
    // Draw-granularity mode. The hashes are PS resource-pointer FNV-1a32s
    // captured at the exact matched draw; paste them into a <When/> sub-
    // filter to target just that draw in a <ShaderPatch> entry.
    unsigned fpN = 0;
    uint32_t fpH[kMaxFingerprintHashes];
    {
        std::lock_guard<std::mutex> g(s_lock);
        fpN = s_fpCount;
        for (unsigned i = 0; i < fpN; ++i) fpH[i] = s_fpHashes[i];
    }
    if (fpN > 0)
    {
        fprintf(fp, "\n---- Authoring entry (D): sub-draw <When/> filter (from Draw-mode capture) ----\n");
        fprintf(fp, "<!-- PS resource-pointer hashes bound at the matched draw.\n"
                    "     Paste one or more <When TextureCRC=.../> into the <ShaderPatch>\n"
                    "     above to apply the primitive ONLY when that texture is bound.\n"
                    "     Session-local hashes — re-capture if you restart the game. -->\n");
        for (unsigned i = 0; i < fpN; ++i)
            fprintf(fp, "<When TextureCRC=\"0x%08X\"/>   <!-- PS slot %u -->\n", fpH[i], i);
    }

    fclose(fp);
    DDILog("[hunt] captured CRC=0x%08X stage=%s to %ls\n",
           crc, (stage == HuntStage::VS ? "VS" : "PS"), path);

    // Light up the OSD save-toast. 2 s matches the status popup's fade time
    // (NoteSettingChanged) so the two feel consistent. Updating the flash
    // overwrites any still-running pulse — the user cares about the newest
    // capture, not the one from 1.9 seconds ago.
    //
    // Extract just the leaf (basename) for the toast. Hunter files are
    // written with ASCII names "0x<CRC>_<stage>_YYYYMMDD_HHMMSS.txt", so a
    // best-effort TCHAR -> char conversion of the leaf is lossless.
    {
        const TCHAR* leaf = _tcsrchr(path, _T('\\'));
        leaf = leaf ? leaf + 1 : path;
        char leafA[128] = {};
#ifdef _UNICODE
        WideCharToMultiByte(CP_ACP, 0, leaf, -1, leafA, (int)sizeof(leafA) - 1, nullptr, nullptr);
#else
        strncpy_s(leafA, leaf, _TRUNCATE);
#endif
        std::lock_guard<std::mutex> g(s_lock);
        s_flashDeadline = (uint64_t)GetTickCount64() + 2000;
        strncpy_s(s_flashLeaf, leafA, _TRUNCATE);
    }
}

ShaderHunter::State ShaderHunter::GetState()
{
    State out = {};
    // Lock up-front: the save-toast fields need to be readable even when
    // hunt mode is off, so the toast keeps rendering if the user exits
    // hunt mode in the 2 s window after pressing Numpad 3.
    std::lock_guard<std::mutex> g(s_lock);
    if (s_flashDeadline && (uint64_t)GetTickCount64() < s_flashDeadline)
    {
        out.flashDeadline = s_flashDeadline;
        strncpy_s(out.flashLeaf, s_flashLeaf, _TRUNCATE);
    }
    if (!PathIsOn()) return out;
    out.active     = true;
    const CrcStageKey hidden = s_hidden.load(std::memory_order_relaxed);
    if (hidden)
    {
        out.hiddenCRC   = (uint32_t)(hidden >> 1);
        out.hiddenStage = (HuntStage)(hidden & 1);
        auto it = s_drawCountForFrame.find(hidden);
        out.drawsForHidden = (it == s_drawCountForFrame.end()) ? 0 : it->second;
        out.cycleIdx       = s_cycleIdx + 1;   // 1-based for the OSD
    }
    out.cycleSize = s_cycle.size();
    // 0.5.0 Numpad 5 granularity + draw-cycle state (OSD displays "draw X/Y" line).
    out.drawGranularity = ((HuntGranularity)s_granularity.load(std::memory_order_relaxed)
                            == HuntGranularity::Draw);
    out.drawCycleIdx    = s_drawCycleIdx;
    out.drawCycleSize   = s_drawCycleSize;
    return out;
}

}  // namespace wiz3d
