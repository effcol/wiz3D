// -----------------------------------------------------------------------------
// wiz3D ShaderHunter implementation. See ShaderHunter.h for the contract and
// docs/SHADER_HUNTER_DESIGN.md for the design + Akshay-review notes.
// -----------------------------------------------------------------------------
#include "StdAfx.h"
#include "ShaderHunter.h"
#include "ShaderAnalyzer11.h"
#include "AdapterFunctions.h"            // DDILog, for hunter traces into wiz3D_proxy.log

#include "..\S3DAPI\GlobalData.h"

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
CrcStageKey                                 s_hidden = 0;   // 0 => nothing hidden

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

// Build DX11ShaderDataCache/<Game>/Hunter/<CRC>_<ts>.txt. Returns false if
// the path can't be built or the dir can't be created. Caller passes the
// final filled TCHAR buffer back for fopen. Mirrors the DX9ShaderDataCache
// convention iZ3D used on DX9.
bool BuildHunterPath(uint32_t crc, HuntStage stage, TCHAR out[MAX_PATH])
{
    _tcscpy(out, gInfo.DriverDirectory);
    PathAppend(out, _T("DX11ShaderDataCache"));
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
    if (!PathIsOn()) return false;
    // Minimise lock scope: read s_hidden under lock, then decide outside. We
    // trust s_hidden to only be mutated under the same lock; a stale read
    // one draw before the hotkey toggle lands is a visible-but-safe artefact
    // (one extra frame of the "old" hidden shader) and isn't worth paying for.
    CrcStageKey hidden;
    {
        std::lock_guard<std::mutex> g(s_lock);
        hidden = s_hidden;
    }
    if (!hidden) return false;
    uint32_t hiddenCrc  = (uint32_t)(hidden >> 1);
    HuntStage hiddenStg = (HuntStage)(hidden & 1);
    const uint32_t cand = (hiddenStg == HuntStage::VS) ? vsCrc : psCrc;
    return cand == hiddenCrc;
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
    if (s_hidden)
    {
        auto it = std::lower_bound(s_cycle.begin(), s_cycle.end(), s_hidden);
        if (it != s_cycle.end() && *it == s_hidden)
            s_cycleIdx = (size_t)(it - s_cycle.begin());
        else
            s_cycleIdx = 0;
    }
    else
    {
        s_cycleIdx = 0;
    }

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
        s_hidden = 0;
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
    if (s_cycle.empty()) { DDILog("[hunt] prev: cycle empty (no draws observed this frame yet)\n"); return; }
    if (s_cycleIdx == 0) s_cycleIdx = s_cycle.size() - 1;
    else                 s_cycleIdx--;
    s_hidden = s_cycle[s_cycleIdx];
    DDILog("[hunt] prev -> CRC=0x%08X stage=%s (%zu/%zu)\n",
           (uint32_t)(s_hidden >> 1),
           ((s_hidden & 1) ? "PS" : "VS"),
           s_cycleIdx + 1, s_cycle.size());
}

void ShaderHunter::OnCycleNext()
{
    if (!PathIsOn()) { DDILog("[hunt] next: hunt mode is off (Numpad 0 to enter)\n"); return; }
    std::lock_guard<std::mutex> g(s_lock);
    if (s_cycle.empty()) { DDILog("[hunt] next: cycle empty (no draws observed this frame yet)\n"); return; }
    s_cycleIdx = (s_cycleIdx + 1) % s_cycle.size();
    s_hidden = s_cycle[s_cycleIdx];
    DDILog("[hunt] next -> CRC=0x%08X stage=%s (%zu/%zu)\n",
           (uint32_t)(s_hidden >> 1),
           ((s_hidden & 1) ? "PS" : "VS"),
           s_cycleIdx + 1, s_cycle.size());
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
        if (!s_hidden && !s_cycle.empty())
        {
            s_cycleIdx = 0;
            s_hidden   = s_cycle[0];
        }
        hidden = s_hidden;
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
    if (s_hidden)
    {
        out.hiddenCRC   = (uint32_t)(s_hidden >> 1);
        out.hiddenStage = (HuntStage)(s_hidden & 1);
        auto it = s_drawCountForFrame.find(s_hidden);
        out.drawsForHidden = (it == s_drawCountForFrame.end()) ? 0 : it->second;
        out.cycleIdx       = s_cycleIdx + 1;   // 1-based for the OSD
    }
    out.cycleSize = s_cycle.size();
    return out;
}

}  // namespace wiz3d
