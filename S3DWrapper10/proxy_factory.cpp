/* wiz3D d3d11 - COM-proxy factory bridge implementation (Option B Stage 2).
 *
 * Ported pattern from NvDirectMode/d3d11/proxy_factory.cpp. The export
 * wiz3D_WrapD3D11DeviceAndContext is the integration point the d3d11.dll
 * proxy (wiz3D-proxy/d3d11/dllmain.cpp) calls right after the real
 * D3D11CreateDevice returns, swapping the returned device + immediate
 * context for our wrappers before the game sees them.
 */

#include "StdAfx.h"
#include "proxy_factory.h"
#include "Device11Proxy.h"
#include "Context11Proxy.h"
#include "Texture2D11Proxy.h"
#include "RTV11Proxy.h"
#include "DSV11Proxy.h"
#include "Buffer11Proxy.h"
#include "SRV11Proxy.h"
#include "UAV11Proxy.h"
#include "Texture1D11Proxy.h"
#include "Texture3D11Proxy.h"
#include "Device10Proxy.h"
#include "Texture2D10Proxy.h"
#include "RTV10Proxy.h"
#include "DSV10Proxy.h"
#include "Buffer10Proxy.h"
#include "SRV10Proxy.h"
#include "Texture1D10Proxy.h"
#include "Texture3D10Proxy.h"
#include "SwapChain10Proxy.h"
#include "SwapChain11Proxy.h"
#include "AdapterFunctions.h"  // DDILog

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// {3D8F1B2E-7A4C-4D6E-B1F0-8C3A9D2E5F6B}
// Private GUID claimed by Device11Proxy::QueryInterface so cross-DLL identity
// checks (the dxgi.dll proxy probing whether a CreateSwapChain device is one
// of ours) work via memcmp on the IID alone — no shared symbols needed.
EXTERN_C const GUID IID_wiz3D_Device11Proxy =
    { 0x3D8F1B2E, 0x7A4C, 0x4D6E, { 0xB1, 0xF0, 0x8C, 0x3A, 0x9D, 0x2E, 0x5F, 0x6B } };

// Stage 3b: private GUIDs for the resource/view proxies. Used by the
// Try*Unwrap helpers to identify wiz3D wrappers passed at COM boundaries
// before forwarding to the real D3D11 runtime. Same memcmp-on-IID pattern
// keeps every TU that needs to unwrap free of header dependencies on the
// concrete proxy classes.
// {C8E94A12-3B5D-4F2A-9D1C-7A6B8E3F4C5D}
EXTERN_C const GUID IID_wiz3D_Texture2D11Proxy =
    { 0xC8E94A12, 0x3B5D, 0x4F2A, { 0x9D, 0x1C, 0x7A, 0x6B, 0x8E, 0x3F, 0x4C, 0x5D } };
// {D9F05B23-4C6E-5A3B-AE2D-8B7C9F4D5E6E}
EXTERN_C const GUID IID_wiz3D_RTV11Proxy =
    { 0xD9F05B23, 0x4C6E, 0x5A3B, { 0xAE, 0x2D, 0x8B, 0x7C, 0x9F, 0x4D, 0x5E, 0x6E } };
// {EAF16C34-5D7F-6B4C-BF3E-9C8DAF5E6F7F}
EXTERN_C const GUID IID_wiz3D_DSV11Proxy =
    { 0xEAF16C34, 0x5D7F, 0x6B4C, { 0xBF, 0x3E, 0x9C, 0x8D, 0xAF, 0x5E, 0x6F, 0x7F } };
// {FB027D45-6E80-7C5D-CF4F-AD9EBF6F708F}
EXTERN_C const GUID IID_wiz3D_SwapChain11Proxy =
    { 0xFB027D45, 0x6E80, 0x7C5D, { 0xCF, 0x4F, 0xAD, 0x9E, 0xBF, 0x6F, 0x70, 0x8F } };
// {0C138E56-7F91-8D6E-DF50-BEAFC07F819F}
EXTERN_C const GUID IID_wiz3D_Buffer11Proxy =
    { 0x0C138E56, 0x7F91, 0x8D6E, { 0xDF, 0x50, 0xBE, 0xAF, 0xC0, 0x7F, 0x81, 0x9F } };
// Stage 3c.2 private IIDs.
// {7368B5CD-E809-FED5-46C7-25163FA6657C}
EXTERN_C const GUID IID_wiz3D_SRV11Proxy =
    { 0x7368B5CD, 0xE809, 0xFED5, { 0x46, 0xC7, 0x25, 0x16, 0x3F, 0xA6, 0x65, 0x7C } };
// {8479C6DE-F91A-0FE6-57D8-36274BB7768D}
EXTERN_C const GUID IID_wiz3D_UAV11Proxy =
    { 0x8479C6DE, 0xF91A, 0x0FE6, { 0x57, 0xD8, 0x36, 0x27, 0x4B, 0xB7, 0x76, 0x8D } };
// {958AD7EF-0A2B-10F7-68E9-47385CC8879E}
EXTERN_C const GUID IID_wiz3D_Texture1D11Proxy =
    { 0x958AD7EF, 0x0A2B, 0x10F7, { 0x68, 0xE9, 0x47, 0x38, 0x5C, 0xC8, 0x87, 0x9E } };
// {A69BE8F0-1B3C-2108-79FA-58496DD998AF}
EXTERN_C const GUID IID_wiz3D_Texture3D11Proxy =
    { 0xA69BE8F0, 0x1B3C, 0x2108, { 0x79, 0xFA, 0x58, 0x49, 0x6D, 0xD9, 0x98, 0xAF } };
// {1D249F67-80A2-9E7F-E061-CFB0D1809020} — DX10 Option B Device proxy
EXTERN_C const GUID IID_wiz3D_Device10Proxy =
    { 0x1D249F67, 0x80A2, 0x9E7F, { 0xE0, 0x61, 0xCF, 0xB0, 0xD1, 0x80, 0x90, 0x20 } };
// {2E35A078-91B3-AF80-F172-D0C1E2910131} — DX10 Texture2D
EXTERN_C const GUID IID_wiz3D_Texture2D10Proxy =
    { 0x2E35A078, 0x91B3, 0xAF80, { 0xF1, 0x72, 0xD0, 0xC1, 0xE2, 0x91, 0x01, 0x31 } };
// {3F46B189-A2C4-BA91-0283-E1D2F3A21242} — DX10 RTV
EXTERN_C const GUID IID_wiz3D_RTV10Proxy =
    { 0x3F46B189, 0xA2C4, 0xBA91, { 0x02, 0x83, 0xE1, 0xD2, 0xF3, 0xA2, 0x12, 0x42 } };
// {4057C29A-B3D5-CBA2-1394-F2E304B32353} — DX10 DSV
EXTERN_C const GUID IID_wiz3D_DSV10Proxy =
    { 0x4057C29A, 0xB3D5, 0xCBA2, { 0x13, 0x94, 0xF2, 0xE3, 0x04, 0xB3, 0x23, 0x53 } };
// {516803AB-C4E6-DCB3-24A5-03F415C43464} — DX10 Buffer
EXTERN_C const GUID IID_wiz3D_Buffer10Proxy =
    { 0x516803AB, 0xC4E6, 0xDCB3, { 0x24, 0xA5, 0x03, 0xF4, 0x15, 0xC4, 0x34, 0x64 } };
// {627914BC-D5F7-EDC4-35B6-1405269554A5} — DX10 SwapChain
EXTERN_C const GUID IID_wiz3D_SwapChain10Proxy =
    { 0x627914BC, 0xD5F7, 0xEDC4, { 0x35, 0xB6, 0x14, 0x05, 0x26, 0x95, 0x54, 0xA5 } };
// Stage 3c.2 DX10 port IIDs.
// {738A25CD-E608-FED5-46C7-25163FA6657D}
EXTERN_C const GUID IID_wiz3D_SRV10Proxy =
    { 0x738A25CD, 0xE608, 0xFED5, { 0x46, 0xC7, 0x25, 0x16, 0x3F, 0xA6, 0x65, 0x7D } };
// {849B36DE-F719-0FE6-57D8-36274BB7768E}
EXTERN_C const GUID IID_wiz3D_Texture1D10Proxy =
    { 0x849B36DE, 0xF719, 0x0FE6, { 0x57, 0xD8, 0x36, 0x27, 0x4B, 0xB7, 0x76, 0x8E } };
// {95AC47EF-082A-10F7-68E9-47385CC8879F}
EXTERN_C const GUID IID_wiz3D_Texture3D10Proxy =
    { 0x95AC47EF, 0x082A, 0x10F7, { 0x68, 0xE9, 0x47, 0x38, 0x5C, 0xC8, 0x87, 0x9F } };

namespace wiz3d
{

void WrapD3D11DeviceAndContext(void** ppDeviceInOut, void** ppContextInOut)
{
    if (!ppDeviceInOut || !*ppDeviceInOut) return;

    auto* realDevice  = static_cast<ID3D11Device*>(*ppDeviceInOut);

    // Cache hit: the real device is already wrapped. Return the existing
    // proxy with +1 AddRef and drop the duplicate real-device ref this
    // call brought in. Keeps shader fingerprint / overlay / RTV state
    // unified across the game's redundant D3D11CreateDevice calls (FC3
    // Dunia 2 does this 4x per level load, same real device each time).
    if (Device11Proxy* cached = FindAndAddRefDeviceByReal(realDevice))
    {
        realDevice->Release();
        DDILog("WrapD3D11DeviceAndContext: realDevice=%p -> reusing wiz3d::Device11Proxy=%p (cache hit)\n",
               realDevice, cached);
        if (ppContextInOut && *ppContextInOut)
        {
            auto* realCtx = static_cast<ID3D11DeviceContext*>(*ppContextInOut);
            Context11Proxy* ctxProxy = cached->GetContextProxy();
            if (ctxProxy && ctxProxy->GetReal() == realCtx)
            {
                ctxProxy->AddRef();
                realCtx->Release();   // duplicate ref from the paired CreateDevice
                *ppContextInOut = static_cast<ID3D11DeviceContext*>(ctxProxy);
                DDILog("WrapD3D11DeviceAndContext: realCtx=%p -> reusing wiz3d::Context11Proxy=%p\n",
                       realCtx, ctxProxy);
            }
            else
            {
                // Different real context (rare: game passed its own device at
                // another wrap entry). Wrap it against the cached device.
                auto* newCtx = new Context11Proxy(realCtx, cached);
                *ppContextInOut = static_cast<ID3D11DeviceContext*>(newCtx);
                DDILog("WrapD3D11DeviceAndContext: realCtx=%p (fresh) -> wiz3d::Context11Proxy=%p\n",
                       realCtx, newCtx);
            }
        }
        *ppDeviceInOut = static_cast<ID3D11Device*>(cached);
        return;
    }

    auto* deviceProxy = new Device11Proxy(realDevice);
    DDILog("WrapD3D11DeviceAndContext: realDevice=%p -> wiz3d::Device11Proxy=%p\n",
           realDevice, deviceProxy);

    if (ppContextInOut && *ppContextInOut)
    {
        auto* realCtx  = static_cast<ID3D11DeviceContext*>(*ppContextInOut);
        auto* ctxProxy = new Context11Proxy(realCtx, deviceProxy);
        deviceProxy->SetImmediateContextProxy(ctxProxy);
        DDILog("WrapD3D11DeviceAndContext: realCtx=%p -> wiz3d::Context11Proxy=%p\n",
               realCtx, ctxProxy);
        *ppContextInOut = static_cast<ID3D11DeviceContext*>(ctxProxy);
    }

    *ppDeviceInOut = static_cast<ID3D11Device*>(deviceProxy);
}

// COM single-inheritance: the IUnknown* returned via the private-IID QI has
// the same address as the proxy's `this`. Casting back to the concrete proxy
// type is a base→derived narrowing that's well-defined when the pointer's
// dynamic type matches — which it does here because only the proxy claims
// that private IID. C-style cast bypasses the C4946 "reinterpret between
// related classes" warning (the proxies inherit from IUnknown via ID3D11Xxx,
// so the compiler can see the relationship and reinterpret_cast triggers
// W4-as-error in this project). Standard idiom for COM private-IID lookups.

Texture2D11Proxy* TryUnwrapTexture2D(ID3D11Resource* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_Texture2D11Proxy,
                                  reinterpret_cast<void**>(&probe))) || !probe)
        return nullptr;
    probe->Release();   // drop QI ref — we only care about identity
    return (Texture2D11Proxy*)probe;
}

RTV11Proxy* TryUnwrapRTV(ID3D11RenderTargetView* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_RTV11Proxy,
                                  reinterpret_cast<void**>(&probe))) || !probe)
        return nullptr;
    probe->Release();
    return (RTV11Proxy*)probe;
}

DSV11Proxy* TryUnwrapDSV(ID3D11DepthStencilView* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_DSV11Proxy,
                                  reinterpret_cast<void**>(&probe))) || !probe)
        return nullptr;
    probe->Release();
    return (DSV11Proxy*)probe;
}

Buffer11Proxy* TryUnwrapBuffer(ID3D11Resource* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_Buffer11Proxy,
                                  reinterpret_cast<void**>(&probe))) || !probe)
        return nullptr;
    probe->Release();
    return (Buffer11Proxy*)probe;
}

SRV11Proxy* TryUnwrapSRV(ID3D11ShaderResourceView* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_SRV11Proxy,
                                  reinterpret_cast<void**>(&probe))) || !probe)
        return nullptr;
    probe->Release();
    return (SRV11Proxy*)probe;
}

UAV11Proxy* TryUnwrapUAV(ID3D11UnorderedAccessView* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_UAV11Proxy,
                                  reinterpret_cast<void**>(&probe))) || !probe)
        return nullptr;
    probe->Release();
    return (UAV11Proxy*)probe;
}

Texture1D11Proxy* TryUnwrapTexture1D(ID3D11Resource* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_Texture1D11Proxy,
                                  reinterpret_cast<void**>(&probe))) || !probe)
        return nullptr;
    probe->Release();
    return (Texture1D11Proxy*)probe;
}

Texture3D11Proxy* TryUnwrapTexture3D(ID3D11Resource* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_Texture3D11Proxy,
                                  reinterpret_cast<void**>(&probe))) || !probe)
        return nullptr;
    probe->Release();
    return (Texture3D11Proxy*)probe;
}

Texture2D10Proxy* TryUnwrapTexture2D_10(ID3D10Resource* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_Texture2D10Proxy,
                                  reinterpret_cast<void**>(&probe))) || !probe)
        return nullptr;
    probe->Release();
    return (Texture2D10Proxy*)probe;
}

RTV10Proxy* TryUnwrapRTV_10(ID3D10RenderTargetView* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_RTV10Proxy,
                                  reinterpret_cast<void**>(&probe))) || !probe)
        return nullptr;
    probe->Release();
    return (RTV10Proxy*)probe;
}

DSV10Proxy* TryUnwrapDSV_10(ID3D10DepthStencilView* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_DSV10Proxy,
                                  reinterpret_cast<void**>(&probe))) || !probe)
        return nullptr;
    probe->Release();
    return (DSV10Proxy*)probe;
}

Buffer10Proxy* TryUnwrapBuffer_10(ID3D10Resource* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_Buffer10Proxy,
                                  reinterpret_cast<void**>(&probe))) || !probe)
        return nullptr;
    probe->Release();
    return (Buffer10Proxy*)probe;
}

SRV10Proxy* TryUnwrapSRV_10(ID3D10ShaderResourceView* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_SRV10Proxy,
                                  reinterpret_cast<void**>(&probe))) || !probe)
        return nullptr;
    probe->Release();
    return (SRV10Proxy*)probe;
}

Texture1D10Proxy* TryUnwrapTexture1D_10(ID3D10Resource* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_Texture1D10Proxy,
                                  reinterpret_cast<void**>(&probe))) || !probe)
        return nullptr;
    probe->Release();
    return (Texture1D10Proxy*)probe;
}

Texture3D10Proxy* TryUnwrapTexture3D_10(ID3D10Resource* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_Texture3D10Proxy,
                                  reinterpret_cast<void**>(&probe))) || !probe)
        return nullptr;
    probe->Release();
    return (Texture3D10Proxy*)probe;
}

} // namespace wiz3d

extern "C" __declspec(dllexport) void
wiz3D_WrapD3D11DeviceAndContext(void** ppDeviceInOut, void** ppContextInOut)
{
    wiz3d::WrapD3D11DeviceAndContext(ppDeviceInOut, ppContextInOut);
}

extern "C" __declspec(dllexport) void
wiz3D_WrapD3D10Device(void** ppDeviceInOut)
{
    // DX10 Option B Stage 1: pure-passthrough wrap. Same gate as DX11 —
    // we always wrap when called so the legacy DDI path stays disabled
    // (the d3d10 dllmain only calls this when UseCOMWrap is on). The
    // wrapped device returns itself for ID3D10Device QI and the private
    // IID_wiz3D_Device10Proxy for cross-DLL identity probes.
    if (!ppDeviceInOut || !*ppDeviceInOut) return;
    auto* realDevice  = static_cast<ID3D10Device*>(*ppDeviceInOut);
    auto* deviceProxy = new wiz3d::Device10Proxy(realDevice);
    DDILog("WrapD3D10Device: realDevice=%p -> wiz3d::Device10Proxy=%p\n",
           realDevice, deviceProxy);
    *ppDeviceInOut = static_cast<ID3D10Device*>(deviceProxy);
}

extern "C" __declspec(dllexport) void
wiz3D_WrapD3D10SwapChain(void** ppSwapChainInOut, void* pWrappedDevice)
{
    if (!ppSwapChainInOut || !*ppSwapChainInOut) return;
    if (!pWrappedDevice) return;
    if (!gInfo.UseCOMWrapSwapChain)
    {
        DDILog("wiz3D_WrapD3D10SwapChain: UseCOMWrapSwapChain=0 -- passing through unwrapped\n");
        return;
    }
    auto* deviceProxy = reinterpret_cast<wiz3d::Device10Proxy*>(pWrappedDevice);
    auto* realSC      = static_cast<IDXGISwapChain*>(*ppSwapChainInOut);

    // DX10 Factory2 path: CreateSwapChainForHwnd hands us an IDXGISwapChain1
    // directly. QI for it so SwapChain10Proxy can satisfy IDXGISwapChain1
    // surface requests from the game. Failure is fine — Factory1 path returns
    // a plain IDXGISwapChain and the proxy serves E_NOINTERFACE on the 1+
    // methods.
    IDXGISwapChain1* realSC1 = nullptr;
    realSC->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&realSC1));

    auto* scProxy = new wiz3d::SwapChain10Proxy(realSC, realSC1, deviceProxy);
    DDILog("wiz3D_WrapD3D10SwapChain: realSC=%p realSC1=%p -> SwapChain10Proxy=%p (device=%p)\n",
           realSC, realSC1, scProxy, deviceProxy);
    *ppSwapChainInOut = static_cast<IDXGISwapChain*>(scProxy);
}

extern "C" __declspec(dllexport) void
wiz3D_WrapSwapChain(void** ppSwapChainInOut, void* pWrappedDevice)
{
    if (!ppSwapChainInOut || !*ppSwapChainInOut) return;
    if (!pWrappedDevice) return;

    // Stage 4b.3 safety gate: BioShock (UE2.5-era engine) crashes inside the
    // first method call on our SwapChain11Proxy because its rendering code
    // walks swap-chain struct internals past the COM vtable — same TR2013 /
    // Lost Planet / Hitman class of bug iZ3D ran into pre-NvDirectMode. Our
    // proxy's member layout doesn't match the real DXGI swap chain, so the
    // game reads garbage and crashes on ECX=1. Until Stage 4d revisits with
    // either vtable hot-patching or a game-by-game enable, default this off
    // so games that don't need the Present hook (i.e. all of them in
    // Stages 1-3 + 4b) keep working unmodified.
    if (!gInfo.UseCOMWrapSwapChain)
    {
        DDILog("wiz3D_WrapSwapChain: UseCOMWrapSwapChain=0 -- passing through unwrapped (game keeps real SC)\n");
        return;
    }

    // Cast pWrappedDevice back to Device11Proxy. The caller (d3d11.dll
    // proxy's D3D11CreateDeviceAndSwapChain) passes the wrapped device
    // returned from wiz3D_WrapD3D11DeviceAndContext, so this is safe — the
    // pointer IS our proxy.
    auto* deviceProxy = reinterpret_cast<wiz3d::Device11Proxy*>(pWrappedDevice);
    auto* realSC      = static_cast<IDXGISwapChain*>(*ppSwapChainInOut);

    // Try to also get an IDXGISwapChain1 (Win8+ platform). Failure is fine,
    // SwapChain11Proxy is tolerant — it returns E_NOINTERFACE for the 1+
    // methods when the QI didn't succeed.
    IDXGISwapChain1* realSC1 = nullptr;
    realSC->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&realSC1));

    auto* scProxy = new wiz3d::SwapChain11Proxy(realSC, realSC1, deviceProxy);
    DDILog("wiz3D_WrapSwapChain: realSC=%p realSC1=%p -> SwapChain11Proxy=%p (device=%p)\n",
           realSC, realSC1, scProxy, deviceProxy);
    *ppSwapChainInOut = static_cast<IDXGISwapChain*>(scProxy);
}

void wiz3D_EmulateFullscreen(IDXGISwapChain* pSwapChain)
{
    IUnknown* ours = nullptr;
    if (!pSwapChain || FAILED(pSwapChain->QueryInterface(IID_wiz3D_SwapChain11Proxy, reinterpret_cast<void**>(&ours))) || !ours)
        return;
    static_cast<wiz3d::SwapChain11Proxy*>(static_cast<IDXGISwapChain*>(ours))->EmulateFullscreen();
    ours->Release();
}

// DX10 parity companion. Same discovery pattern via QI; the DX10 and DX11
// IIDs are distinct (IID_wiz3D_SwapChain10Proxy vs ..._SwapChain11Proxy)
// so Factory wrap sites call the matching helper per wrapper path.
void wiz3D_EmulateFullscreen10(IDXGISwapChain* pSwapChain)
{
    IUnknown* ours = nullptr;
    if (!pSwapChain || FAILED(pSwapChain->QueryInterface(IID_wiz3D_SwapChain10Proxy, reinterpret_cast<void**>(&ours))) || !ours)
        return;
    static_cast<wiz3d::SwapChain10Proxy*>(static_cast<IDXGISwapChain*>(ours))->EmulateFullscreen();
    ours->Release();
}
