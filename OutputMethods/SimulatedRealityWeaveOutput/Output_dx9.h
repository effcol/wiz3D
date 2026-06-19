/*
* Project : iZ3D Stereo Driver
* Simulated Reality Weave Output for Samsung Odyssey 3D / Acer SpatialLabs
* Copyright (C) iZ3D Inc. 2002 - 2025
*/

#pragma once

#include "OutputMethod_dx9.h"

// Forward declaration — full definition in ThirdPartyLibs/SR-Lib_v1.1.2/include/SR.hpp.
namespace SimulatedReality {
    class SRInterfaceDX9;
}

namespace DX9Output
{

class SimulatedRealityWeaveOutput :
    public OutputMethod
{
public:
    SimulatedRealityWeaveOutput(DWORD mode, DWORD spanMode);
    virtual ~SimulatedRealityWeaveOutput(void);

    virtual UINT    GetOutputChainsNumber();
    virtual HRESULT Output(CBaseSwapChain* pSwapChain);
    virtual HRESULT InitializeSCData(CBaseSwapChain* pSwapChain);
    virtual void    Clear();
    virtual void    ReadConfigData(const char* configXml);

private:
    SimulatedReality::SRInterfaceDX9* m_pSRInterface;
    IDirect3DTexture9* m_pSBSTexture;
    UINT               m_ViewWidth;
    UINT               m_ViewHeight;
    D3DFORMAT          m_ViewFormat;
    bool               m_WeaverInitialized;
    bool               m_WeavingEnabled;
    bool               m_bSRGB;             // Treat input as sRGB and write sRGB output. Default true (modern games / SR sample default). Override per-game via config.
    bool               m_bSRFallbackActive; // Sticky: set to true when InitializeWeaver fails (no Simulated Reality runtime / no SR display / SR Service down). Once set, Output() renders plain Half SBS instead of attempting SR weave again.

    HRESULT InitializeWeaver(IDirect3DDevice9* pDevice, HWND hWnd);
    void    CleanupWeaver();
    HRESULT OutputSBSFallback(CBaseSwapChain* pSwapChain);  // Plain Half SBS (two StretchRects into primary BB) — invoked when SR weave is unavailable
};

}
