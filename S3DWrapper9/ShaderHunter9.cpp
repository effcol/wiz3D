// -----------------------------------------------------------------------------
// wiz3D ShaderHunter DX9 implementation. See header for the contract.
// -----------------------------------------------------------------------------
#include "StdAfx.h"
#include "ShaderHunter9.h"

#include <d3d9.h>
#include <d3dx9.h>
#include <atlbase.h>
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

#include "../S3DAPI/GlobalData.h"
#include "../S3DAPI/ReadData.h"
#include "../S3DAPI/ShaderProfileData.h"
#include "../CommonUtils/System.h"

#pragma comment(lib, "shlwapi.lib")

namespace wiz3d
{
namespace dx9
{

uint32_t HunterComputeCRC32(const void* bytecode, uint32_t sizeBytes)
{
    if (!bytecode || sizeBytes == 0) return 0;
    return (uint32_t)CalculateCRC32(bytecode, sizeBytes);
}

namespace
{

std::atomic<bool>   g_active{ false };
std::mutex          s_lock;

struct HuntMeta
{
    HuntStage                             stage = HuntStage::VS;
    std::shared_ptr<std::vector<BYTE>>    bytecode;
};
using CrcStageKey = uint64_t;
inline CrcStageKey MakeKey(uint32_t crc, HuntStage s) { return (uint64_t(crc) << 1) | (uint64_t)s; }

std::unordered_map<CrcStageKey, HuntMeta>   s_known;
std::unordered_set<CrcStageKey>             s_drewThisFrame;
std::unordered_map<CrcStageKey, uint32_t>   s_drawCountForFrame;
std::vector<CrcStageKey>                    s_cycle;
size_t                                      s_cycleIdx = 0;
CrcStageKey                                 s_hidden   = 0;

// 0.4.1 save-toast. See S3DWrapper10/ShaderHunter.cpp for the design.
uint64_t                                    s_flashDeadline  = 0;
char                                        s_flashLeaf[128] = {};

bool PathIsOn() { return g_active.load(std::memory_order_relaxed); }

// Diagnostic log appender — writes to the game folder's wiz3D_proxy.log
// via the same path convention S3DWrapper10 uses, so the DX9 hunter's
// traces land in the same file an author would read.
void HLog(const char* fmt, ...)
{
    static FILE* fp = nullptr;
    if (!fp)
    {
        WCHAR dir[MAX_PATH];
        GetModuleFileNameW(NULL, dir, MAX_PATH);
        WCHAR* pSlash = wcsrchr(dir, L'\\');
        if (pSlash) *(pSlash + 1) = L'\0';
        lstrcatW(dir, L"wiz3D_Logs");
        CreateDirectoryW(dir, NULL);
        lstrcatW(dir, L"\\wiz3D_proxy.log");
        fp = _wfopen(dir, L"a");
    }
    if (!fp) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fflush(fp);
}

bool BuildHunterPath(uint32_t crc, HuntStage stage, TCHAR out[MAX_PATH])
{
    // DX9ShaderDataCache/<Game>/Hunter/<CRC>_<stage>_<ts>.txt
    _tcscpy(out, gInfo.DriverDirectory);
    PathAppend(out, _T("DX9ShaderDataCache"));
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

bool HunterIsActive() { return PathIsOn(); }

void HunterNoteCreated(uint32_t crc, HuntStage stage, const void* bytecode, uint32_t sizeBytes)
{
    if (!crc) return;
    std::lock_guard<std::mutex> g(s_lock);
    HuntMeta m;
    m.stage = stage;
    if (bytecode && sizeBytes)
    {
        m.bytecode = std::make_shared<std::vector<BYTE>>(
            (const BYTE*)bytecode, (const BYTE*)bytecode + sizeBytes);
    }
    s_known[MakeKey(crc, stage)] = m;
}

void HunterNoteDraw(uint32_t vsCrc, uint32_t psCrc)
{
    if (!PathIsOn()) return;
    std::lock_guard<std::mutex> g(s_lock);
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

bool HunterShouldSkipDraw(uint32_t vsCrc, uint32_t psCrc)
{
    if (!PathIsOn()) return false;
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

void HunterFrameEnd()
{
    if (!PathIsOn()) return;
    std::lock_guard<std::mutex> g(s_lock);
    s_cycle.assign(s_drewThisFrame.begin(), s_drewThisFrame.end());
    std::sort(s_cycle.begin(), s_cycle.end());
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
    s_drewThisFrame.clear();
    s_drawCountForFrame.clear();
}

void HunterOnToggle()
{
    std::lock_guard<std::mutex> g(s_lock);
    const bool next = !g_active.load(std::memory_order_relaxed);
    g_active.store(next, std::memory_order_release);
    if (!next) s_hidden = 0;
    HLog("[hunt-dx9] %s\n", next ? "ENTER" : "EXIT");
}

void HunterOnCyclePrev()
{
    // Always log the outcome so a "nothing happens" complaint has a
    // diagnostic trail. Mirror of the DX11 ShaderHunter.
    if (!PathIsOn()) { HLog("[hunt-dx9] prev: hunt mode is off (Numpad 0 to enter)\n"); return; }
    std::lock_guard<std::mutex> g(s_lock);
    if (s_cycle.empty()) { HLog("[hunt-dx9] prev: cycle empty (no draws observed this frame yet)\n"); return; }
    if (s_cycleIdx == 0) s_cycleIdx = s_cycle.size() - 1;
    else                 s_cycleIdx--;
    s_hidden = s_cycle[s_cycleIdx];
    HLog("[hunt-dx9] prev -> CRC=0x%08X stage=%s (%zu/%zu)\n",
         (uint32_t)(s_hidden >> 1),
         ((s_hidden & 1) ? "PS" : "VS"),
         s_cycleIdx + 1, s_cycle.size());
}

void HunterOnCycleNext()
{
    if (!PathIsOn()) { HLog("[hunt-dx9] next: hunt mode is off (Numpad 0 to enter)\n"); return; }
    std::lock_guard<std::mutex> g(s_lock);
    if (s_cycle.empty()) { HLog("[hunt-dx9] next: cycle empty (no draws observed this frame yet)\n"); return; }
    s_cycleIdx = (s_cycleIdx + 1) % s_cycle.size();
    s_hidden = s_cycle[s_cycleIdx];
    HLog("[hunt-dx9] next -> CRC=0x%08X stage=%s (%zu/%zu)\n",
         (uint32_t)(s_hidden >> 1),
         ((s_hidden & 1) ? "PS" : "VS"),
         s_cycleIdx + 1, s_cycle.size());
}

void HunterOnCapture()
{
    if (!PathIsOn()) return;
    CrcStageKey hidden;
    HuntMeta    meta;
    uint32_t    drawsSeen;
    {
        std::lock_guard<std::mutex> g(s_lock);
        // 0.4.1: if the user presses Numpad 3 before cycling, auto-pick the first
        // shader in the current cycle so the capture still fires. Matches DX11.
        if (!s_hidden && !s_cycle.empty())
        {
            s_cycleIdx = 0;
            s_hidden   = s_cycle[0];
        }
        hidden = s_hidden;
        if (!hidden) { HLog("[hunt-dx9] capture: nothing hidden and no draws seen yet\n"); return; }
        auto it = s_known.find(hidden);
        if (it == s_known.end())
        {
            HLog("[hunt-dx9] capture: no metadata for CRC=0x%08X\n", (uint32_t)(hidden >> 1));
            return;
        }
        meta = it->second;
        auto dit = s_drawCountForFrame.find(hidden);
        drawsSeen = (dit == s_drawCountForFrame.end()) ? 0 : dit->second;
    }

    uint32_t  crc   = (uint32_t)(hidden >> 1);
    HuntStage stage = (HuntStage)(hidden & 1);

    TCHAR path[MAX_PATH];
    if (!BuildHunterPath(crc, stage, path))
    {
        HLog("[hunt-dx9] capture: could not build output path\n");
        return;
    }

    FILE* fp = _tfopen(path, _T("w"));
    if (!fp)
    {
        HLog("[hunt-dx9] capture: fopen failed\n");
        return;
    }

    fprintf(fp, "Shader CRC:   0x%08X\n", crc);
    fprintf(fp, "Stage:        %s\n", (stage == HuntStage::VS ? "VS" : "PS"));
    fprintf(fp, "Draws seen:   %u (this frame)\n", drawsSeen);
    fprintf(fp, "API:          DirectX 9 (SM1-3)\n\n");

    if (meta.bytecode && !meta.bytecode->empty())
    {
        CComPtr<ID3DXBuffer> pDisasm;
        HRESULT hr = D3DXDisassembleShader(
            (const DWORD*)meta.bytecode->data(),
            FALSE,  // EnableColorCode
            NULL,
            &pDisasm);
        if (SUCCEEDED(hr) && pDisasm)
        {
            fprintf(fp, "---- D3DXDisassembleShader ----\n");
            fprintf(fp, "%s\n", (const char*)pDisasm->GetBufferPointer());
        }
        else
        {
            fprintf(fp, "D3DXDisassembleShader failed hr=0x%08X\n", (unsigned)hr);
        }
    }
    else
    {
        fprintf(fp, "No bytecode retained for this shader.\n");
    }

    // DX9's shader-fix path uses iZ3D-native profile XML. The capture report
    // points authors at the <Mono> block for shader-level stereo-off. More
    // targeted stereo correction (projection matrices etc) uses <Multiplier>
    // and <Matrices> entries — see docs/SHADER_FIXES.md.
    fprintf(fp, "\n---- Authoring: mono-force this shader ----\n"
                "   <Profile Name=\"%s\">\n"
                "       <Mono><%sShader CRC=\"0x%08X\"/></Mono>\n"
                "   </Profile>\n"
                "\n"
                "For projection-matrix fixes see <Matrices> / <Multiplier> in\n"
                "docs/SHADER_FIXES.md.\n",
            "", (stage == HuntStage::VS ? "Vertex" : "Pixel"), crc);

    fclose(fp);
    HLog("[hunt-dx9] captured CRC=0x%08X stage=%s to %ls\n",
         crc, (stage == HuntStage::VS ? "VS" : "PS"), path);

    // Save-toast — mirror of DX11. 2 s matches the OSD status popup.
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

HunterState HunterGetState()
{
    HunterState out = {};
    // Lock up-front so the save-toast fields are readable even if hunt
    // mode is off (mirrors DX11 behaviour — toast survives exiting hunt).
    std::lock_guard<std::mutex> g(s_lock);
    if (s_flashDeadline && (uint64_t)GetTickCount64() < s_flashDeadline)
    {
        out.flashDeadline = s_flashDeadline;
        strncpy_s(out.flashLeaf, s_flashLeaf, _TRUNCATE);
    }
    if (!PathIsOn()) return out;
    out.active = true;
    if (s_hidden)
    {
        out.hiddenCRC   = (uint32_t)(s_hidden >> 1);
        out.hiddenStage = (HuntStage)(s_hidden & 1);
        auto it = s_drawCountForFrame.find(s_hidden);
        out.drawsForHidden = (it == s_drawCountForFrame.end()) ? 0 : it->second;
        out.cycleIdx = s_cycleIdx + 1;
    }
    out.cycleSize = s_cycle.size();
    return out;
}

// 0.4.1 runtime profile reload. Mirrors the DX11 side in S3DWrapperD3D10.cpp.
// Ctrl+F5 queues; the actual re-parse happens at Present boundary when no
// draw is reading g_ProfileData.
namespace
{
std::atomic<bool> g_profileReloadPending{ false };
}

void RequestProfileReload()
{
    g_profileReloadPending.store(true, std::memory_order_release);
    HLog("[reload-dx9] Ctrl+F5 pressed — BaseProfile.xml reload queued\n");
}

void MaybeReloadProfileBetweenFrames()
{
    if (!g_profileReloadPending.exchange(false, std::memory_order_acq_rel))
        return;
    HLog("[reload-dx9] re-parsing BaseProfile.xml ...\n");
    g_ProfileData.VSCRCData.clear();
    g_ProfileData.PSCRCData.clear();
    g_ProfileData.GSCRCData.clear();
    FreeProfiles();
    // DX9 doesn't park the VendorId globally the way DX10/11 does on
    // gData.VendorId. Pass 0 — the reload reads BaseProfile.xml fresh and
    // the vendor-specific selectors in ReadCurrentProfile fall back to
    // generic. The initial device-create call still used the real vendor;
    // we're only invalidating the ShaderPatch / ShaderOverride CRC maps.
    ReadCurrentProfile(0);
    HLog("[reload-dx9] reload done, VS=%u PS=%u GS=%u entries\n",
         (unsigned)g_ProfileData.VSCRCData.size(),
         (unsigned)g_ProfileData.PSCRCData.size(),
         (unsigned)g_ProfileData.GSCRCData.size());
}

}  // namespace dx9
}  // namespace wiz3d
