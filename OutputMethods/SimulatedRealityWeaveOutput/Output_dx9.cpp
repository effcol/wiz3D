/*
* Project : iZ3D Stereo Driver
* Simulated Reality Weave Output for Samsung Odyssey 3D / Acer SpatialLabs
* Copyright (C) iZ3D Inc. 2002 - 2025
*/

#include "stdafx.h"
#include "Output_dx9.h"
#include "S3DWrapper9\BaseSwapChain.h"
#include <tinyxml.h>

// SR-Lib simplified C-style factory API. Static lib with the SR runtime DLLs
// delay-loaded — see SR.hpp for the rationale and the linker setup that
// pairs with it (DelayLoadDLLs in the vcxproj).
#include "SR.hpp"

using namespace DX9Output;

#ifdef _MANAGED
#pragma managed(push, off)
#endif

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

#ifdef _MANAGED
#pragma managed(pop)
#endif

OUTPUT_API void* CALLBACK CreateOutputDX9(DWORD mode, DWORD spanMode)
{
    return new SimulatedRealityWeaveOutput(mode, spanMode);
}

OUTPUT_API void* CALLBACK CreateOutputDX10(DWORD mode, DWORD spanMode)
{
    return nullptr;
}

OUTPUT_API void* CALLBACK CreateOutputDX11(DWORD mode, DWORD spanMode)
{
    return nullptr;
}

OUTPUT_API DWORD CALLBACK GetOutputCaps()
{
    return 0;
}

OUTPUT_API void CALLBACK GetOutputName(char* name, DWORD size)
{
    strcpy_s(name, size, "SimulatedRealityWeave");
}

OUTPUT_API BOOL CALLBACK EnumOutputModes(DWORD num, char* name, DWORD size)
{
    switch(num)
    {
    case 0:
        strcpy_s(name, size, "Default");
        return TRUE;
    default:
        return FALSE;
    }
}

SimulatedRealityWeaveOutput::SimulatedRealityWeaveOutput(DWORD mode, DWORD spanMode)
    : OutputMethod(mode, spanMode)
    , m_pSRInterface(nullptr)
    , m_pSBSTexture(nullptr)
    , m_ViewWidth(0)
    , m_ViewHeight(0)
    , m_ViewFormat(D3DFMT_A8R8G8B8)
    , m_WeaverInitialized(false)
    , m_WeavingEnabled(true)
    , m_bSRGB(true)        // Default sRGB on — matches SR SDK directx9_weaving sample (eColorSpace::sRGBHardware) and modern engines (Source, Unreal, Unity)
    , m_bSRFallbackActive(false)
{
}

SimulatedRealityWeaveOutput::~SimulatedRealityWeaveOutput(void)
{
    CleanupWeaver();
}

UINT SimulatedRealityWeaveOutput::GetOutputChainsNumber()
{
    return 1;
}

HRESULT SimulatedRealityWeaveOutput::InitializeSCData(CBaseSwapChain* pSwapChain)
{
    if (!pSwapChain)
        return E_INVALIDARG;

    HRESULT hr = OutputMethod::InitializeSCData(pSwapChain);
    if (FAILED(hr))
        return hr;

    // Release existing SBS texture — will be recreated below for new dimensions
    if (m_pSBSTexture) {
        m_pSBSTexture->Release();
        m_pSBSTexture = nullptr;
    }
    // SR-Lib has no invalidate/restore device-objects API; the supported pattern
    // on device reset (resize, windowed↔fullscreen) is destroy + lazy-recreate.
    // Output() will rebuild via InitializeWeaver() on the next frame against the
    // new device state and SBS texture dimensions.
    if (m_pSRInterface) {
        m_pSRInterface->Delete();
        m_pSRInterface = nullptr;
    }
    m_WeaverInitialized = false;

    if (!m_pd3dDevice || !pSwapChain->m_pPrimaryBackBuffer)
        return S_OK;

    D3DSURFACE_DESC desc;
    if (SUCCEEDED(pSwapChain->m_pPrimaryBackBuffer->GetDesc(&desc))) {
        m_ViewWidth  = desc.Width;
        m_ViewHeight = desc.Height;
        m_ViewFormat = desc.Format;

        // SBS texture: left eye occupies [0, ViewWidth), right eye [ViewWidth, ViewWidth*2)
        m_pd3dDevice->CreateTexture(
            m_ViewWidth * 2, m_ViewHeight,
            1, D3DUSAGE_RENDERTARGET, m_ViewFormat,
            D3DPOOL_DEFAULT, &m_pSBSTexture, nullptr);
    }

    return S_OK;
}

HRESULT SimulatedRealityWeaveOutput::InitializeWeaver(IDirect3DDevice9* pDevice, HWND hWnd)
{
    // Tear down any previously created interface (re-init path after device reset / resize)
    if (m_pSRInterface) {
        m_pSRInterface->Delete();
        m_pSRInterface = nullptr;
    }

    // CreateSRInterfaceDX9 manages the lifecycle internally and
    // probes runtime availability via LoadLibrary before touching any of the
    // delay-loaded SR DLLs — returns E_NOINTERFACE when the runtime is absent
    // (the soft path that lets us fall back to Half SBS instead of crashing),
    // or another failing HRESULT for server-down / no-SR-display / etc.
    HRESULT hr = SimulatedReality::CreateSRInterfaceDX9(pDevice, hWnd, &m_pSRInterface);
    if (FAILED(hr) || !m_pSRInterface)
        return FAILED(hr) ? hr : E_FAIL;

    m_WeaverInitialized = true;

    // Bind the SBS texture once here rather than every Output() frame —
    // it only changes on device reset, which tears down and recreates
    // everything via InitializeSCData → InitializeWeaver.
    if (m_pSBSTexture)
        m_pSRInterface->SetInputTexture(m_pSBSTexture, m_bSRGB);

    return S_OK;
}

// Called by the wrapper's StopEngine() before each device Reset (and during
// shutdown). The base OutputMethod::Clear() doesn't know about SR-specific
// resources, so without this override the weaver and our DEFAULT-pool SBS
// texture would survive Reset as dangling pointers - next Output() crashes
// inside m_pSRInterface->Weave() when SR tries to bind its now-invalid vertex
// shader (Max Payne 3 -stereo 1 reproducer, 2026-05-08). After Clear,
// the wrapper's StartEngine() calls Initialize()/InitializeSCData() to
// rebuild for the new device state.
void SimulatedRealityWeaveOutput::Clear()
{
	CleanupWeaver();
	if (m_pSBSTexture)
	{
		m_pSBSTexture->Release();
		m_pSBSTexture = nullptr;
	}
	// Reset the SR-fallback decision on Clear so post-device-reset (e.g. windowed↔
	// fullscreen toggle) we get a fresh chance to detect the SR runtime. If the
	// user installed the Simulated Reality Service mid-session, this is when it gets picked up.
	m_bSRFallbackActive = false;
	OutputMethod::Clear();
}

void SimulatedRealityWeaveOutput::CleanupWeaver()
{
    if (m_pSRInterface) {
        m_pSRInterface->Delete();
        m_pSRInterface = nullptr;
    }
    if (m_pSBSTexture) {
        m_pSBSTexture->Release();
        m_pSBSTexture = nullptr;
    }
    m_WeaverInitialized = false;
}

HRESULT SimulatedRealityWeaveOutput::Output(CBaseSwapChain* pSwapChain)
{
    if (!pSwapChain || !m_WeavingEnabled)
        return S_OK;

    if (!m_pd3dDevice)
        return S_OK;

    // Lazy-initialize the weaver on the first Output call (device and window are ready by then).
    // If the Simulated Reality runtime isn't installed, the SR Service isn't running, or no SR display is
    // connected, InitializeWeaver fails — we then sticky-flag the session into SBS fallback mode
    // so users on non-SR hardware still see a usable stereo image (plain Half SBS) instead of a
    // black frame, and we don't keep retrying SR every Present.
    if (!m_WeaverInitialized && !m_bSRFallbackActive) {
        HWND hWnd = pSwapChain->GetAppWindow();
        HRESULT hr = InitializeWeaver(m_pd3dDevice, hWnd);
        if (FAILED(hr)) {
            m_bSRFallbackActive = true;
            OutputDebugStringA("[SimulatedRealityWeaveOutput] SR runtime unavailable — falling back to Half SBS for this session.\n");
        }
    }

    if (m_bSRFallbackActive)
        return OutputSBSFallback(pSwapChain);

    if (!m_pSRInterface || !m_pSBSTexture)
        return S_OK;

    IDirect3DSurface9* pLeft  = pSwapChain->GetLeftBackBufferRT();
    IDirect3DSurface9* pRight = pSwapChain->GetRightBackBufferRT();
    RECT* pLeftRect  = pSwapChain->GetLeftBackBufferRect();
    RECT* pRightRect = pSwapChain->GetRightBackBufferRect();

    if (!pLeft || !pRight)
        return S_OK;

    IDirect3DSurface9* pSBSSurface = nullptr;
    if (FAILED(m_pSBSTexture->GetSurfaceLevel(0, &pSBSSurface)))
        return S_OK;

    // Blit left eye into left half and right eye into right half of the SBS texture
    RECT leftDst  = { 0,                 0, (LONG)m_ViewWidth,     (LONG)m_ViewHeight };
    RECT rightDst = { (LONG)m_ViewWidth, 0, (LONG)m_ViewWidth * 2, (LONG)m_ViewHeight };

    m_pd3dDevice->StretchRect(pLeft,  pLeftRect,  pSBSSurface, &leftDst,  D3DTEXF_NONE);
    m_pd3dDevice->StretchRect(pRight, pRightRect, pSBSSurface, &rightDst, D3DTEXF_NONE);
    pSBSSurface->Release();

    // Route weaved output to the primary back buffer
    m_pd3dDevice->SetRenderTarget(0, pSwapChain->m_pPrimaryBackBuffer);

    // State-block save/restore: the Dimenco/LeiaSR DX9 weaver mutates D3D9
    // device state (shaders, samplers, render states, vertex decl) and does
    // not restore it. Re-applying our D3DSBT_ALL state block after weave()
    // returns puts the device back to what the wrapper expects, so the
    // wrapper's post-Present code sees the same state it set up.
    IDirect3DStateBlock9* pSavedState = nullptr;
    HRESULT hrSB = m_pd3dDevice->CreateStateBlock(D3DSBT_ALL, &pSavedState);

    // SR-Lib builds its own clean D3D objects internally, so the old
    // wrapped-pointer crash path (Tales of Berseria, SR SDK era) should be
    // closed. No SEH wrapper needed; if SR-Lib ever crashes here the
    // minidump will point at the real problem.
    m_pSRInterface->Weave();

    if (SUCCEEDED(hrSB) && pSavedState)
    {
        pSavedState->Apply();
        pSavedState->Release();
    }

    return S_OK;
}

// Plain Half SBS rendering, invoked when SR weave is unavailable. Mirrors the
// minimum-viable pixel path of SideBySideOutput::Output() (mode 0, no gap, no
// crosseyed) — left eye into the left half of the primary back buffer, right
// eye into the right half. Two StretchRect calls, no shaders, no extra state.
HRESULT SimulatedRealityWeaveOutput::OutputSBSFallback(CBaseSwapChain* pSwapChain)
{
    if (!pSwapChain || !m_pd3dDevice) return S_OK;
    IDirect3DSurface9* primary = pSwapChain->m_pPrimaryBackBuffer;
    if (!primary) return S_OK;

    IDirect3DSurface9* pLeft  = pSwapChain->GetLeftBackBufferRT();
    IDirect3DSurface9* pRight = pSwapChain->GetRightBackBufferRT();
    if (!pLeft || !pRight) return S_OK;
    RECT* pLeftRect  = pSwapChain->GetLeftBackBufferRect();
    RECT* pRightRect = pSwapChain->GetRightBackBufferRect();

    D3DSURFACE_DESC desc;
    primary->GetDesc(&desc);
    RECT leftHalf  = { 0,                       0, (LONG)(desc.Width / 2), (LONG)desc.Height };
    RECT rightHalf = { (LONG)(desc.Width / 2),  0, (LONG)desc.Width,       (LONG)desc.Height };
    m_pd3dDevice->StretchRect(pLeft,  pLeftRect,  primary, &leftHalf,  D3DTEXF_LINEAR);
    m_pd3dDevice->StretchRect(pRight, pRightRect, primary, &rightHalf, D3DTEXF_LINEAR);
    return S_OK;
}

void SimulatedRealityWeaveOutput::ReadConfigData(const char* configXml)
{
    // Optional override under <Outputs><SimulatedRealityWeaveOutput><sRGB Value="0|1"/>.
    // Default (no element) keeps m_bSRGB at the constructor value (true).
    // Re-parse with our own (ticpp-shim) TinyXML so no node crosses from S3DAPI.
    if (!configXml || !*configXml)
        return;
    TiXmlDocument doc;
    doc.Parse(configXml);
    if (doc.Error())
        return;
    TiXmlElement* root = doc.RootElement();
    if (!root)
        return;
    TiXmlElement* el = root->FirstChildElement("sRGB");
    if (el)
    {
        int v = 1;
        if (el->QueryIntAttribute("Value", &v) == TIXML_SUCCESS)
            m_bSRGB = (v != 0);
    }
}
