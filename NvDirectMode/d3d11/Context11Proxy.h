/* NvDirectMode - ID3D11DeviceContext proxy
 *
 * Stage 1b-i: passthrough wrapper. Stage 1b-iv hooks OMSetRenderTargets +
 * OMSetRenderTargetsAndUnorderedAccessViews to redirect viewport per active
 * eye when a wrapped backbuffer RTV is bound at slot 0.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

namespace NvDirectMode
{

// Per-frame draw-call counter. Incremented on every Draw* and Dispatch*
// interception in Context11Proxy; read + reset from SwapChainProxy::Present.
// Purpose: distinguish Auto-Mode variants for stereo-capable engines —
// a game that renders each frame twice for stereo (RAGE / MP3 candidate)
// shows ~2x the draws-per-frame count when its 3D option is enabled.
//
// Sibling counter for ExecuteCommandList: deferred-context titles (RAGE +
// most modern engines) record their draws into deferred contexts we DON'T
// wrap yet, then play them back via ExecuteCommandList on the immediate
// context (which IS us). We can't see individual draws from a deferred
// context, but we CAN count how many command lists get executed per frame.
// A dual-pass game would still show 2x command-list executions when 3D on.
extern volatile LONG g_drawsThisFrame;
extern volatile LONG g_cmdListsThisFrame;
inline void CountDraw()       { InterlockedIncrement(&g_drawsThisFrame); }
inline void CountCmdList()    { InterlockedIncrement(&g_cmdListsThisFrame); }
LONG GetAndResetDrawsThisFrame();
LONG GetAndResetCmdListsThisFrame();

class Device11Proxy;

class Context11Proxy : public ID3D11DeviceContext
{
public:
    Context11Proxy(ID3D11DeviceContext* real, Device11Proxy* parent);
    virtual ~Context11Proxy();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef() override                                  { return InterlockedIncrement(&m_refs); }
    ULONG   STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_refs);
        if (r == 0) { if (m_real) { m_real->Release(); m_real = nullptr; } delete this; }
        return (ULONG)r;
    }

    // ID3D11DeviceChild
    void    STDMETHODCALLTYPE GetDevice(ID3D11Device** ppDevice) override;  // returns wrapped parent
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override         { return m_real->GetPrivateData(guid, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override     { return m_real->SetPrivateData(guid, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override       { return m_real->SetPrivateDataInterface(guid, pData); }

    // ID3D11DeviceContext — input assembler / shader stages / rasteriser / output merger / draws
    void STDMETHODCALLTYPE VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override                                                                  { m_real->VSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE PSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override                                                    { m_real->PSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE PSSetShader(ID3D11PixelShader* pPixelShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override                                             { m_real->PSSetShader(pPixelShader, ppClassInstances, NumClassInstances); }
    void STDMETHODCALLTYPE PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override                                                                         { m_real->PSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE VSSetShader(ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override                                           { m_real->VSSetShader(pVertexShader, ppClassInstances, NumClassInstances); }
    void STDMETHODCALLTYPE DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) override                                                                                  { CountDraw(); m_real->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation); }
    void STDMETHODCALLTYPE Draw(UINT VertexCount, UINT StartVertexLocation) override                                                                                                               { CountDraw(); m_real->Draw(VertexCount, StartVertexLocation); }
    HRESULT STDMETHODCALLTYPE Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource) override                               { return m_real->Map(pResource, Subresource, MapType, MapFlags, pMappedResource); }
    void STDMETHODCALLTYPE Unmap(ID3D11Resource* pResource, UINT Subresource) override                                                                                                             { m_real->Unmap(pResource, Subresource); }
    void STDMETHODCALLTYPE PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override                                                                  { m_real->PSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE IASetInputLayout(ID3D11InputLayout* pInputLayout) override                                                                                                              { m_real->IASetInputLayout(pInputLayout); }
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppVertexBuffers, const UINT* pStrides, const UINT* pOffsets) override                          { m_real->IASetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets); }
    void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset) override                                                                                  { m_real->IASetIndexBuffer(pIndexBuffer, Format, Offset); }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override              { CountDraw(); m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation); }
    void STDMETHODCALLTYPE DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) override                                           { CountDraw(); m_real->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation); }
    void STDMETHODCALLTYPE GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override                                                                  { m_real->GSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE GSSetShader(ID3D11GeometryShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override                                               { m_real->GSSetShader(pShader, ppClassInstances, NumClassInstances); }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) override                                                                                                      { m_real->IASetPrimitiveTopology(Topology); }
    void STDMETHODCALLTYPE VSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override                                                    { m_real->VSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE VSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override                                                                         { m_real->VSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE Begin(ID3D11Asynchronous* pAsync) override                                                                                                                              { m_real->Begin(pAsync); }
    void STDMETHODCALLTYPE End(ID3D11Asynchronous* pAsync) override                                                                                                                                { m_real->End(pAsync); }
    HRESULT STDMETHODCALLTYPE GetData(ID3D11Asynchronous* pAsync, void* pData, UINT DataSize, UINT GetDataFlags) override                                                                          { return m_real->GetData(pAsync, pData, DataSize, GetDataFlags); }
    void STDMETHODCALLTYPE SetPredication(ID3D11Predicate* pPredicate, BOOL PredicateValue) override                                                                                               { m_real->SetPredication(pPredicate, PredicateValue); }
    void STDMETHODCALLTYPE GSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override                                                    { m_real->GSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE GSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override                                                                         { m_real->GSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView) override;
    void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts) override;
    void STDMETHODCALLTYPE OMSetBlendState(ID3D11BlendState* pBlendState, const FLOAT BlendFactor[4], UINT SampleMask) override                                                                    { m_real->OMSetBlendState(pBlendState, BlendFactor, SampleMask); }
    void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D11DepthStencilState* pDepthStencilState, UINT StencilRef) override                                                                           { m_real->OMSetDepthStencilState(pDepthStencilState, StencilRef); }
    void STDMETHODCALLTYPE SOSetTargets(UINT NumBuffers, ID3D11Buffer* const* ppSOTargets, const UINT* pOffsets) override                                                                          { m_real->SOSetTargets(NumBuffers, ppSOTargets, pOffsets); }
    void STDMETHODCALLTYPE DrawAuto() override                                                                                                                                                     { CountDraw(); m_real->DrawAuto(); }
    void STDMETHODCALLTYPE DrawIndexedInstancedIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) override                                                                      { CountDraw(); m_real->DrawIndexedInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs); }
    void STDMETHODCALLTYPE DrawInstancedIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) override                                                                             { CountDraw(); m_real->DrawInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs); }
    void STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override                                                                               { CountDraw(); m_real->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ); }
    void STDMETHODCALLTYPE DispatchIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) override                                                                                  { CountDraw(); m_real->DispatchIndirect(pBufferForArgs, AlignedByteOffsetForArgs); }
    void STDMETHODCALLTYPE RSSetState(ID3D11RasterizerState* pRasterizerState) override                                                                                                            { m_real->RSSetState(pRasterizerState); }
    void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports) override;
    void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D11_RECT* pRects) override                                                                                                     { m_real->RSSetScissorRects(NumRects, pRects); }
    // Intercepted out-of-line for the NV magic-header SBS capture path.
    void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox) override;
    void STDMETHODCALLTYPE CopyResource(ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource) override;
    void STDMETHODCALLTYPE UpdateSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) override     { m_real->UpdateSubresource(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch); }
    void STDMETHODCALLTYPE CopyStructureCount(ID3D11Buffer* pDstBuffer, UINT DstAlignedByteOffset, ID3D11UnorderedAccessView* pSrcView) override                                                   { m_real->CopyStructureCount(pDstBuffer, DstAlignedByteOffset, pSrcView); }
    void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4]) override                                                                     { m_real->ClearRenderTargetView(pRenderTargetView, ColorRGBA); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* pUnorderedAccessView, const UINT Values[4]) override                                                            { m_real->ClearUnorderedAccessViewUint(pUnorderedAccessView, Values); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* pUnorderedAccessView, const FLOAT Values[4]) override                                                          { m_real->ClearUnorderedAccessViewFloat(pUnorderedAccessView, Values); }
    void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil) override                                                  { m_real->ClearDepthStencilView(pDepthStencilView, ClearFlags, Depth, Stencil); }
    void STDMETHODCALLTYPE GenerateMips(ID3D11ShaderResourceView* pShaderResourceView) override                                                                                                    { m_real->GenerateMips(pShaderResourceView); }
    void STDMETHODCALLTYPE SetResourceMinLOD(ID3D11Resource* pResource, FLOAT MinLOD) override                                                                                                     { m_real->SetResourceMinLOD(pResource, MinLOD); }
    FLOAT STDMETHODCALLTYPE GetResourceMinLOD(ID3D11Resource* pResource) override                                                                                                                  { return m_real->GetResourceMinLOD(pResource); }
    void STDMETHODCALLTYPE ResolveSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, ID3D11Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override                   { m_real->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format); }
    void STDMETHODCALLTYPE ExecuteCommandList(ID3D11CommandList* pCommandList, BOOL RestoreContextState) override                                                                                  { CountCmdList(); m_real->ExecuteCommandList(pCommandList, RestoreContextState); }
    void STDMETHODCALLTYPE HSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override                                                    { m_real->HSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE HSSetShader(ID3D11HullShader* pHullShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override                                               { m_real->HSSetShader(pHullShader, ppClassInstances, NumClassInstances); }
    void STDMETHODCALLTYPE HSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override                                                                         { m_real->HSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE HSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override                                                                  { m_real->HSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE DSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override                                                    { m_real->DSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE DSSetShader(ID3D11DomainShader* pDomainShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override                                           { m_real->DSSetShader(pDomainShader, ppClassInstances, NumClassInstances); }
    void STDMETHODCALLTYPE DSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override                                                                         { m_real->DSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE DSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override                                                                  { m_real->DSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE CSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override                                                    { m_real->CSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE CSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts) override               { m_real->CSSetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts); }
    void STDMETHODCALLTYPE CSSetShader(ID3D11ComputeShader* pComputeShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override                                         { m_real->CSSetShader(pComputeShader, ppClassInstances, NumClassInstances); }
    void STDMETHODCALLTYPE CSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override                                                                         { m_real->CSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE CSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override                                                                  { m_real->CSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { m_real->VSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE PSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { m_real->PSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE PSGetShader(ID3D11PixelShader** ppPixelShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                               { m_real->PSGetShader(ppPixelShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE PSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->PSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE VSGetShader(ID3D11VertexShader** ppVertexShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                             { m_real->VSGetShader(ppVertexShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE PSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { m_real->PSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE IAGetInputLayout(ID3D11InputLayout** ppInputLayout) override                                                                                                            { m_real->IAGetInputLayout(ppInputLayout); }
    void STDMETHODCALLTYPE IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppVertexBuffers, UINT* pStrides, UINT* pOffsets) override                                            { m_real->IAGetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets); }
    void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D11Buffer** pIndexBuffer, DXGI_FORMAT* Format, UINT* Offset) override                                                                               { m_real->IAGetIndexBuffer(pIndexBuffer, Format, Offset); }
    void STDMETHODCALLTYPE GSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { m_real->GSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE GSGetShader(ID3D11GeometryShader** ppGeometryShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                         { m_real->GSGetShader(ppGeometryShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* pTopology) override                                                                                                    { m_real->IAGetPrimitiveTopology(pTopology); }
    void STDMETHODCALLTYPE VSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { m_real->VSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE VSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->VSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE GetPredication(ID3D11Predicate** ppPredicate, BOOL* pPredicateValue) override                                                                                           { m_real->GetPredication(ppPredicate, pPredicateValue); }
    void STDMETHODCALLTYPE GSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { m_real->GSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE GSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->GSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE OMGetRenderTargets(UINT NumViews, ID3D11RenderTargetView** ppRenderTargetViews, ID3D11DepthStencilView** ppDepthStencilView) override                                   { m_real->OMGetRenderTargets(NumViews, ppRenderTargetViews, ppDepthStencilView); }
    void STDMETHODCALLTYPE OMGetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs, ID3D11RenderTargetView** ppRenderTargetViews, ID3D11DepthStencilView** ppDepthStencilView, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUnorderedAccessViews) override { m_real->OMGetRenderTargetsAndUnorderedAccessViews(NumRTVs, ppRenderTargetViews, ppDepthStencilView, UAVStartSlot, NumUAVs, ppUnorderedAccessViews); }
    void STDMETHODCALLTYPE OMGetBlendState(ID3D11BlendState** ppBlendState, FLOAT BlendFactor[4], UINT* pSampleMask) override                                                                      { m_real->OMGetBlendState(ppBlendState, BlendFactor, pSampleMask); }
    void STDMETHODCALLTYPE OMGetDepthStencilState(ID3D11DepthStencilState** ppDepthStencilState, UINT* pStencilRef) override                                                                       { m_real->OMGetDepthStencilState(ppDepthStencilState, pStencilRef); }
    void STDMETHODCALLTYPE SOGetTargets(UINT NumBuffers, ID3D11Buffer** ppSOTargets) override                                                                                                      { m_real->SOGetTargets(NumBuffers, ppSOTargets); }
    void STDMETHODCALLTYPE RSGetState(ID3D11RasterizerState** ppRasterizerState) override                                                                                                          { m_real->RSGetState(ppRasterizerState); }
    void STDMETHODCALLTYPE RSGetViewports(UINT* pNumViewports, D3D11_VIEWPORT* pViewports) override                                                                                                { m_real->RSGetViewports(pNumViewports, pViewports); }
    void STDMETHODCALLTYPE RSGetScissorRects(UINT* pNumRects, D3D11_RECT* pRects) override                                                                                                         { m_real->RSGetScissorRects(pNumRects, pRects); }
    void STDMETHODCALLTYPE HSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { m_real->HSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE HSGetShader(ID3D11HullShader** ppHullShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                                 { m_real->HSGetShader(ppHullShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE HSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->HSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE HSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { m_real->HSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE DSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { m_real->DSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE DSGetShader(ID3D11DomainShader** ppDomainShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                             { m_real->DSGetShader(ppDomainShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE DSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->DSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE DSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { m_real->DSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE CSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { m_real->CSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE CSGetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUnorderedAccessViews) override                                                    { m_real->CSGetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews); }
    void STDMETHODCALLTYPE CSGetShader(ID3D11ComputeShader** ppComputeShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                           { m_real->CSGetShader(ppComputeShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE CSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->CSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE CSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { m_real->CSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE ClearState() override                                                                                                                                                   { m_real->ClearState(); }
    void STDMETHODCALLTYPE Flush() override                                                                                                                                                        { m_real->Flush(); }
    D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType() override                                                                                                                                 { return m_real->GetType(); }
    UINT STDMETHODCALLTYPE GetContextFlags() override                                                                                                                                              { return m_real->GetContextFlags(); }
    HRESULT STDMETHODCALLTYPE FinishCommandList(BOOL RestoreDeferredContextState, ID3D11CommandList** ppCommandList) override                                                                      { return m_real->FinishCommandList(RestoreDeferredContextState, ppCommandList); }

    // Accessors for 1b-iv routing logic
    ID3D11DeviceContext* GetReal()  const { return m_real; }
    Device11Proxy*       GetParent() const { return m_parent; }

    // Stage 3 helpers — exposed so the shared OMSet hook can mark whether
    // the BB-RTV is currently bound (RSSetViewports needs to know).
    void SetCurrentBBBound(bool b) { m_currentBBBound = b; }
    bool GetCurrentBBBound() const { return m_currentBBBound; }

private:
    ID3D11DeviceContext* m_real;
    Device11Proxy*       m_parent;
    LONG                 m_refs;
    bool                 m_currentBBBound;   // last OMSet had BB-RTV at slot 0
};

} // namespace NvDirectMode
