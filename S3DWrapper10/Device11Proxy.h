/* wiz3D - ID3D11Device proxy (Option B Stage 2)
 *
 * Ported from NvDirectMode/d3d11/Device11Proxy by Stage 2 of the DX10/11
 * Option B migration. Passthrough wrapper for ID3D11Device with COM-identity
 * preservation: IDXGIDevice / 1 / 2 / 3 QIs route through DXGIDeviceProxy so
 * `dev->QI(IDXGIDevice)->QI(ID3D11Device)` returns this same wrapper, not the
 * unwrapped real device. Without that round-trip, refcount math diverges and
 * games like Tomb Raider 2013 / BioShock double-release the real device.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9types.h>   // D3DCOLORVALUE for the DXGI_RGBA shim below
#ifndef _DXGI_RGBA_DEFINED
#define _DXGI_RGBA_DEFINED
typedef D3DCOLORVALUE DXGI_RGBA;     // bundled lib/d3d10 dxgitype.h still
                                     // shadows the SDK header for DXGI_RGBA;
                                     // pre-define before d3d11_3.h pulls in
                                     // dxgi1_2.h via its transitive chain.
#endif
#include <d3d11_3.h>   // ID3D11Device3 (inherits Device2/Device1/Device)
#include <unordered_set>
#include <unordered_map>
#include "ShaderAnalyzer11.h"  // ShaderAnalysis11Result

namespace wiz3d
{

class Context11Proxy;
class DXGIDeviceProxy;
class Texture2D11Proxy;

class Device11Proxy : public ID3D11Device3
{
public:
    explicit Device11Proxy(ID3D11Device* real);
    virtual ~Device11Proxy();

    // Set by the dllmain D3D11CreateDevice path right after the immediate
    // context is wrapped, so GetImmediateContext can return that same proxy
    // (with AddRef) instead of producing a different one each call.
    void SetImmediateContextProxy(Context11Proxy* ctxProxy);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef() override                                  { return InterlockedIncrement(&m_refs); }
    ULONG   STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_refs);
        if (r == 0) { if (m_real) { m_real->Release(); m_real = nullptr; } delete this; }
        return (ULONG)r;
    }

    // ID3D11Device
    HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer) override;
    HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D11_TEXTURE1D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture1D** ppTexture1D) override;
    HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D) override;
    HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D11_TEXTURE3D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture3D** ppTexture3D) override;
    HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource* pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, ID3D11ShaderResourceView** ppSRView) override;
    HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource* pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc, ID3D11UnorderedAccessView** ppUAView) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource* pResource, const D3D11_RENDER_TARGET_VIEW_DESC* pDesc, ID3D11RenderTargetView** ppRTView) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource* pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc, ID3D11DepthStencilView** ppDepthStencilView) override;
    HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements, const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D11InputLayout** ppInputLayout) override { return m_real->CreateInputLayout(pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout); }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader) override;
    HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11GeometryShader** ppGeometryShader) override;
    HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void* pShaderBytecode, SIZE_T BytecodeLength, const D3D11_SO_DECLARATION_ENTRY* pSODeclaration, UINT NumEntries, const UINT* pBufferStrides, UINT NumStrides, UINT RasterizedStream, ID3D11ClassLinkage* pClassLinkage, ID3D11GeometryShader** ppGeometryShader) override { return m_real->CreateGeometryShaderWithStreamOutput(pShaderBytecode, BytecodeLength, pSODeclaration, NumEntries, pBufferStrides, NumStrides, RasterizedStream, pClassLinkage, ppGeometryShader); }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader) override           { return m_real->CreatePixelShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader); }
    HRESULT STDMETHODCALLTYPE CreateHullShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11HullShader** ppHullShader) override;
    HRESULT STDMETHODCALLTYPE CreateDomainShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11DomainShader** ppDomainShader) override;
    HRESULT STDMETHODCALLTYPE CreateComputeShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11ComputeShader** ppComputeShader) override     { return m_real->CreateComputeShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppComputeShader); }
    HRESULT STDMETHODCALLTYPE CreateClassLinkage(ID3D11ClassLinkage** ppLinkage) override                                                                                                    { return m_real->CreateClassLinkage(ppLinkage); }
    HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC* pBlendStateDesc, ID3D11BlendState** ppBlendState) override                                                            { return m_real->CreateBlendState(pBlendStateDesc, ppBlendState); }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* pDepthStencilDesc, ID3D11DepthStencilState** ppDepthStencilState) override                             { return m_real->CreateDepthStencilState(pDepthStencilDesc, ppDepthStencilState); }
    HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC* pRasterizerDesc, ID3D11RasterizerState** ppRasterizerState) override                                        { return m_real->CreateRasterizerState(pRasterizerDesc, ppRasterizerState); }
    HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC* pSamplerDesc, ID3D11SamplerState** ppSamplerState) override                                                       { return m_real->CreateSamplerState(pSamplerDesc, ppSamplerState); }
    HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC* pQueryDesc, ID3D11Query** ppQuery) override                                                                                { return m_real->CreateQuery(pQueryDesc, ppQuery); }
    HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D11_QUERY_DESC* pPredicateDesc, ID3D11Predicate** ppPredicate) override                                                                { return m_real->CreatePredicate(pPredicateDesc, ppPredicate); }
    HRESULT STDMETHODCALLTYPE CreateCounter(const D3D11_COUNTER_DESC* pCounterDesc, ID3D11Counter** ppCounter) override                                                                      { return m_real->CreateCounter(pCounterDesc, ppCounter); }
    // Diagnostic: out-of-line in the .cpp so it can DDILog. A game that
    // renders on deferred contexts bypasses Context11Proxy's record-for-
    // replay entirely (we hand back the REAL context), so the right eye
    // never receives those draws. Logging here tells us whether that's
    // happening rather than us inferring it from an empty command stream.
    HRESULT STDMETHODCALLTYPE CreateDeferredContext(UINT ContextFlags, ID3D11DeviceContext** ppDeferredContext) override;
    HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE hResource, REFIID ReturnedInterface, void** ppResource) override                                                                     { return m_real->OpenSharedResource(hResource, ReturnedInterface, ppResource); }
    HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT Format, UINT* pFormatSupport) override                                                                                          { return m_real->CheckFormatSupport(Format, pFormatSupport); }
    HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT Format, UINT SampleCount, UINT* pNumQualityLevels) override                                                          { return m_real->CheckMultisampleQualityLevels(Format, SampleCount, pNumQualityLevels); }
    void    STDMETHODCALLTYPE CheckCounterInfo(D3D11_COUNTER_INFO* pCounterInfo) override                                                                                                    { m_real->CheckCounterInfo(pCounterInfo); }
    HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC* pDesc, D3D11_COUNTER_TYPE* pType, UINT* pActiveCounters, LPSTR szName, UINT* pNameLength, LPSTR szUnits, UINT* pUnitsLength, LPSTR szDescription, UINT* pDescriptionLength) override { return m_real->CheckCounter(pDesc, pType, pActiveCounters, szName, pNameLength, szUnits, pUnitsLength, szDescription, pDescriptionLength); }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D11_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override                                                    { return m_real->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override                                                                                            { return m_real->GetPrivateData(guid, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override                                                                                        { return m_real->SetPrivateData(guid, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override                                                                                          { return m_real->SetPrivateDataInterface(guid, pData); }
    D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override                                                                                                                           { return m_real->GetFeatureLevel(); }
    UINT    STDMETHODCALLTYPE GetCreationFlags() override                                                                                                                                    { return m_real->GetCreationFlags(); }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override                                                                                                                              { return m_real->GetDeviceRemovedReason(); }
    void    STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext** ppImmediateContext) override;
    HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT RaiseFlags) override                                                                                                                     { return m_real->SetExceptionMode(RaiseFlags); }
    UINT    STDMETHODCALLTYPE GetExceptionMode() override                                                                                                                                    { return m_real->GetExceptionMode(); }

    // ----- ID3D11Device1 (D3D11.1) — claim these IIDs in QI with `this` so
    // games that QI for the higher Device versions land on our proxy instead
    // of getting an unwrapped real device handed back (which was the dominant
    // right-eye-breakage bypass identified May 2026). Methods that return
    // contexts are wrapped (return our Context proxy via the higher cast).
    // Resource-creating *1 / *2 / *3 variants currently passthrough — most
    // games use the base CreateTexture2D / CreateRenderTargetView which are
    // already wrapped via the Device-base vtable slots.
    void    STDMETHODCALLTYPE GetImmediateContext1(ID3D11DeviceContext1** ppImmediateContext) override;
    HRESULT STDMETHODCALLTYPE CreateDeferredContext1(UINT ContextFlags, ID3D11DeviceContext1** ppDeferredContext) override;
    HRESULT STDMETHODCALLTYPE CreateBlendState1(const D3D11_BLEND_DESC1* pBlendStateDesc, ID3D11BlendState1** ppBlendState) override                                                        { return m_real1 ? m_real1->CreateBlendState1(pBlendStateDesc, ppBlendState) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE CreateRasterizerState1(const D3D11_RASTERIZER_DESC1* pRasterizerDesc, ID3D11RasterizerState1** ppRasterizerState) override                                    { return m_real1 ? m_real1->CreateRasterizerState1(pRasterizerDesc, ppRasterizerState) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE CreateDeviceContextState(UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, REFIID EmulatedInterface, D3D_FEATURE_LEVEL* pChosenFeatureLevel, ID3DDeviceContextState** ppContextState) override { return m_real1 ? m_real1->CreateDeviceContextState(Flags, pFeatureLevels, FeatureLevels, SDKVersion, EmulatedInterface, pChosenFeatureLevel, ppContextState) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE OpenSharedResource1(HANDLE hResource, REFIID returnedInterface, void** ppResource) override                                                                   { return m_real1 ? m_real1->OpenSharedResource1(hResource, returnedInterface, ppResource) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE OpenSharedResourceByName(LPCWSTR lpName, DWORD dwDesiredAccess, REFIID returnedInterface, void** ppResource) override                                         { return m_real1 ? m_real1->OpenSharedResourceByName(lpName, dwDesiredAccess, returnedInterface, ppResource) : E_NOINTERFACE; }

    // ----- ID3D11Device2 (D3D11.2)
    void    STDMETHODCALLTYPE GetImmediateContext2(ID3D11DeviceContext2** ppImmediateContext) override;
    HRESULT STDMETHODCALLTYPE CreateDeferredContext2(UINT ContextFlags, ID3D11DeviceContext2** ppDeferredContext) override;
    void    STDMETHODCALLTYPE GetResourceTiling(ID3D11Resource* pTiledResource, UINT* pNumTilesForEntireResource, D3D11_PACKED_MIP_DESC* pPackedMipDesc, D3D11_TILE_SHAPE* pStandardTileShapeForNonPackedMips, UINT* pNumSubresourceTilings, UINT FirstSubresourceTilingToGet, D3D11_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) override { if (m_real2) m_real2->GetResourceTiling(pTiledResource, pNumTilesForEntireResource, pPackedMipDesc, pStandardTileShapeForNonPackedMips, pNumSubresourceTilings, FirstSubresourceTilingToGet, pSubresourceTilingsForNonPackedMips); }
    HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels1(DXGI_FORMAT Format, UINT SampleCount, UINT Flags, UINT* pNumQualityLevels) override                                            { return m_real2 ? m_real2->CheckMultisampleQualityLevels1(Format, SampleCount, Flags, pNumQualityLevels) : E_NOINTERFACE; }

    // ----- ID3D11Device3 (D3D11.3)
    HRESULT STDMETHODCALLTYPE CreateTexture2D1(const D3D11_TEXTURE2D_DESC1* pDesc1, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D1** ppTexture2D) override                     { return m_real3 ? m_real3->CreateTexture2D1(pDesc1, pInitialData, ppTexture2D) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE CreateTexture3D1(const D3D11_TEXTURE3D_DESC1* pDesc1, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture3D1** ppTexture3D) override                     { return m_real3 ? m_real3->CreateTexture3D1(pDesc1, pInitialData, ppTexture3D) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE CreateRasterizerState2(const D3D11_RASTERIZER_DESC2* pRasterizerDesc, ID3D11RasterizerState2** ppRasterizerState) override                                    { return m_real3 ? m_real3->CreateRasterizerState2(pRasterizerDesc, ppRasterizerState) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE CreateShaderResourceView1(ID3D11Resource* pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC1* pDesc1, ID3D11ShaderResourceView1** ppSRView1) override          { return m_real3 ? m_real3->CreateShaderResourceView1(pResource, pDesc1, ppSRView1) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView1(ID3D11Resource* pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC1* pDesc1, ID3D11UnorderedAccessView1** ppUAView1) override       { return m_real3 ? m_real3->CreateUnorderedAccessView1(pResource, pDesc1, ppUAView1) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE CreateRenderTargetView1(ID3D11Resource* pResource, const D3D11_RENDER_TARGET_VIEW_DESC1* pDesc1, ID3D11RenderTargetView1** ppRTView1) override                { return m_real3 ? m_real3->CreateRenderTargetView1(pResource, pDesc1, ppRTView1) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE CreateQuery1(const D3D11_QUERY_DESC1* pQueryDesc1, ID3D11Query1** ppQuery1) override                                                                          { return m_real3 ? m_real3->CreateQuery1(pQueryDesc1, ppQuery1) : E_NOINTERFACE; }
    void    STDMETHODCALLTYPE GetImmediateContext3(ID3D11DeviceContext3** ppImmediateContext) override;
    HRESULT STDMETHODCALLTYPE CreateDeferredContext3(UINT ContextFlags, ID3D11DeviceContext3** ppDeferredContext) override;
    void    STDMETHODCALLTYPE WriteToSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) override { if (m_real3) m_real3->WriteToSubresource(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch); }
    void    STDMETHODCALLTYPE ReadFromSubresource(void* pDstData, UINT DstRowPitch, UINT DstDepthPitch, ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox) override { if (m_real3) m_real3->ReadFromSubresource(pDstData, DstRowPitch, DstDepthPitch, pSrcResource, SrcSubresource, pSrcBox); }

    // Accessor for 1b-iii / 1b-iv to plumb through state mutators.
    ID3D11Device*   GetReal()         const { return m_real;     }
    // Stage 4b.2: SwapChain11Proxy::Present needs the immediate context to
    // call its frame-boundary hook (ClearFrameCommands / future replay).
    Context11Proxy* GetContextProxy() const { return m_ctxProxy; }

    // Logical (one-eye) backbuffer size, set by DXGI swap-chain creation /
    // ResizeBuffers. Used by 1b-iv's OMSetRenderTargets viewport routing.
    void SetLogicalBackBufferSize(UINT w, UINT h) { m_logicalWidth = w; m_logicalHeight = h; }
    UINT GetLogicalBackBufferWidth()  const       { return m_logicalWidth; }
    UINT GetLogicalBackBufferHeight() const       { return m_logicalHeight; }

    // 1b-iv back-buffer-RTV identity tracking. The flow is:
    //   SwapChainProxy::GetBuffer(0)             -> RegisterBackBufferTexture
    //   Device11Proxy::CreateRenderTargetView(BB) -> TrackBackBufferRTV
    //   Context11Proxy::OMSetRenderTargets(rtv)   -> IsBackBufferRTV check
    //
    // Stored as void* / raw IUnknown so we don't have to worry about which
    // interface (Texture2D / Resource / DXGISurface) the game requested.
    // Pointer-identity is stable for COM single-inheritance chains.
    void RegisterBackBufferTexture(void* pTextureLike);
    bool IsBackBufferResource(ID3D11Resource* p) const;
    void TrackBackBufferRTV(ID3D11RenderTargetView* rtv);
    bool IsBackBufferRTV(ID3D11RenderTargetView* rtv) const;

    // Stage 4e: record per-shader projection-matrix metadata keyed by the
    // returned ID3D11Vertex/Geometry/Hull/Domain Shader pointer. Stored only
    // when the analyzer actually found one or more projection matrices —
    // mono shaders don't need an entry. Lookup returns nullptr if none.
    void StoreShaderProjection(void* shaderPtr, const ShaderAnalysis11Result& info);
    const ShaderAnalysis11Result* LookupShaderProjection(void* shaderPtr) const;

    // Real-pointer → Texture2D11Proxy lookup map. Games sometimes obtain a
    // raw real ID3D11Resource* from one of our wrapped textures via paths
    // we don't fully wrap (most common: pProxyTex->QI(IDXGIResource) →
    // dxgiRes->QI(ID3D11Resource) returns the m_realLeft pointer). When the
    // game then passes that raw pointer to CreateRenderTargetView, the
    // standard TryUnwrapTexture2D probe fails (the IID-based QI doesn't
    // recognise a real D3D11 texture as one of ours). This map gives us a
    // fallback: register every Texture2D11Proxy's m_realLeft pointer at
    // creation, unregister at destruction; CreateRTV/DSV/SRV/UAV consult
    // the map when the IID probe fails. Closes the De Blob right-eye bypass
    // confirmed via per-frame trace 2026-05-17.
    void RegisterRealToProxy(void* realLeft, Texture2D11Proxy* proxy);
    void UnregisterRealToProxy(void* realLeft);
    Texture2D11Proxy* LookupProxyByReal(void* realLeft) const;

private:
    // Helper for QueryInterface(IDXGIDevice*) — returns a new ref on the
    // cached proxy (creating + caching it on first call). Returns nullptr
    // if the real device doesn't expose any IDXGIDevice (shouldn't happen
    // for a real D3D11 device).
    //
    // Lifetime: the cache holds one strong ref on the proxy until our
    // own destructor runs. That's what closes the resurrection race —
    // a thread can't AddRef the cached proxy "after" it was deleted,
    // because our cache ref keeps it alive as long as the parent device
    // is alive. When the parent device dies, ~Device11Proxy detaches
    // the parent pointer (so any outstanding DXGIDeviceProxy refs the
    // game still holds get E_NOINTERFACE on QI(ID3D11Device)) and then
    // Releases the cache ref.
    DXGIDeviceProxy* GetOrCreateDXGIDeviceProxyAddRef();

    ID3D11Device*    m_real;
    // Cached upgrades of m_real for Device1/2/3 method dispatch. Nullable —
    // populated in the constructor via QueryInterface. Refs released in dtor.
    ID3D11Device1*   m_real1;
    ID3D11Device2*   m_real2;
    ID3D11Device3*   m_real3;
    Context11Proxy*  m_ctxProxy;       // not owned — created in dllmain, freed on Release chain
    DXGIDeviceProxy* m_dxgiDeviceProxy;// strong ref (released in ~Device11Proxy)
    CRITICAL_SECTION m_dxgiCacheLock;
    LONG             m_refs;
    UINT             m_logicalWidth;
    UINT             m_logicalHeight;

    // 1b-iv state. We don't AddRef any of these — game manages lifetimes;
    // we just identity-track. A released-and-reallocated pointer would be
    // a stale-positive worst case (game gets per-eye routing on a non-BB
    // RTV, harmless — only the viewport changes, no crash).
    void*            m_pBackBufferResource;
    std::unordered_set<ID3D11RenderTargetView*> m_backBufferRTVs;
    CRITICAL_SECTION m_rtvSetLock;

    // Stage 4e: shader -> projection-matrix info map. Game-side shader
    // pointers (not addref'd — game owns lifetime). Stale-entry risk is
    // bounded: if a game releases shader X then creates a new one at the
    // same address, the cached entry may apply to the new shader; harmless
    // (we'd just be looking for projection matrices that aren't there or
    // are at different registers — at worst we skip per-eye stereo for
    // that draw, which falls back to mono).
    std::unordered_map<void*, ShaderAnalysis11Result> m_shaderProjections;
    CRITICAL_SECTION                                  m_shaderProjLock;

    // Real-pointer → Texture2D11Proxy fallback. See accessor doc above. Game
    // owns the real-pointer lifetime via the proxy; entry is unregistered in
    // ~Texture2D11Proxy before the real Release fires.
    std::unordered_map<void*, Texture2D11Proxy*>      m_realToProxy;
    CRITICAL_SECTION                                  m_realToProxyLock;
};

} // namespace wiz3d
