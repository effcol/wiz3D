/*
 * Project : wiz3D
 * Copyright (C) wiz3D contributors 2026
 *
 * Katanga shared-texture producer, DX9 variant. Mirrors the protocol
 * handled by KatangaProducer.h for DX11 producers, but uses the DX9
 * CreateTexture(..., &pSharedHandle) API path. The underlying shared
 * handle is a legacy KMT handle that Katanga's DX11 consumer opens
 * via OpenSharedResource -- cross-API sharing works because both ends
 * use the same KMT convention.
 *
 * Protocol (identical to DX11 side; verified against DeviarePlugin
 * InProc_DX9.cpp producer):
 *   - Named mutex "KatangaSetupMutex" (Katanga owns; we open SYNCHRONIZE)
 *   - Named mapped file "Local\\KatangaMappedFile" (4 bytes, 32-bit HANDLE)
 *   - Shared texture: 2W x H, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT,
 *     format matches backbuffer (strip _SRGB equivalents).
 *   - Per-frame sync: ScopedKatangaMutex wait/release around StretchRect
 *     of each eye into its half of the shared texture.
 *   - Teardown: null the mapped view before releasing the texture.
 *
 * License posture: identical to the DX11 helper. Protocol is API contract,
 * nothing linked from Katanga's GPL-3 source.
 */
#pragma once

#include <windows.h>
#include <d3d9.h>

namespace wiz3d {

class KatangaProducerDX9
{
public:
    KatangaProducerDX9();
    ~KatangaProducerDX9();

    // Open Katanga's named mutex + mapped file. Returns false if Katanga
    // isn't running (mutex not present). Idempotent on retry.
    bool TryInit();

    // Create (or recreate) the shared wide-SBS texture + publish its KMT
    // HANDLE into the mapped file. Call on first frame and whenever the
    // backbuffer dimensions or format change (Device Reset).
    bool OnBackbufferReady(IDirect3DDevice9* device,
                            UINT backbufferWidth, UINT backbufferHeight,
                            D3DFORMAT backbufferFormat);

    // Per-frame: wait setup mutex, StretchRect left + right eye surfaces
    // into the left/right halves of the shared texture, release mutex.
    // Returns false if Katanga isn't connected or state is bad.
    bool PublishFrame(IDirect3DDevice9* device,
                      IDirect3DSurface9* leftEye, IDirect3DSurface9* rightEye,
                      bool swapEyes);

    // Null the mapped view + release the shared texture. Keeps mutex +
    // mapped file open for a subsequent OnBackbufferReady; call Shutdown()
    // to tear everything down.
    void OnResize();

    // Full teardown: null mapped view, release texture, close mutex +
    // mapped file handles. Safe to call multiple times.
    void Shutdown();

    // Non-destructive probe: does Katanga appear to be running?
    static bool IsKatangaRunning();

    bool IsReady() const { return m_SharedTex != nullptr && m_MappedView != nullptr; }

private:
    KatangaProducerDX9(const KatangaProducerDX9&) = delete;
    KatangaProducerDX9& operator=(const KatangaProducerDX9&) = delete;

    HANDLE  m_SetupMutex    = nullptr;
    HANDLE  m_MappedFile    = nullptr;
    void*   m_MappedView    = nullptr;

    IDirect3DTexture9* m_SharedTex    = nullptr;
    IDirect3DSurface9* m_SharedSurf   = nullptr;    // base level 0 surface for StretchRect dest
    HANDLE             m_SharedHandle = nullptr;    // KMT handle from CreateTexture pSharedHandle
    UINT               m_SbsWidth     = 0;          // 2 * backbuffer width
    UINT               m_SbsHeight    = 0;          // backbuffer height
    D3DFORMAT          m_SbsFormat    = D3DFMT_UNKNOWN;

    bool    m_InitAttempted = false;
};

} // namespace wiz3d
