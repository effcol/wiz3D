/*
 * wiz3D Simulated Reality -- menu-squeeze implementation
 *
 * Copyright (C) 2026 wiz3D contributors.
 * Licensed under the GNU Lesser General Public License, version 2.1.
 *
 * Clean-room re-implementation. The flat/stereo detection trick (diff
 * Device vtable call counters each frame; GetEyePosition is polled only
 * on stereo frames) is from vl3c's braindance3d_menus.c (MIT 2026) --
 * studied as reference, no code copied. The DX12 copy+squeeze pipeline
 * shape (CopyResource → fullscreen-triangle draw sampling into both
 * halves) is also community-known DX12 content; our implementation is
 * written from scratch against the DX12 API.
 */

#include "menu_squeeze.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace wiz3d_sr {

// ---------------------------------------------------------------------------
// Logging -- shared wiz3D_proxy.log, tagged [SR-squeeze] to distinguish
// from the bootstrap's own entries.
// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logLock;
static bool g_logInit = false;

static void LogInit()
{
    if (g_logInit) return;
    InitializeCriticalSection(&g_logLock);
    g_logInit = true;
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    WCHAR* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    lstrcatW(path, L"wiz3D_Logs");
    CreateDirectoryW(path, NULL);
    lstrcatW(path, L"\\wiz3D_proxy.log");
    g_log = _wfopen(path, L"a");
}

static void Log(const char* fmt, ...)
{
    if (!g_logInit) LogInit();
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02u:%02u:%02u.%03u] [SR-squeeze] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

// ---------------------------------------------------------------------------
// Device-call-counter bridge to our Odyssey3DSDK.dll
//
// The counter getter is exported from Odyssey3DSDK.dll. We resolve it
// lazily -- the game loads Odyssey3DSDK.dll via its registry-driven path,
// which happens after our dxgi.dll's DllMain and typically before the
// first Present we care about. If we can't resolve, we skip detection
// (treat everything as a stereo frame, i.e. no squeeze) -- safer than
// over-squeezing stereo content.
// ---------------------------------------------------------------------------
typedef LONG64 (*PFN_GetDeviceCallCount)(int slot);
static PFN_GetDeviceCallCount g_pfnDeviceCallCount = nullptr;
static bool g_counterResolveTried = false;

static LONG64 GetDeviceCallCount(int slot)
{
    if (!g_pfnDeviceCallCount && !g_counterResolveTried)
    {
        g_counterResolveTried = true;
        HMODULE h = GetModuleHandleW(L"Odyssey3DSDK.dll");
        if (h)
        {
            g_pfnDeviceCallCount = (PFN_GetDeviceCallCount)
                GetProcAddress(h, "wiz3d_sr_get_device_call_count");
            if (g_pfnDeviceCallCount)
                Log("Resolved Odyssey3DSDK counter getter at %p\n", (void*)g_pfnDeviceCallCount);
            else
                Log("WARN: Odyssey3DSDK.dll loaded but counter getter not exported\n");
        }
    }
    if (!g_pfnDeviceCallCount) return 0;
    LONG64 v = g_pfnDeviceCallCount(slot);
    return v < 0 ? 0 : v;
}

// Slot ids match Odyssey3DSDK's Device vtable layout.
static const int SLOT_GET_EYE_POSITION = 1;
static const int SLOT_GET_MONITOR_RECT = 6;
static const int SLOT_IS_AVAILABLE     = 10;

// ---------------------------------------------------------------------------
// Config -- envvar overrides. XML config comes in a later phase.
// ---------------------------------------------------------------------------
static bool g_enabled = true;    // WIZ3D_SR_MENU_SQUEEZE=0 disables

static void ReadConfig()
{
    char v[8] = {};
    DWORD n = GetEnvironmentVariableA("WIZ3D_SR_MENU_SQUEEZE", v, sizeof(v));
    if (n > 0 && v[0] == '0') g_enabled = false;
    Log("Menu squeeze %s (set WIZ3D_SR_MENU_SQUEEZE=0 to disable)\n",
        g_enabled ? "ENABLED" : "DISABLED");
}

// Only squeeze once we've seen at least one stereo frame (via a positive
// GetEyePosition delta) -- otherwise we'd squeeze the main menu / splash
// screens before 3D ever activates. Seeded true at init if CP2077's
// UserSettings.json says 3D Mode is enabled (see SeedStereoFromUserSettings
// below), so the pre-engine CDPR logo squeezes properly.
static bool g_stereoEverSeen = false;

// Seed g_stereoEverSeen from CP2077's persisted 3D-Mode toggle so the
// squeeze fires on the pre-engine CDPR intro logo too (the game writes
// a flat full-width logo frame before it activates its stereo render
// path, so GetEyePosition hasn't fired yet and our usual "wait for first
// stereo frame" gate stays closed -- user then sees a mono logo for
// a couple of seconds before stereo kicks in). If the config says 3D is
// enabled, we assume we're going to see stereo frames shortly and start
// squeezing flat frames immediately. If the config says off / can't be
// read, we leave the safe default (never squeeze until stereo seen).
//
// File: %LOCALAPPDATA%\CD Projekt Red\Cyberpunk 2077\UserSettings.json
// Key:  "Odyssey3D_StereoEnabled" : true
//
// Substring scan rather than a full JSON parse -- the file is well-formed
// CP2077 config, the key name is distinctive enough to not collide, and
// we only need a true/false branch.
static void SeedStereoFromUserSettings()
{
    WCHAR path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wcscat_s(path, MAX_PATH, L"\\CD Projekt Red\\Cyberpunk 2077\\UserSettings.json");

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Log("UserSettings.json not found (%ls); squeeze gated on first stereo frame\n", path);
        return;
    }
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > 4 * 1024 * 1024) { CloseHandle(h); return; }
    char* buf = (char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)sz.QuadPart + 1);
    if (!buf) { CloseHandle(h); return; }
    DWORD read = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)sz.QuadPart, &read, NULL);
    CloseHandle(h);
    if (!ok) { HeapFree(GetProcessHeap(), 0, buf); return; }
    buf[read] = 0;

    const char* key = strstr(buf, "Odyssey3D_StereoEnabled");
    if (key) {
        // Walk past the key, past the colon, past whitespace, read first
        // non-whitespace token to classify true/false.
        const char* p = key + strlen("Odyssey3D_StereoEnabled");
        while (*p && *p != ':' && *p != '\n') ++p;
        if (*p == ':') ++p;
        while (*p == ' ' || *p == '\t' || *p == '"') ++p;
        if (p[0] == 't' || p[0] == 'T' || p[0] == '1') {
            g_stereoEverSeen = true;
            Log("UserSettings.json: Odyssey3D_StereoEnabled=true -> seeding squeeze ON from frame 1 (CDPR-logo path)\n");
        } else {
            Log("UserSettings.json: Odyssey3D_StereoEnabled=false -> squeeze stays gated on first stereo frame\n");
        }
    } else {
        Log("UserSettings.json: Odyssey3D_StereoEnabled key not found -> squeeze stays gated on first stereo frame\n");
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

// ---------------------------------------------------------------------------
// Globals -- single swap chain at a time (CP2077 pattern). If a game
// creates multiple swap chains we track the first and let the rest pass.
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_lock;
static bool g_lockInit = false;

static IDXGISwapChain*      g_sc           = nullptr;    // AddRef'd
static ID3D12Device*        g_device       = nullptr;    // AddRef'd
static ID3D12CommandQueue*  g_queue        = nullptr;    // AddRef'd, direct queue from CreateSwapChainForHwnd

// Pipeline resources -- created lazily on first squeeze.
static ID3D12Resource*            g_copyTex       = nullptr;
static D3D12_RESOURCE_DESC        g_copyDesc      = {};
static DXGI_FORMAT                g_bbFormat      = DXGI_FORMAT_UNKNOWN;
static UINT                       g_bbWidth       = 0;
static UINT                       g_bbHeight      = 0;
static ID3D12DescriptorHeap*      g_srvHeap       = nullptr;
static ID3D12DescriptorHeap*      g_rtvHeap       = nullptr;
static ID3D12RootSignature*       g_rootSig       = nullptr;
static ID3D12PipelineState*       g_pso           = nullptr;
static ID3D12CommandAllocator*    g_alloc[4]      = {};
static ID3D12GraphicsCommandList* g_cmd           = nullptr;
static ID3D12Fence*               g_fence         = nullptr;
static UINT64                     g_fenceVal      = 0;
static HANDLE                     g_fenceEvent    = NULL;
static unsigned                   g_allocIdx      = 0;

// Per-frame detection state
static LONG64 g_lastEye     = 0;
static LONG64 g_lastAvail   = 0;
static LONG64 g_lastRect    = 0;
// g_stereoEverSeen declared above the config section so SeedStereoFromUserSettings can see it.

// ---------------------------------------------------------------------------
// Vtable patch helper -- raw pointer swap guarded by VirtualProtect so we
// can write to the typically-read-only vtable page. Returns the previous
// slot value so we can call through to it.
// ---------------------------------------------------------------------------
static void* PatchVtableSlot(void* object, size_t slot, void* newFn)
{
    void*** vtbl = (void***)object;
    void*  page  = (void*)&((*vtbl)[slot]);
    DWORD oldProt = 0;
    if (!VirtualProtect(page, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt))
    {
        Log("VirtualProtect failed on vtable[%zu] of %p (err=%lu)\n",
            slot, object, GetLastError());
        return nullptr;
    }
    void* prev = (*vtbl)[slot];
    (*vtbl)[slot] = newFn;
    VirtualProtect(page, sizeof(void*), oldProt, &oldProt);
    return prev;
}

// ---------------------------------------------------------------------------
// Original function pointer trampolines (one per hooked vtable slot).
// We keep them global static so our hook functions can call through.
// ---------------------------------------------------------------------------
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChain       )(IDXGIFactory*,  IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present               )(IDXGISwapChain*,  UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present1              )(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

static PFN_CreateSwapChain        g_origCreateSwapChain        = nullptr;
static PFN_CreateSwapChainForHwnd g_origCreateSwapChainForHwnd = nullptr;
static PFN_Present                g_origPresent                = nullptr;
static PFN_Present1               g_origPresent1               = nullptr;

// IDXGIFactory      slots: QI=0, AddRef=1, Release=2, SetPrivateData=3,
//                          SetPrivateDataInterface=4, GetPrivateData=5,
//                          GetParent=6, EnumAdapters=7, MakeWindowAssociation=8,
//                          GetWindowAssociation=9, CreateSwapChain=10,
//                          CreateSoftwareAdapter=11
// IDXGIFactory2     slots 15=CreateSwapChainForHwnd
// IDXGISwapChain    slots 8=Present
// IDXGISwapChain1   slots 22=Present1
static const size_t SLOT_FACTORY_CREATE_SC           = 10;
static const size_t SLOT_FACTORY2_CREATE_SC_FOR_HWND = 15;
static const size_t SLOT_SWAPCHAIN_PRESENT           = 8;
static const size_t SLOT_SWAPCHAIN1_PRESENT1         = 22;

// ---------------------------------------------------------------------------
// DX12 pipeline init / teardown.
//
// Called under g_lock, from the Present hook, lazily the first time we
// detect a flat frame on a captured swap chain + queue. If any step
// fails we clean up and leave g_enabled set so future frames also skip.
// ---------------------------------------------------------------------------
static const char* kVsSrc =
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut main(uint id : SV_VertexID) {\n"
    "  VSOut o;\n"
    "  o.uv  = float2((id << 1) & 2, id & 2);\n"
    "  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "  return o;\n"
    "}\n";

// Both halves of the output = full BB squeezed to half-width. Matches
// vl3c's shader (community-known shape).
static const char* kPsSrc =
    "Texture2D frame : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 main(VSOut i) : SV_Target {\n"
    "  float x = i.uv.x < 0.5 ? i.uv.x * 2.0 : i.uv.x * 2.0 - 1.0;\n"
    "  return frame.Sample(samp, float2(x, i.uv.y));\n"
    "}\n";

static ID3DBlob* Compile(const char* src, const char* target)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* err  = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            "main", target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        Log("D3DCompile %s failed hr=0x%08lX: %s\n", target, hr,
            err ? (const char*)err->GetBufferPointer() : "<no msg>");
        if (err) err->Release();
        if (code) code->Release();
        return nullptr;
    }
    if (err) err->Release();
    return code;
}

static void ReleasePipelineResources()
{
    if (g_cmd)      { g_cmd->Release();      g_cmd      = nullptr; }
    for (int i = 0; i < 4; ++i) if (g_alloc[i]) { g_alloc[i]->Release(); g_alloc[i] = nullptr; }
    if (g_pso)      { g_pso->Release();      g_pso      = nullptr; }
    if (g_rootSig)  { g_rootSig->Release();  g_rootSig  = nullptr; }
    if (g_rtvHeap)  { g_rtvHeap->Release();  g_rtvHeap  = nullptr; }
    if (g_srvHeap)  { g_srvHeap->Release();  g_srvHeap  = nullptr; }
    if (g_copyTex)  { g_copyTex->Release();  g_copyTex  = nullptr; }
    if (g_fence)    { g_fence->Release();    g_fence    = nullptr; }
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = NULL; }
    g_bbWidth = g_bbHeight = 0;
    g_bbFormat = DXGI_FORMAT_UNKNOWN;
}

static bool EnsurePipeline(ID3D12Device* device, ID3D12Resource* bb)
{
    D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();

    // Resize/format change -> rebuild only the dim/format-dependent pieces.
    if (g_copyTex && (g_bbWidth != bbDesc.Width || g_bbHeight != bbDesc.Height
                      || g_bbFormat != bbDesc.Format))
    {
        Log("BB geometry changed (%ux%u fmt=%d -> %llux%u fmt=%d); rebuilding pipeline\n",
            g_bbWidth, g_bbHeight, (int)g_bbFormat,
            (unsigned long long)bbDesc.Width, bbDesc.Height, (int)bbDesc.Format);
        ReleasePipelineResources();
    }

    if (g_copyTex) return true;    // already initialised and dims match

    g_bbWidth  = (UINT)bbDesc.Width;
    g_bbHeight = bbDesc.Height;
    g_bbFormat = bbDesc.Format;

    HRESULT hr;

    // Intermediate copy texture -- same desc as BB, no special flags.
    D3D12_HEAP_PROPERTIES heapDefault = {};
    heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC copyDesc = bbDesc;
    copyDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE,
                                         &copyDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                         nullptr, IID_PPV_ARGS(&g_copyTex));
    if (FAILED(hr)) { Log("CreateCommittedResource(copyTex) hr=0x%08lX\n", hr); return false; }

    // Descriptor heaps: 1 SRV (shader-visible), 1 RTV (CPU).
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap));
    if (FAILED(hr)) { Log("CreateDescriptorHeap(srv) hr=0x%08lX\n", hr); ReleasePipelineResources(); return false; }

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap));
    if (FAILED(hr)) { Log("CreateDescriptorHeap(rtv) hr=0x%08lX\n", hr); ReleasePipelineResources(); return false; }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = bbDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(g_copyTex, &srvDesc,
                                     g_srvHeap->GetCPUDescriptorHandleForHeapStart());

    // Root signature: 1 SRV descriptor table + 1 static linear-clamp sampler.
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp.DescriptorTable.NumDescriptorRanges = 1;
    rp.DescriptorTable.pDescriptorRanges = &range;

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &rp;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &samp;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* rsBlob = nullptr;
    ID3DBlob* rsErr  = nullptr;
    hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &rsBlob, &rsErr);
    if (FAILED(hr)) {
        Log("D3D12SerializeRootSignature hr=0x%08lX: %s\n", hr,
            rsErr ? (const char*)rsErr->GetBufferPointer() : "<no msg>");
        if (rsBlob) rsBlob->Release();
        if (rsErr)  rsErr->Release();
        ReleasePipelineResources();
        return false;
    }
    hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                     rsBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&g_rootSig));
    rsBlob->Release();
    if (rsErr) rsErr->Release();
    if (FAILED(hr)) { Log("CreateRootSignature hr=0x%08lX\n", hr); ReleasePipelineResources(); return false; }

    // Shaders + PSO
    ID3DBlob* vs = Compile(kVsSrc, "vs_5_0");
    ID3DBlob* ps = Compile(kPsSrc, "ps_5_0");
    if (!vs || !ps) {
        if (vs) vs->Release();
        if (ps) ps->Release();
        ReleasePipelineResources();
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = g_rootSig;
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = bbDesc.Format;
    pso.SampleDesc.Count = 1;
    hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pso));
    vs->Release(); ps->Release();
    if (FAILED(hr)) { Log("CreateGraphicsPipelineState hr=0x%08lX\n", hr); ReleasePipelineResources(); return false; }

    // Ring of 4 command allocators + 1 command list.
    for (int i = 0; i < 4; ++i) {
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&g_alloc[i]));
        if (FAILED(hr)) { Log("CreateCommandAllocator[%d] hr=0x%08lX\n", i, hr); ReleasePipelineResources(); return false; }
    }
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   g_alloc[0], g_pso, IID_PPV_ARGS(&g_cmd));
    if (FAILED(hr)) { Log("CreateCommandList hr=0x%08lX\n", hr); ReleasePipelineResources(); return false; }
    g_cmd->Close();

    // Fence
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    if (FAILED(hr)) { Log("CreateFence hr=0x%08lX\n", hr); ReleasePipelineResources(); return false; }
    g_fenceEvent = CreateEventW(NULL, FALSE, FALSE, NULL);

    Log("Pipeline initialised (BB %ux%u fmt=%d)\n", g_bbWidth, g_bbHeight, (int)g_bbFormat);
    return true;
}

// ---------------------------------------------------------------------------
// Squeeze execution -- runs on game's command queue inline between the
// game's last render command and its Present. Submits our own command
// list containing copy + draw + barriers.
// ---------------------------------------------------------------------------
static void RunSqueeze(IDXGISwapChain* sc)
{
    if (!g_device || !g_queue) return;

    UINT bbIdx = 0;
    {
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
            bbIdx = sc3->GetCurrentBackBufferIndex();
            sc3->Release();
        }
    }
    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(bbIdx, IID_PPV_ARGS(&bb)))) return;

    if (!EnsurePipeline(g_device, bb)) { bb->Release(); return; }

    // Wait for a ring slot if we're 4 frames behind (shouldn't happen in practice).
    unsigned slot = g_allocIdx % 4;
    UINT64 expectedComplete = g_fenceVal >= 4 ? g_fenceVal - 3 : 0;
    if (g_fence->GetCompletedValue() < expectedComplete) {
        g_fence->SetEventOnCompletion(expectedComplete, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 1000);
    }

    g_alloc[slot]->Reset();
    g_cmd->Reset(g_alloc[slot], g_pso);

    // bb: PRESENT -> COPY_SOURCE   copyTex: COPY_DEST (expected steady state)
    D3D12_RESOURCE_BARRIER b[2] = {};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = bb;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_cmd->ResourceBarrier(1, &b[0]);

    g_cmd->CopyResource(g_copyTex, bb);

    // Transition for the draw: bb -> RT, copyTex -> PS SRV
    b[0].Transition.pResource = bb;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[1].Transition.pResource = g_copyTex;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_cmd->ResourceBarrier(2, b);

    // Bind RTV for BB
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = g_bbFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_device->CreateRenderTargetView(bb, &rtvDesc, rtv);
    g_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT vp = { 0.f, 0.f, (float)g_bbWidth, (float)g_bbHeight, 0.f, 1.f };
    D3D12_RECT     sc_r = { 0, 0, (LONG)g_bbWidth, (LONG)g_bbHeight };
    g_cmd->RSSetViewports(1, &vp);
    g_cmd->RSSetScissorRects(1, &sc_r);

    ID3D12DescriptorHeap* heaps[1] = { g_srvHeap };
    g_cmd->SetDescriptorHeaps(1, heaps);
    g_cmd->SetGraphicsRootSignature(g_rootSig);
    g_cmd->SetGraphicsRootDescriptorTable(0, g_srvHeap->GetGPUDescriptorHandleForHeapStart());
    g_cmd->SetPipelineState(g_pso);
    g_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_cmd->DrawInstanced(3, 1, 0, 0);

    // Transition back for Present and next frame's CopyResource
    b[0].Transition.pResource = bb;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    b[1].Transition.pResource = g_copyTex;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    g_cmd->ResourceBarrier(2, b);

    g_cmd->Close();
    ID3D12CommandList* lists[] = { g_cmd };
    g_queue->ExecuteCommandLists(1, lists);
    g_fenceVal++;
    g_queue->Signal(g_fence, g_fenceVal);
    g_allocIdx++;

    bb->Release();
}

// ---------------------------------------------------------------------------
// Hooked Present(1) -- detect flat/stereo via counter diffs, squeeze
// if flat + stereo mode is on.
// ---------------------------------------------------------------------------
static void BeforePresent(IDXGISwapChain* sc)
{
    if (!g_enabled) return;
    if (sc != g_sc) return;    // only operate on the captured swap chain

    LONG64 eye   = GetDeviceCallCount(SLOT_GET_EYE_POSITION);
    LONG64 avail = GetDeviceCallCount(SLOT_IS_AVAILABLE);
    LONG64 rect  = GetDeviceCallCount(SLOT_GET_MONITOR_RECT);

    LONG64 dEye   = eye   - g_lastEye;
    LONG64 dAvail = avail - g_lastAvail;
    LONG64 dRect  = rect  - g_lastRect;
    g_lastEye = eye; g_lastAvail = avail; g_lastRect = rect;

    (void)dAvail; (void)dRect;    // reserved for stereo-ON/OFF tracking in future phase

    if (dEye > 0) {
        g_stereoEverSeen = true;
        return;    // stereo frame; do not squeeze
    }

    if (!g_stereoEverSeen) return;    // pre-3D-handshake; main menu stays as the game draws it

    RunSqueeze(sc);
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* sc, UINT interval, UINT flags)
{
    if (!(flags & DXGI_PRESENT_TEST)) BeforePresent(sc);
    return g_origPresent(sc, interval, flags);
}

static HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* sc, UINT interval, UINT flags,
                                                const DXGI_PRESENT_PARAMETERS* params)
{
    if (!(flags & DXGI_PRESENT_TEST)) BeforePresent(sc);
    return g_origPresent1(sc, interval, flags, params);
}

// ---------------------------------------------------------------------------
// Capture the swap chain + queue the first time the game creates one via
// IDXGIFactory2::CreateSwapChainForHwnd (DX12 games). The first-arg
// pDevice on DX12 is actually ID3D12CommandQueue* -- that's our handle
// to the game's direct queue.
// ---------------------------------------------------------------------------
static void CaptureSwapChainAndQueue(IUnknown* pDevice, IDXGISwapChain* sc)
{
    EnterCriticalSection(&g_lock);
    if (g_sc) {
        LeaveCriticalSection(&g_lock);
        return;    // only track the first one
    }

    ID3D12CommandQueue* q = nullptr;
    if (pDevice && SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&q))) && q) {
        g_queue = q;    // AddRef'd by QI
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&dev)))) {
            g_device = dev;    // AddRef'd
        }
    } else {
        Log("CreateSwapChainForHwnd pDevice is not an ID3D12CommandQueue -- not DX12? Skipping capture.\n");
        LeaveCriticalSection(&g_lock);
        return;
    }

    sc->AddRef();
    g_sc = sc;

    // Patch Present on the swap-chain object. Both IDXGISwapChain::Present and
    // IDXGISwapChain1::Present1 live on the same vtable pointer target (same
    // COM object), so we patch both slots.
    g_origPresent = (PFN_Present)PatchVtableSlot(sc, SLOT_SWAPCHAIN_PRESENT, (void*)&HookedPresent);
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
        g_origPresent1 = (PFN_Present1)PatchVtableSlot(sc1, SLOT_SWAPCHAIN1_PRESENT1, (void*)&HookedPresent1);
        sc1->Release();
    }

    DXGI_SWAP_CHAIN_DESC d = {};
    sc->GetDesc(&d);
    Log("Captured swap chain=%p queue=%p device=%p BB=%ux%u fmt=%d; Present slots patched\n",
        (void*)sc, (void*)g_queue, (void*)g_device,
        d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format);

    // Pin this DLL so the patched vtable slots never dangle on unload.
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCWSTR)&CaptureSwapChainAndQueue, &self);
    LeaveCriticalSection(&g_lock);
}

static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
    IDXGIFactory2* f, IUnknown* pDevice, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
    IDXGIOutput* out, IDXGISwapChain1** ppSc)
{
    HRESULT hr = g_origCreateSwapChainForHwnd(f, pDevice, hwnd, d, fs, out, ppSc);
    if (SUCCEEDED(hr) && ppSc && *ppSc) CaptureSwapChainAndQueue(pDevice, *ppSc);
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
    IDXGIFactory* f, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** ppSc)
{
    HRESULT hr = g_origCreateSwapChain(f, pDevice, d, ppSc);
    if (SUCCEEDED(hr) && ppSc && *ppSc) CaptureSwapChainAndQueue(pDevice, *ppSc);
    return hr;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
void MenuSqueezeInit()
{
    if (g_lockInit) return;
    InitializeCriticalSection(&g_lock);
    g_lockInit = true;
    LogInit();
    ReadConfig();
    SeedStereoFromUserSettings();
    Log("MenuSqueezeInit\n");
}

void MenuSqueezeShutdown()
{
    Log("MenuSqueezeShutdown (DLL pinned; vtable patches persist for process lifetime)\n");
    // We deliberately don't un-patch -- the DLL is pinned (see CaptureSwapChainAndQueue)
    // so our hook pointers remain valid for process lifetime. Releasing the pipeline
    // resources is fine though.
    EnterCriticalSection(&g_lock);
    ReleasePipelineResources();
    if (g_sc)     { g_sc->Release();     g_sc     = nullptr; }
    if (g_queue)  { g_queue->Release();  g_queue  = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    LeaveCriticalSection(&g_lock);
}

void OnFactoryCreated(IDXGIFactory* factory)
{
    if (!factory) return;
    MenuSqueezeInit();

    EnterCriticalSection(&g_lock);
    if (!g_origCreateSwapChain)
        g_origCreateSwapChain = (PFN_CreateSwapChain)PatchVtableSlot(factory, SLOT_FACTORY_CREATE_SC,
                                                                     (void*)&HookedCreateSwapChain);

    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f2))) && f2) {
        if (!g_origCreateSwapChainForHwnd)
            g_origCreateSwapChainForHwnd = (PFN_CreateSwapChainForHwnd)
                PatchVtableSlot(f2, SLOT_FACTORY2_CREATE_SC_FOR_HWND,
                                (void*)&HookedCreateSwapChainForHwnd);
        f2->Release();
    }
    Log("Factory %p vtable patched (orig CreateSwapChain=%p CreateSwapChainForHwnd=%p)\n",
        (void*)factory, (void*)g_origCreateSwapChain, (void*)g_origCreateSwapChainForHwnd);
    LeaveCriticalSection(&g_lock);
}

}  // namespace wiz3d_sr
