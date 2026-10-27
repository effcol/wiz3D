/*
 * Project : wiz3D
 * Copyright (C) wiz3D contributors 2026
 *
 * Katanga shared-texture producer helper. Katanga is bo3b's open-source
 * stereo-content viewer (https://github.com/bo3b/Katanga, GPL-3.0). This
 * header declares a self-contained DX11 producer that any wiz3D wrapper
 * can own to feed Katanga a wide SBS texture.
 *
 * Protocol (verified 2026-10-27 against DeviarePlugin/InProc_DX11.cpp):
 *   - Named shared memory: "Local\\KatangaMappedFile", 4 bytes, holds the
 *     32-bit HANDLE of our shared texture (via PtrToUint).
 *   - Named mutex:          "KatangaSetupMutex", Katanga-owned; we open
 *     with SYNCHRONIZE only. Katanga MUST be running first; if the mutex
 *     doesn't exist, TryInit() returns false and the wrapper should fall
 *     back to HalfSBS output + a user prompt.
 *   - Texture: 2W x H (wide SBS, left eye [0..W), right [W..2W)),
 *     R8G8B8A8_UNORM or B8G8R8A8_UNORM (strip _SRGB suffix),
 *     D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_SHARED (legacy KMT,
 *     NOT _NTHANDLE).
 *   - Sync: pure setup-mutex handshake. No keyed mutex. Producer waits
 *     before overwrite, Katanga waits before read. Per-frame cycle:
 *     Wait(mutex) -> CopyResource into our shared tex -> Release(mutex).
 *   - Teardown: null the mapped view (publish HANDLE == 0) before
 *     releasing the texture.
 *
 * License posture: the protocol is an API contract (not copyrightable).
 * This file is LGPL-2.1 wiz3D-authored; nothing from Katanga's GPL-3
 * source is linked, embedded, or depended on at build or run time.
 */
#pragma once

#include <windows.h>
#include <d3d11.h>

namespace wiz3d {

// Lifecycle helper. Instances are intended to be owned by a wrapper's
// swap-chain proxy. Thread-affinity: all methods must be called from the
// same thread that owns the backing D3D11 device context.
class KatangaProducer
{
public:
    KatangaProducer();
    ~KatangaProducer();

    // Open the Katanga-side mutex + mapped file. Returns true if Katanga
    // is running (mutex exists) and the mapped file opened; false
    // otherwise (caller should fall back to another output mode and
    // optionally prompt the user to launch Katanga).
    // Idempotent -- calling after a successful TryInit is a no-op.
    bool TryInit();

    // Create (or recreate on Resize) the shared SBS texture and publish
    // its HANDLE into the mapped file. Must be called with a live DX11
    // device + the current backbuffer description so the texture inherits
    // format / sample count / usage. Width is doubled internally for the
    // wide-SBS layout. Returns true on success.
    bool OnBackbufferReady(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& bbDesc);

    // Per-Present handshake. Takes a wide-SBS source texture (2W x H)
    // that the wrapper's compositor already filled this frame and copies
    // it into the shared texture under the setup mutex. Returns true if
    // the copy ran; false if the mutex wait timed out or state is bad.
    bool PublishFrame(ID3D11DeviceContext* ctx, ID3D11Texture2D* wideSbsSrc);

    // Called on wrapper teardown or when the user switches away from
    // Katanga output mode. Nulls the mapped view and releases the shared
    // texture. Safe to call multiple times.
    void Shutdown();

    // Non-fatal availability check that doesn't open any handles.
    // Returns true if Katanga's setup mutex is currently present on
    // the system (i.e. Katanga is running). For OutputMode auto-detect.
    static bool IsKatangaRunning();

    // Expected dimensions of the SBS source texture the wrapper must
    // produce before calling PublishFrame. Width is 2x the backbuffer
    // width; height matches. The wrapper's compositor must write left
    // eye to [0..W) and right eye to [W..2W).
    UINT GetSbsWidth()  const { return m_SbsWidth; }
    UINT GetSbsHeight() const { return m_SbsHeight; }
    DXGI_FORMAT GetSbsFormat() const { return m_SbsFormat; }

    bool IsReady() const { return m_SharedTex != nullptr && m_MappedView != nullptr; }

private:
    KatangaProducer(const KatangaProducer&) = delete;
    KatangaProducer& operator=(const KatangaProducer&) = delete;

    // Strip _SRGB suffix from a backbuffer format. Katanga expects a
    // non-sRGB view; the wrapper is responsible for any gamma handling
    // on its side of the composite.
    static DXGI_FORMAT StripSrgb(DXGI_FORMAT fmt);

    HANDLE  m_SetupMutex    = nullptr;    // KatangaSetupMutex, opened with SYNCHRONIZE
    HANDLE  m_MappedFile    = nullptr;    // Local\KatangaMappedFile
    void*   m_MappedView    = nullptr;    // 4 bytes, holds the HANDLE as 32-bit

    ID3D11Texture2D* m_SharedTex = nullptr;
    HANDLE  m_SharedHandle  = nullptr;    // KMT handle via IDXGIResource::GetSharedHandle
    UINT    m_SbsWidth      = 0;
    UINT    m_SbsHeight     = 0;
    DXGI_FORMAT m_SbsFormat = DXGI_FORMAT_UNKNOWN;

    bool    m_InitAttempted = false;      // for idempotency on TryInit retries
};

} // namespace wiz3d
