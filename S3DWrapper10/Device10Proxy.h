/* wiz3D - ID3D10Device proxy (Option B for DX10)
 *
 * D3D10 has no separate device/context split — ID3D10Device handles both
 * resource creation and state/draws on a single interface. So this single
 * class is the d3d10 equivalent of d3d11's Device11Proxy + Context11Proxy
 * combined.
 *
 * Stage 1 of the DX10 port: passthrough wrap so games on FC2 / JC2 / De Blob
 * land on the Option B path instead of the legacy DDI hooks (which break on
 * Win11 D3D11.10 layouts). Future stages will add per-eye routing, replay
 * sweep, SBS composite, etc. — mirroring what the DX11 side already has.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d10.h>
#include <functional>
#include <vector>
#include <unordered_map>
#include "ShaderAnalyzer11.h"  // ShaderAnalysis11Result — DXBC analyzer works for SM4 (DX10) too

class ShaderProfileData;  // ::ShaderProfileData (global namespace) — used by WhenFilterMatches10

namespace wiz3d
{

class Buffer10Proxy;
class DXGIDevice10Proxy;

// RAII COM-pointer holder for closure captures. Same shape as the DX11
// Context11Proxy's ComRefHolder — keeps a reference alive across the
// frame even if the game releases its own.
struct ComRefHolder10
{
    IUnknown* p;
    ComRefHolder10() : p(nullptr) {}
    explicit ComRefHolder10(IUnknown* x) : p(x) { if (p) p->AddRef(); }
    ComRefHolder10(const ComRefHolder10& o) : p(o.p) { if (p) p->AddRef(); }
    ComRefHolder10(ComRefHolder10&& o) noexcept : p(o.p) { o.p = nullptr; }
    ComRefHolder10& operator=(const ComRefHolder10& o)
    {
        if (this != &o) { if (p) p->Release(); p = o.p; if (p) p->AddRef(); }
        return *this;
    }
    ComRefHolder10& operator=(ComRefHolder10&& o) noexcept
    {
        if (this != &o) { if (p) p->Release(); p = o.p; o.p = nullptr; }
        return *this;
    }
    ~ComRefHolder10() { if (p) p->Release(); }
};

class Device10Proxy : public ID3D10Device
{
public:
    explicit Device10Proxy(ID3D10Device* real);
    virtual ~Device10Proxy();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef() override                                  { return InterlockedIncrement(&m_refs); }
    ULONG   STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_refs);
        if (r == 0) { if (m_real) { m_real->Release(); m_real = nullptr; } delete this; }
        return (ULONG)r;
    }

    // ID3D10Device — input assembler / shader stages / rasteriser / OM / draws
    void STDMETHODCALLTYPE VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D10Buffer* const* ppConstantBuffers) override;
    void STDMETHODCALLTYPE PSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D10ShaderResourceView* const* ppShaderResourceViews) override;
    void STDMETHODCALLTYPE PSSetShader(ID3D10PixelShader* pPixelShader) override;
    void STDMETHODCALLTYPE PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D10SamplerState* const* ppSamplers) override;
    void STDMETHODCALLTYPE VSSetShader(ID3D10VertexShader* pVertexShader) override;
    void STDMETHODCALLTYPE DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) override;
    void STDMETHODCALLTYPE Draw(UINT VertexCount, UINT StartVertexLocation) override;
    void STDMETHODCALLTYPE PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D10Buffer* const* ppConstantBuffers) override;
    void STDMETHODCALLTYPE IASetInputLayout(ID3D10InputLayout* pInputLayout) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D10Buffer* const* ppVertexBuffers, const UINT* pStrides, const UINT* pOffsets) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(ID3D10Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset) override;
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D10Buffer* const* ppConstantBuffers) override;
    void STDMETHODCALLTYPE GSSetShader(ID3D10GeometryShader* pShader) override;
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY Topology) override;
    void STDMETHODCALLTYPE VSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D10ShaderResourceView* const* ppShaderResourceViews) override;
    void STDMETHODCALLTYPE VSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D10SamplerState* const* ppSamplers) override;
    void STDMETHODCALLTYPE SetPredication(ID3D10Predicate* pPredicate, BOOL PredicateValue) override                                                                                               { m_real->SetPredication(pPredicate, PredicateValue); }
    void STDMETHODCALLTYPE GSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D10ShaderResourceView* const* ppShaderResourceViews) override;
    void STDMETHODCALLTYPE GSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D10SamplerState* const* ppSamplers) override;
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumViews, ID3D10RenderTargetView* const* ppRenderTargetViews, ID3D10DepthStencilView* pDepthStencilView) override;
    void STDMETHODCALLTYPE OMSetBlendState(ID3D10BlendState* pBlendState, const FLOAT BlendFactor[4], UINT SampleMask) override;
    void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D10DepthStencilState* pDepthStencilState, UINT StencilRef) override;
    void STDMETHODCALLTYPE SOSetTargets(UINT NumBuffers, ID3D10Buffer* const* ppSOTargets, const UINT* pOffsets) override;
    void STDMETHODCALLTYPE DrawAuto() override;
    void STDMETHODCALLTYPE RSSetState(ID3D10RasterizerState* pRasterizerState) override;
    void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D10_VIEWPORT* pViewports) override;
    void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D10_RECT* pRects) override;
    void STDMETHODCALLTYPE CopySubresourceRegion(ID3D10Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D10Resource* pSrcResource, UINT SrcSubresource, const D3D10_BOX* pSrcBox) override;
    void STDMETHODCALLTYPE CopyResource(ID3D10Resource* pDstResource, ID3D10Resource* pSrcResource) override;
    void STDMETHODCALLTYPE UpdateSubresource(ID3D10Resource* pDstResource, UINT DstSubresource, const D3D10_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) override;
    void STDMETHODCALLTYPE ClearRenderTargetView(ID3D10RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4]) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(ID3D10DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil) override;
    void STDMETHODCALLTYPE GenerateMips(ID3D10ShaderResourceView* pShaderResourceView) override;
    void STDMETHODCALLTYPE ResolveSubresource(ID3D10Resource* pDstResource, UINT DstSubresource, ID3D10Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override;
    void STDMETHODCALLTYPE VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D10Buffer** ppConstantBuffers) override                                                                        { m_real->VSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE PSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D10ShaderResourceView** ppShaderResourceViews) override                                                          { m_real->PSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE PSGetShader(ID3D10PixelShader** ppPixelShader) override                                                                                                                 { m_real->PSGetShader(ppPixelShader); }
    void STDMETHODCALLTYPE PSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D10SamplerState** ppSamplers) override                                                                               { m_real->PSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE VSGetShader(ID3D10VertexShader** ppVertexShader) override                                                                                                               { m_real->VSGetShader(ppVertexShader); }
    void STDMETHODCALLTYPE PSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D10Buffer** ppConstantBuffers) override                                                                        { m_real->PSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE IAGetInputLayout(ID3D10InputLayout** ppInputLayout) override                                                                                                            { m_real->IAGetInputLayout(ppInputLayout); }
    void STDMETHODCALLTYPE IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D10Buffer** ppVertexBuffers, UINT* pStrides, UINT* pOffsets) override                                            { m_real->IAGetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets); }
    void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D10Buffer** pIndexBuffer, DXGI_FORMAT* Format, UINT* Offset) override                                                                               { m_real->IAGetIndexBuffer(pIndexBuffer, Format, Offset); }
    void STDMETHODCALLTYPE GSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D10Buffer** ppConstantBuffers) override                                                                        { m_real->GSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE GSGetShader(ID3D10GeometryShader** ppGeometryShader) override                                                                                                           { m_real->GSGetShader(ppGeometryShader); }
    void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY* pTopology) override                                                                                                    { m_real->IAGetPrimitiveTopology(pTopology); }
    void STDMETHODCALLTYPE VSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D10ShaderResourceView** ppShaderResourceViews) override                                                          { m_real->VSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE VSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D10SamplerState** ppSamplers) override                                                                               { m_real->VSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE GetPredication(ID3D10Predicate** ppPredicate, BOOL* pPredicateValue) override                                                                                           { m_real->GetPredication(ppPredicate, pPredicateValue); }
    void STDMETHODCALLTYPE GSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D10ShaderResourceView** ppShaderResourceViews) override                                                          { m_real->GSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE GSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D10SamplerState** ppSamplers) override                                                                               { m_real->GSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE OMGetRenderTargets(UINT NumViews, ID3D10RenderTargetView** ppRenderTargetViews, ID3D10DepthStencilView** ppDepthStencilView) override                                   { m_real->OMGetRenderTargets(NumViews, ppRenderTargetViews, ppDepthStencilView); }
    void STDMETHODCALLTYPE OMGetBlendState(ID3D10BlendState** ppBlendState, FLOAT BlendFactor[4], UINT* pSampleMask) override                                                                      { m_real->OMGetBlendState(ppBlendState, BlendFactor, pSampleMask); }
    void STDMETHODCALLTYPE OMGetDepthStencilState(ID3D10DepthStencilState** ppDepthStencilState, UINT* pStencilRef) override                                                                       { m_real->OMGetDepthStencilState(ppDepthStencilState, pStencilRef); }
    void STDMETHODCALLTYPE SOGetTargets(UINT NumBuffers, ID3D10Buffer** ppSOTargets, UINT* pOffsets) override                                                                                      { m_real->SOGetTargets(NumBuffers, ppSOTargets, pOffsets); }
    void STDMETHODCALLTYPE RSGetState(ID3D10RasterizerState** ppRasterizerState) override                                                                                                          { m_real->RSGetState(ppRasterizerState); }
    void STDMETHODCALLTYPE RSGetViewports(UINT* NumViewports, D3D10_VIEWPORT* pViewports) override                                                                                                 { m_real->RSGetViewports(NumViewports, pViewports); }
    void STDMETHODCALLTYPE RSGetScissorRects(UINT* NumRects, D3D10_RECT* pRects) override                                                                                                          { m_real->RSGetScissorRects(NumRects, pRects); }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override                                                                                                                                    { return m_real->GetDeviceRemovedReason(); }
    HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT RaiseFlags) override                                                                                                                           { return m_real->SetExceptionMode(RaiseFlags); }
    UINT STDMETHODCALLTYPE GetExceptionMode() override                                                                                                                                             { return m_real->GetExceptionMode(); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override                                                                                                  { return m_real->GetPrivateData(guid, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override                                                                                              { return m_real->SetPrivateData(guid, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override                                                                                                { return m_real->SetPrivateDataInterface(guid, pData); }
    void STDMETHODCALLTYPE ClearState() override                                                                                                                                                   { m_real->ClearState(); }
    void STDMETHODCALLTYPE Flush() override                                                                                                                                                        { m_real->Flush(); }
    HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D10_BUFFER_DESC* pDesc, const D3D10_SUBRESOURCE_DATA* pInitialData, ID3D10Buffer** ppBuffer) override;
    HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D10_TEXTURE1D_DESC* pDesc, const D3D10_SUBRESOURCE_DATA* pInitialData, ID3D10Texture1D** ppTexture1D) override;
    HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D10_TEXTURE2D_DESC* pDesc, const D3D10_SUBRESOURCE_DATA* pInitialData, ID3D10Texture2D** ppTexture2D) override;
    HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D10_TEXTURE3D_DESC* pDesc, const D3D10_SUBRESOURCE_DATA* pInitialData, ID3D10Texture3D** ppTexture3D) override;
    HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D10Resource* pResource, const D3D10_SHADER_RESOURCE_VIEW_DESC* pDesc, ID3D10ShaderResourceView** ppSRView) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D10Resource* pResource, const D3D10_RENDER_TARGET_VIEW_DESC* pDesc, ID3D10RenderTargetView** ppRTView) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D10Resource* pResource, const D3D10_DEPTH_STENCIL_VIEW_DESC* pDesc, ID3D10DepthStencilView** ppDepthStencilView) override;
    HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D10_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements, const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D10InputLayout** ppInputLayout) override { return m_real->CreateInputLayout(pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout); }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D10VertexShader** ppVertexShader) override;
    HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D10GeometryShader** ppGeometryShader) override;
    HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void* pShaderBytecode, SIZE_T BytecodeLength, const D3D10_SO_DECLARATION_ENTRY* pSODeclaration, UINT NumEntries, UINT OutputStreamStride, ID3D10GeometryShader** ppGeometryShader) override;
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D10PixelShader** ppPixelShader) override                                                     { return m_real->CreatePixelShader(pShaderBytecode, BytecodeLength, ppPixelShader); }
    HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D10_BLEND_DESC* pBlendStateDesc, ID3D10BlendState** ppBlendState) override                                                                   { return m_real->CreateBlendState(pBlendStateDesc, ppBlendState); }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D10_DEPTH_STENCIL_DESC* pDepthStencilDesc, ID3D10DepthStencilState** ppDepthStencilState) override                                    { return m_real->CreateDepthStencilState(pDepthStencilDesc, ppDepthStencilState); }
    HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D10_RASTERIZER_DESC* pRasterizerDesc, ID3D10RasterizerState** ppRasterizerState) override                                               { return m_real->CreateRasterizerState(pRasterizerDesc, ppRasterizerState); }
    HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D10_SAMPLER_DESC* pSamplerDesc, ID3D10SamplerState** ppSamplerState) override                                                              { return m_real->CreateSamplerState(pSamplerDesc, ppSamplerState); }
    HRESULT STDMETHODCALLTYPE CreateQuery(const D3D10_QUERY_DESC* pQueryDesc, ID3D10Query** ppQuery) override                                                                                       { return m_real->CreateQuery(pQueryDesc, ppQuery); }
    HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D10_QUERY_DESC* pPredicateDesc, ID3D10Predicate** ppPredicate) override                                                                       { return m_real->CreatePredicate(pPredicateDesc, ppPredicate); }
    HRESULT STDMETHODCALLTYPE CreateCounter(const D3D10_COUNTER_DESC* pCounterDesc, ID3D10Counter** ppCounter) override                                                                             { return m_real->CreateCounter(pCounterDesc, ppCounter); }
    HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT Format, UINT* pFormatSupport) override                                                                                                { return m_real->CheckFormatSupport(Format, pFormatSupport); }
    HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT Format, UINT SampleCount, UINT* pNumQualityLevels) override                                                                { return m_real->CheckMultisampleQualityLevels(Format, SampleCount, pNumQualityLevels); }
    void    STDMETHODCALLTYPE CheckCounterInfo(D3D10_COUNTER_INFO* pCounterInfo) override                                                                                                          { m_real->CheckCounterInfo(pCounterInfo); }
    HRESULT STDMETHODCALLTYPE CheckCounter(const D3D10_COUNTER_DESC* pDesc, D3D10_COUNTER_TYPE* pType, UINT* pActiveCounters, LPSTR szName, UINT* pNameLength, LPSTR szUnits, UINT* pUnitsLength, LPSTR szDescription, UINT* pDescriptionLength) override { return m_real->CheckCounter(pDesc, pType, pActiveCounters, szName, pNameLength, szUnits, pUnitsLength, szDescription, pDescriptionLength); }
    UINT STDMETHODCALLTYPE GetCreationFlags() override                                                                                                                                             { return m_real->GetCreationFlags(); }
    HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE hResource, REFIID ReturnedInterface, void** ppResource) override                                                                            { return m_real->OpenSharedResource(hResource, ReturnedInterface, ppResource); }
    void STDMETHODCALLTYPE SetTextFilterSize(UINT Width, UINT Height) override                                                                                                                     { m_real->SetTextFilterSize(Width, Height); }
    void STDMETHODCALLTYPE GetTextFilterSize(UINT* pWidth, UINT* pHeight) override                                                                                                                 { m_real->GetTextFilterSize(pWidth, pHeight); }

    ID3D10Device* GetReal() const { return m_real; }

    // DX10 Stage 4a equivalent: active-eye state controls which real
    // RTV/DSV/RT the Do* helpers bind.
    enum class Eye { Left = 0, Right = 1 };
    void SetActiveEye(Eye e) { m_activeEye = e; }
    Eye  GetActiveEye() const { return m_activeEye; }

    void SetLogicalBackBufferSize(UINT w, UINT h) { m_logicalWidth = w; m_logicalHeight = h; }
    UINT GetLogicalBackBufferWidth()  const       { return m_logicalWidth; }
    UINT GetLogicalBackBufferHeight() const       { return m_logicalHeight; }

    // DX10 Stage 4b: replay infrastructure mirroring Context11Proxy. The
    // SwapChain10Proxy fires the Pre-Present hook which calls
    // ReplayFrameCommands(Right) on the device, then Post-Present clears
    // and arms the recorder for the next frame.
    void ClearFrameCommands();
    void ReplayFrameCommands(Eye eye);
    void SetPresentHookActive(bool active) { m_presentHookActive = active; }
    bool IsPresentHookActive() const       { return m_presentHookActive; }
    // 4c10: Buffer10Proxy::Unmap pushes Map+memcpy+eye-shift+Unmap
    // replay closures through this entry point so the per-eye CB math
    // applies on the right-eye replay sweep.
    void PushFrameCommand(std::function<void()> fn) { m_frameCommands.emplace_back(std::move(fn)); }
    Eye  ActiveEye()      const { return m_activeEye; }

    // DX10 DXGIDevice identity loop: lazily creates and caches a wrapped
    // DXGIDevice10Proxy so the game's QI(IDXGIDevice) → QI(ID3D10Device)
    // round-trip lands back on this Device10Proxy. Without it, the runtime
    // hands the unwrapped real device to factory->CreateSwapChain and our
    // factory hook misses (FC2 / JC2 stay mono).
    DXGIDevice10Proxy* GetOrCreateDXGIDeviceProxyAddRef();

    // Stage 4e DX10 port: analyzer cache + VS-binding snapshot. DX10 has
    // no separate context so the binding state lives on the device itself.
    // Buffer10Proxy::Unmap consults LookupShaderProjection(m_boundVS) +
    // walks m_boundVSCBs to build a precise matrix-target list per CB.
    static constexpr UINT kMaxVSCBSlots = 14;  // D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
    void StoreShaderProjection(void* shaderPtr, const ShaderAnalysis11Result& info);
    const ShaderAnalysis11Result* LookupShaderProjection(void* shaderPtr) const;
    ID3D10VertexShader* GetBoundVS() const { return m_boundVS; }
    ID3D10Buffer*       GetBoundVSCB(UINT slot) const
    {
        return slot < kMaxVSCBSlots ? m_boundVSCBs[slot] : nullptr;
    }

    // ==========================================================================
    // DX10 Option A (task #62 cycles 2+3): per-draw right-eye edit + primitive
    // engine. Mirrors Context11Proxy's binding state so the shader primitives
    // (<SkipStereo>, <FixedDepth>, <ShiftUV>, <DepthFromCB>, <AnimateDepth>,
    // <OverrideCBValue>, <PerEyeTexture>, <SpriteDepth>, <ParallaxLayer>,
    // <When TextureCRC>) + ShaderHunter note-draw can fire on DX10 games.
    //
    // DX10 has three programmable stages: VS, GS, PS (no HS/DS/CS).
    // ==========================================================================
    enum StageIdx10 { ST10_VS = 0, ST10_GS = 1, ST10_PS = 2, ST10_COUNT = 3 };
    static constexpr UINT kMaxSRVSlots10 = 128; // D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT

    // Per-stage bound-shader analysis (filled at *SetShader); null = not bound.
    const ShaderAnalysis11Result* GetBoundInfo(StageIdx10 st) const
    { return (st < ST10_COUNT) ? m_boundInfo[st] : nullptr; }

    // Per-stage CB slots. Stage 4e already tracked VS; cycle 2 extends to GS + PS.
    Buffer10Proxy* GetCBSlot(StageIdx10 st, UINT slot) const
    {
        if (st >= ST10_COUNT || slot >= kMaxVSCBSlots) return nullptr;
        return m_cbSlots[st][slot];
    }

    // Per-stage SRV slots (eye-swap substitution happens here for PerEyeTexture).
    ID3D10ShaderResourceView* GetSRVSlot(StageIdx10 st, UINT slot) const
    {
        if (st >= ST10_COUNT || slot >= kMaxSRVSlots10) return nullptr;
        return m_srvSlots[st][slot];
    }

    // Per-draw right-eye edit entry point. Mirrors Context11Proxy::EditRightCBs
    // minus the 37-feature dispatch (features remain DX11-only in cycle 2).
    // Walks each stage's rightCBs list; for each noted CB, writes shifted bytes
    // into Buffer10Proxy::RightCopy(0) and rebinds it on the right-eye draw.
    void EditRightCBs10();

    // Snapshot of per-draw state Option A needs during right-eye replay.
    // Captured at Draw-record time in the game's thread; applied at replay.
    // Buffer10Proxy refs are AddRef'd in the capture + Released on snapshot
    // destruction so the proxy stays alive through replay even if the game
    // released its binding between frames.
    struct RightCBSnap10
    {
        int            stage = ST10_VS;
        UINT           slot  = 0;
        Buffer10Proxy* bp    = nullptr;  // AddRef'd below
    };
    struct DrawSnapshot10
    {
        bool                        skipStereo = false;
        std::vector<RightCBSnap10>  rightCBs;
        ComRefHolder10              vsHolder;     // keep shader analyses alive -- technically
        ComRefHolder10              psHolder;     // the pointers come from m_shaderProjections
        ComRefHolder10              gsHolder;     // (device-lifetime), but keep the shader object
                                                  // addref'd to satisfy the game's reference
                                                  // counting contract during replay.
        std::vector<ComRefHolder10> cbHolders;    // one holder per entry in rightCBs
    };
    DrawSnapshot10 CaptureDrawSnapshot10();
    void           ApplySnapshotRightCBs10(const DrawSnapshot10& snap);

    // Primitive engine dispatch (DXBC twin binds). Called from the pre-draw
    // hook on the right-eye pass. Returns true if a twin was bound and the
    // caller should UnbindTwin10/UnbindClipSpaceShift10 after the real draw.
    bool BindClipSpaceShift10();
    void UnbindClipSpaceShift10();

    // Shader primitives (one per XML tag). Each returns true if it applied
    // a twin or substitution for this draw.
    bool BindSkipStereoGate(bool& skipRightEye);
    bool BindFixedDepth10(const ShaderAnalysis11Result* info, StageIdx10 st, float depth);
    bool BindShiftUV10(const ShaderAnalysis11Result* info, StageIdx10 st,
                       int outputReg, int component, float amount);
    bool BindDepthFromCB10(const ShaderAnalysis11Result* info, StageIdx10 st,
                           int cbSlot, int offsetBytes);
    bool BindAnimateDepth10(const ShaderAnalysis11Result* info, StageIdx10 st,
                            int cbSlot, int offsetBytes, float start, float end);
    bool BindOverrideCBValue10(const ShaderAnalysis11Result* info, StageIdx10 st,
                               int cbSlot, int offsetBytes, float value);
    bool BindPerEyeTexture10(const ShaderAnalysis11Result* info, StageIdx10 st,
                             int slot, DWORD leftCRC, DWORD rightCRC);

    // <When TextureCRC="..."> sub-filter for shader primitives.
    bool WhenFilterMatches10(const ::ShaderProfileData& p);

    // Scan currently-bound PS SRVs by FNV-1a 32-bit hash over the
    // ID3D10Resource* pointer. Up to 16 hashes written; count out.
    void PsBoundResourceHashes10(uint32_t out[16], unsigned& count);

    // Hunter integration -- called after NoteDraw at each real draw site.
    void HuntMaybeCaptureFingerprint10();

private:
    ID3D10Device*      m_real;
    LONG               m_refs;
    Eye                m_activeEye;
    UINT               m_logicalWidth;
    UINT               m_logicalHeight;
    bool               m_presentHookActive;
    std::vector<std::function<void()>> m_frameCommands;
    DXGIDevice10Proxy* m_dxgiDeviceProxy;
    CRITICAL_SECTION   m_dxgiCacheLock;

    // Stage 4e DX10: shader -> projection info map, lock-protected. Same
    // stale-entry contract as Device11Proxy::m_shaderProjections — game
    // owns shader lifetimes, we don't AddRef.
    std::unordered_map<void*, ShaderAnalysis11Result> m_shaderProjections;
    CRITICAL_SECTION                                  m_shaderProjLock;

    // VS-binding snapshot. DX10 has 14 CB slots per stage. Used by
    // Buffer10Proxy::Unmap to ask which slot the mapped CB is bound at
    // on the VS pipeline, then look up matrix registers via the shader's
    // analyzer data. kMaxVSCBSlots declared public above.
    ID3D10VertexShader*   m_boundVS;
    ID3D10Buffer*         m_boundVSCBs[kMaxVSCBSlots];

    // Option A per-stage binding state (cycle 2). Mirrors Context11Proxy's
    // m_boundInfo[3] and m_cbSlots[stage].proxy[slot]. Null entries mean
    // "nothing bound at this slot." Shader info pointers come from the
    // m_shaderProjections map and have the same lifetime -- never AddRef'd.
    const ShaderAnalysis11Result* m_boundInfo[ST10_COUNT]        = {};
    class Buffer10Proxy*          m_cbSlots[ST10_COUNT][kMaxVSCBSlots] = {};
    ID3D10ShaderResourceView*     m_srvSlots[ST10_COUNT][kMaxSRVSlots10] = {};

    // Scratch byte buffer reused across EditRightCBs10 passes.
    std::vector<unsigned char>    m_editScratch10;
    // Per-eye-shift-value cadence: avoid redundant right-sibling writes.
    float                         m_editLastShift = 0.f;
    float                         m_editLastConv  = 0.f;

    // Hunter sub-draw bookkeeping (parallel to Context11Proxy's).
    unsigned                      m_drawIndex10   = 0;
};

} // namespace wiz3d
