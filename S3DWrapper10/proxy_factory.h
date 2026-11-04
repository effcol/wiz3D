/* wiz3D d3d11 - COM-proxy factory bridge between the d3d11.dll proxy and the
 * Device11Proxy / Context11Proxy classes. Inputs/outputs typed as void* so the
 * d3d11.dll proxy TU stays free of d3d11.h (system header re-declares
 * D3D11CreateDevice without __declspec(dllexport), conflicting with our
 * exports). Mirrors NvDirectMode/d3d11/proxy_factory.h's role for the iZ3D
 * stereo wrapper Option B port.
 */

#pragma once

#include <guiddef.h>

// Private IID claimed by Device11Proxy::QueryInterface so other DLLs can
// detect "is this a wiz3D-wrapped device?" via the standard COM equality path
// without sharing any C++ symbols. The wiz3D dxgi.dll proxy declares its own
// const GUID with the same value and matches on memcmp.
EXTERN_C const GUID IID_wiz3D_Device11Proxy;

// DX10 Option B: same identity pattern as Device11Proxy but for ID3D10Device.
// The DXGIFactoryWrapper's CreateSwapChain hook QIs incoming pDevice for this
// IID so a DX10 game taking the factory two-call path also gets its swap
// chain wrapped post-Original_CreateSwapChain.
EXTERN_C const GUID IID_wiz3D_Device10Proxy;
EXTERN_C const GUID IID_wiz3D_Texture2D10Proxy;
EXTERN_C const GUID IID_wiz3D_RTV10Proxy;
EXTERN_C const GUID IID_wiz3D_DSV10Proxy;
EXTERN_C const GUID IID_wiz3D_Buffer10Proxy;
EXTERN_C const GUID IID_wiz3D_SwapChain10Proxy;
// Stage 3c.2 DX10 port: SRV + Texture1D/3D proxies. DX10 has no UAVs
// (that's a DX11 addition), so no UAV10Proxy.
EXTERN_C const GUID IID_wiz3D_SRV10Proxy;
EXTERN_C const GUID IID_wiz3D_Texture1D10Proxy;
EXTERN_C const GUID IID_wiz3D_Texture3D10Proxy;

// Stage 3b: same pattern for the resource/view proxies. Methods that take an
// `ID3D11Resource*` / `ID3D11RenderTargetView*` / `ID3D11DepthStencilView*`
// at the COM boundary check for these IIDs to detect a wiz3D proxy at their
// input. If positive, the wrap call extracts the real underlying pointer (and
// the right-eye sibling if any) before forwarding to the real D3D11 runtime —
// the runtime would crash if handed our proxy directly since it doesn't
// understand the proxy's vtable layout beyond ID3D11* method signatures.
EXTERN_C const GUID IID_wiz3D_Texture2D11Proxy;
EXTERN_C const GUID IID_wiz3D_RTV11Proxy;
EXTERN_C const GUID IID_wiz3D_DSV11Proxy;
EXTERN_C const GUID IID_wiz3D_SwapChain11Proxy;
// Stage 3c.1: Buffer11Proxy private IID. Buffers aren't stereo-doubled
// (no right-eye sibling) but identity wrapping still matters for the 4c
// eye-shift heuristic, which uses a tag bit on the wrapped buffer to
// decide whether to apply per-eye CB modification.
EXTERN_C const GUID IID_wiz3D_Buffer11Proxy;

// Stage 3c.2: identity IIDs for SRV/UAV/Tex1D/Tex3D proxies. SRV/UAV may
// carry a right-eye sibling (for stereo-doubled resources) so 4e can route
// per-eye binding via *SetShaderResources / CSSetUnorderedAccessViews.
// Tex1D/Tex3D are pure passthrough (no stereo) but still need identity
// wrapping so SRV/UAV creation paths can hand the real resource to the
// runtime instead of our proxy.
EXTERN_C const GUID IID_wiz3D_SRV11Proxy;
EXTERN_C const GUID IID_wiz3D_UAV11Proxy;
EXTERN_C const GUID IID_wiz3D_Texture1D11Proxy;
EXTERN_C const GUID IID_wiz3D_Texture3D11Proxy;

namespace wiz3d
{
    // Wraps the device + (optional) immediate context the system
    // D3D11CreateDevice path produced. Both inputs and outputs are passed
    // as void* so the d3d11.dll dllmain TU stays away from d3d11.h.
    //
    // *ppDeviceInOut    : in/out — system device on entry, Device11Proxy on exit
    // *ppContextInOut   : in/out — system immediate context on entry,
    //                     Context11Proxy on exit. May be NULL if the game
    //                     didn't ask for the context.
    void WrapD3D11DeviceAndContext(void** ppDeviceInOut, void** ppContextInOut);

    // Stage 3b: identify whether a COM pointer the game just handed us is one
    // of our wiz3D proxies. Returns the proxy interface pointer (still
    // AddRef'd by the caller's reference) on hit, nullptr if not ours. The
    // caller does NOT receive a new ref — just identity. Implemented inline
    // so any TU can use them without linking against the proxy classes.
    //
    // Usage pattern at a method entry:
    //   ID3D11Resource* realIn = pResource;
    //   if (auto* tex = TryUnwrapTexture2D(pResource)) realIn = UnwrapRealLeft(tex);
    //   m_real->SomeMethod(realIn, ...);
    //
    // The helpers QueryInterface the input for our private IID; on hit they
    // Release the QI'd ref (we don't need it, only the identity match) and
    // reinterpret_cast back to the proxy. Same pattern NvDirectMode uses for
    // IID_NvDM_Device11Proxy.

    // Forward-declarations so callers don't have to include the proxy headers
    // just to call the unwrap helpers. The .cpp side does include them.
    class Texture2D11Proxy;
    class RTV11Proxy;
    class DSV11Proxy;
    class Buffer11Proxy;

    Texture2D11Proxy* TryUnwrapTexture2D(struct ID3D11Resource* p);
    RTV11Proxy*       TryUnwrapRTV(struct ID3D11RenderTargetView* p);
    DSV11Proxy*       TryUnwrapDSV(struct ID3D11DepthStencilView* p);
    Buffer11Proxy*    TryUnwrapBuffer(struct ID3D11Resource* p);

    // Stage 3c.2 unwrap helpers.
    class SRV11Proxy;
    class UAV11Proxy;
    class Texture1D11Proxy;
    class Texture3D11Proxy;

    SRV11Proxy*       TryUnwrapSRV(struct ID3D11ShaderResourceView* p);
    UAV11Proxy*       TryUnwrapUAV(struct ID3D11UnorderedAccessView* p);
    Texture1D11Proxy* TryUnwrapTexture1D(struct ID3D11Resource* p);
    Texture3D11Proxy* TryUnwrapTexture3D(struct ID3D11Resource* p);

    // DX10 unwrap helpers — same memcmp-on-IID identity pattern as DX11.
    class Texture2D10Proxy;
    class RTV10Proxy;
    class DSV10Proxy;
    class Buffer10Proxy;

    Texture2D10Proxy* TryUnwrapTexture2D_10(struct ID3D10Resource* p);
    RTV10Proxy*       TryUnwrapRTV_10(struct ID3D10RenderTargetView* p);
    DSV10Proxy*       TryUnwrapDSV_10(struct ID3D10DepthStencilView* p);
    Buffer10Proxy*    TryUnwrapBuffer_10(struct ID3D10Resource* p);

    class SRV10Proxy;
    class Texture1D10Proxy;
    class Texture3D10Proxy;

    SRV10Proxy*       TryUnwrapSRV_10(struct ID3D10ShaderResourceView* p);
    Texture1D10Proxy* TryUnwrapTexture1D_10(struct ID3D10Resource* p);
    Texture3D10Proxy* TryUnwrapTexture3D_10(struct ID3D10Resource* p);
}

// Exported entry point the d3d11.dll proxy resolves via GetProcAddress, so
// the existing wiz3D-proxy/d3d11/dllmain doesn't need to be linked against
// S3DWrapperD3D10.lib directly. Uses __cdecl (the default) rather than WINAPI
// so the exported symbol name matches across x86/x64 — __stdcall would
// decorate to _name@N on x86 and break the bare-name def-file entry.
extern "C" __declspec(dllexport) void
wiz3D_WrapD3D11DeviceAndContext(void** ppDeviceInOut, void** ppContextInOut);

// Stage 4b.3: companion export for D3D11CreateDeviceAndSwapChain's swap
// chain output. Called AFTER wiz3D_WrapD3D11DeviceAndContext has run, so
// *ppDeviceInOut is already our Device11Proxy. Wraps the real swap chain
// in SwapChain11Proxy and substitutes it in-place. No-op when either
// pointer is null. Pure passthrough for the eventual frame-boundary
// callback (4b.4 onward): present-time frame-end hook + 4d's SBS composite.
extern "C" __declspec(dllexport) void
wiz3D_WrapSwapChain(void** ppSwapChainInOut, void* pWrappedDevice);

// The factory hooks created this chain windowed though the game asked for fullscreen: a SwapChain11Proxy then
// reports fullscreen to the game. No-op for any other swap chain.
void wiz3D_EmulateFullscreen(struct IDXGISwapChain* pSwapChain);
// DX10 parity companion -- same semantics but for SwapChain10Proxy.
void wiz3D_EmulateFullscreen10(struct IDXGISwapChain* pSwapChain);

// DX10 Option B: wraps the ID3D10Device returned from D3D10CreateDevice /
// D3D10CreateDeviceAndSwapChain in our Device10Proxy. Pure passthrough at
// Stage 1 of the DX10 port; later stages add per-eye routing + composite.
extern "C" __declspec(dllexport) void
wiz3D_WrapD3D10Device(void** ppDeviceInOut);

// DX10 Stage 4d: companion to wiz3D_WrapSwapChain for the DX10 path. Wraps
// the IDXGISwapChain returned by D3D10CreateDeviceAndSwapChain in our
// SwapChain10Proxy. Called from the d3d10 dllmain after the device has
// already been wrapped via wiz3D_WrapD3D10Device. The factory hook in
// DXGIFactoryWrapper.cpp invokes this for the two-call path too.
extern "C" __declspec(dllexport) void
wiz3D_WrapD3D10SwapChain(void** ppSwapChainInOut, void* pWrappedDevice);
