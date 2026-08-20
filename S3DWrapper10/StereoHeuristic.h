/* wiz3D - stereo-doubling heuristic helpers (Option B Stage 3)
 *
 * Mirrors the logic in S3DWrapper10/ResourceWrapper.cpp's
 * IsStereoRenderTargetTexture / IsStereoRenderTargetSurface /
 * IsStereoDepthStencilTexture / IsStereoDepthStencilSurface, but exposed
 * as plain free functions so the new COM-layer Resource11Proxy /
 * Texture2D11Proxy / etc. can decide whether to create a right-eye
 * sibling at construction time. The legacy DDI ResourceWrapper keeps
 * its own copies for the UseCOMWrap=0 fallback path; we don't modify
 * it. When Stage 4 ports per-eye routing onto the proxies, the
 * config knobs (gInfo.MonoRenderTargetTextures, CreateSquareRTInMono,
 * etc.) become the single source of truth for both paths.
 */

#pragma once

#include <dxgi.h>
#include <d3d10.h>
#include "..\S3DAPI\GlobalData.h"

namespace wiz3d
{

inline bool IsSquareSize_Stage3(UINT Width, UINT Height, BOOL CreateBigInStereo, const SIZE* pBBSize)
{
    if (Width != Height) return false;
    if (pBBSize)
    {
        if (pBBSize->cx == pBBSize->cy && Width == (UINT)pBBSize->cx) return false;
        if (CreateBigInStereo && (Width >= (UINT)pBBSize->cx && Height >= (UINT)pBBSize->cy)) return false;
    }
    return true;
}

inline bool IsLessThanBB_Stage3(UINT Width, UINT Height, const SIZE* pBBSize)
{
    if (!pBBSize) return false;
    if (Width == Height) return false;
    if ((Width + 5) >= (UINT)pBBSize->cx || (Height + 5) >= (UINT)pBBSize->cy) return false;
    float aRT = (1.0f * Width / Height);
    float aBB = (1.0f * pBBSize->cx / pBBSize->cy);
    float f   = aRT > aBB ? aRT - aBB : aBB - aRT;
    return f >= 0.01f;
}

inline bool IsShadowFormat_Stage3(DXGI_FORMAT Format)
{
    switch (Format)
    {
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
            return gInfo.CreateOneComponentRTInMono != 0;
        default:
            return false;
    }
}

// Render-target Texture2D: decides whether to double the resource for stereo.
// Mirrors ResourceWrapper::IsStereoRenderTargetTexture (S3DWrapper10/
// ResourceWrapper.cpp:207) at the COM layer.
inline bool ShouldDoubleRenderTargetTexture(DXGI_FORMAT Format, UINT Width, UINT Height, const SIZE* pBBSize)
{
    switch (gInfo.RenderTargetCreationMode)
    {
        case 0: return false;
        case 1: return true;
        default:
            if (IsShadowFormat_Stage3(Format))                                                                            return false;
            if (gInfo.MonoRenderTargetTextures)                                                                            return false;
            if (gInfo.CreateSquareRTInMono && IsSquareSize_Stage3(Width, Height, gInfo.CreateBigSquareRTInStereo, pBBSize)) return false;
            if (gInfo.CreateRTThatLessThanBBInMono && IsLessThanBB_Stage3(Width, Height, pBBSize))                          return false;
            if (Width == 1 || Height == 1)                                                                                 return false;
            return true;
    }
}

// Depth-stencil Texture2D: mirrors ResourceWrapper::IsStereoDepthStencilTexture.
inline bool ShouldDoubleDepthStencilTexture(UINT Width, UINT Height, const SIZE* pBBSize)
{
    switch (gInfo.RenderTargetCreationMode)
    {
        case 0: return false;
        case 1: return true;
        default:
            // Oversized depth targets are shadow atlases; their light-camera
            // constants never shift, so a sibling is identical (GTA V: ~450 draws/frame).
            if (pBBSize && (ULONGLONG)Width * Height >=
                           4ull * (ULONGLONG)pBBSize->cx * (ULONGLONG)pBBSize->cy)
                return false;
            if (gInfo.MonoDepthStencilTextures)                                                                            return false;
            if (gInfo.CreateSquareDSInMono && IsSquareSize_Stage3(Width, Height, gInfo.CreateBigSquareDSInStereo, pBBSize)) return false;
            if (gInfo.CreateDSThatLessThanBBInMono && IsLessThanBB_Stage3(Width, Height, pBBSize))                          return false;
            if (Width == 1 || Height == 1)                                                                                 return false;
            return true;
    }
}

// Helper: given a Texture2D desc, classify it as a render-target / depth-
// stencil / neither, then dispatch to the matching heuristic. Returns true
// if a right-eye sibling should be created.
inline bool ShouldDoubleTexture2D(const D3D11_TEXTURE2D_DESC* pDesc, const SIZE* pBBSize)
{
    if (!pDesc) return false;
    if ((pDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0)
        return ShouldDoubleDepthStencilTexture(pDesc->Width, pDesc->Height, pBBSize);
    if ((pDesc->BindFlags & D3D11_BIND_RENDER_TARGET) != 0)
        return ShouldDoubleRenderTargetTexture(pDesc->Format, pDesc->Width, pDesc->Height, pBBSize);
    // UAV-only textures are deliberately not doubled: NVIDIA's 3D Vision
    // heuristics cover render targets and depth only, and a compute output
    // written once outside a duplicated dispatch would leave the sibling
    // uninitialised. GTA V has none of these anyway.
    return false;
}

// D3D10 overload — same flag values as D3D11 (D3D10_BIND_RENDER_TARGET ==
// D3D11_BIND_RENDER_TARGET == 0x20, D3D10_BIND_DEPTH_STENCIL == 0x40) so we
// share the underlying decisions; only the desc type changes.
inline bool ShouldDoubleTexture2D(const D3D10_TEXTURE2D_DESC* pDesc, const SIZE* pBBSize)
{
    if (!pDesc) return false;
    if ((pDesc->BindFlags & D3D10_BIND_DEPTH_STENCIL) != 0)
        return ShouldDoubleDepthStencilTexture(pDesc->Width, pDesc->Height, pBBSize);
    if ((pDesc->BindFlags & D3D10_BIND_RENDER_TARGET) != 0)
        return ShouldDoubleRenderTargetTexture(pDesc->Format, pDesc->Width, pDesc->Height, pBBSize);
    return false;
}

} // namespace wiz3d
