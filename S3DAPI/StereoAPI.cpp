// S3DAPI.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#include "GlobalData.h"
#include "StereoAPI.h"
#include "Shared\version.h"

HRESULT StereoAPI::GetDriverVersion(DWORD* pMajor, DWORD* pMinor, DWORD* pBuild, DWORD* pQFE)
{
	*pMajor = PRODUCT_VERSION_MAJOR;
	*pMinor = PRODUCT_VERSION_MINOR;
	*pBuild = PRODUCT_VERSION_BUILD;
	*pQFE	= PRODUCT_VERSION_QFE;
	DEBUG_TRACE3(__FUNCTIONW__ L"(%d, %d, %d, %d)\n", 
		PRODUCT_VERSION_MAJOR, PRODUCT_VERSION_MINOR, PRODUCT_VERSION_BUILD, PRODUCT_VERSION_QFE);
	return S_OK;
}

float StereoAPI::GetSeparation()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()=%f\n", gInfo.Input.GetActivePreset()->StereoBase);
	return gInfo.Input.GetActivePreset()->StereoBase;
}

HRESULT StereoAPI::SetSeparation(float Value)
{
	DEBUG_TRACE3(__FUNCTIONW__ L"(%f)\n", Value);
	gInfo.Input.GetActivePreset()->StereoBase = Value;
	return S_OK;
}

float StereoAPI::GetConvergence()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()=%f\n", gInfo.Input.GetActivePreset()->One_div_ZPS);
	return gInfo.Input.GetActivePreset()->One_div_ZPS;
}

HRESULT StereoAPI::SetConvergence(float Value)
{
	DEBUG_TRACE3(__FUNCTIONW__ L"(%f)\n", Value);
	gInfo.Input.GetActivePreset()->One_div_ZPS = Value;
	return S_OK;
}

int StereoAPI::GetPreset()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()=%d\n", gInfo.Input.ActivePreset);
	return gInfo.Input.ActivePreset;
}

HRESULT StereoAPI::SetPreset(int Value)
{
	DEBUG_TRACE3(__FUNCTIONW__ L"(%d)\n", Value);
	if (Value < 0 || Value > 2)
		return E_INVALIDARG;
	gInfo.Input.ActivePreset = Value;
	return S_OK;
}

bool StereoAPI::IsLaserSightOn()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()\n");
	return gInfo.Input.LaserSightEnable;
}

HRESULT StereoAPI::SetLaserSight(bool Value)
{
	DEBUG_TRACE3(__FUNCTIONW__ L"(%d)\n", Value);
	gInfo.Input.LaserSightEnable = Value;
	return S_OK;
}

bool StereoAPI::IsAutoFocusOn()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()\n");
	return gInfo.Input.GetActivePreset()->AutoFocusEnable;
}

HRESULT StereoAPI::SetAutoFocus(bool Value)
{
	DEBUG_TRACE3(__FUNCTIONW__ L"(%d)\n", Value);
	gInfo.Input.GetActivePreset()->AutoFocusEnable = Value;
	return S_OK;
}

bool StereoAPI::IsStereoOn()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()\n");
	return gInfo.Input.StereoActive;
}

HRESULT StereoAPI::SetStereoActive(bool Value)
{
	DEBUG_TRACE3(__FUNCTIONW__ L"(%d)\n", Value);
	gInfo.Input.StereoActive = Value;
	return S_OK;
}

bool StereoAPI::IsShowOSDOn()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()=%d\n", gInfo.ShowOSD);
	return gInfo.ShowOSD;
}

HRESULT StereoAPI::SetShowOSDOn(bool Value)
{
	DEBUG_TRACE3(__FUNCTIONW__ L"(%d)\n", Value);
	gInfo.ShowOSD = Value;
	return S_OK;
}

bool StereoAPI::IsShowFPSOn()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()=%d\n", gInfo.ShowFPS);
	return gInfo.ShowFPS;
}

HRESULT StereoAPI::SetShowFPSOn(bool Value)
{
	DEBUG_TRACE3(__FUNCTIONW__ L"(%d)\n", Value);
	gInfo.ShowFPS = Value;
	return S_OK;
}

float StereoAPI::GetMinSeparation()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()=%f\n", 0.0f);
	return 0.0f;
}

float StereoAPI::GetMaxSeparation()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()=%f\n", MAX_STEREOBASE);
	return MAX_STEREOBASE;
}

float StereoAPI::GetMinConvergence()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()=%f\n",MIN_ONE_DIV_ZPS);
	return MIN_ONE_DIV_ZPS;
}

float StereoAPI::GetMaxConvergence()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()=%f\n",MAX_ONE_DIV_ZPS);
	return MAX_ONE_DIV_ZPS;
}

int StereoAPI::GetPresetsCount()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()=%d\n",3);
	return 3;
}

int StereoAPI::GetRenderTargetCreationMode()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()=%d\n",gInfo.RenderTargetCreationMode);
	return gInfo.RenderTargetCreationMode;
}

HRESULT StereoAPI::SetRenderTargetCreationMode(int Value)
{
	DEBUG_TRACE3(__FUNCTIONW__ L"(%d)\n",gInfo.RenderTargetCreationMode);
	if (Value < 0 || Value > 2)
		return E_INVALIDARG;
	gInfo.RenderTargetCreationMode = Value;
	return S_OK;
}

HRESULT StereoAPI::BeginMonoBlock()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()\n");
	m_BlockStack.push(false);
	gInfo.DrawType = 0;
	return S_OK;
}

HRESULT StereoAPI::EndMonoBlock()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()\n");
	if (m_BlockStack.top() != false)
		return E_FAIL;
	m_BlockStack.pop();
	if (m_BlockStack.empty())
		gInfo.DrawType = 2;
	else
		gInfo.DrawType = m_BlockStack.top() ? 1 : 0;
	return S_OK;
}

HRESULT StereoAPI::BeginStereoBlock()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()\n");
	m_BlockStack.push(true);
	gInfo.DrawType = 1;
	return S_OK;
}

HRESULT StereoAPI::EndStereoBlock()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()\n");
	if (m_BlockStack.top() != true)
		return E_FAIL;
	m_BlockStack.pop();
	if (m_BlockStack.empty())
		gInfo.DrawType = 2;
	else
		gInfo.DrawType = m_BlockStack.top() ? 1 : 0;
	return S_OK;
}

bool StereoAPI::IsLastDrawStereo()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()=%d\n", gInfo.LastDrawStereo);
	return gInfo.LastDrawStereo;
}

HRESULT StereoAPI::SetBltSrcEye(int Value)
{
	DEBUG_TRACE3(__FUNCTIONW__ L"(%d)\n", Value);
	if (Value < 0 || Value > 1)
		return E_INVALIDARG;
	gInfo.BltSrcEye = Value;
	return S_OK;
}

int StereoAPI::GetBltSrcEye()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()=%d\n", gInfo.BltSrcEye);
	return gInfo.BltSrcEye;
}

HRESULT StereoAPI::SetBltDstEye(int Value)
{
	DEBUG_TRACE3(__FUNCTIONW__ L"(%d)\n", Value);
	if (Value < 0 || Value > 1)
		return E_INVALIDARG;
	gInfo.BltDstEye = Value;
	return S_OK;
}

int StereoAPI::GetBltDstEye()
{
	DEBUG_TRACE3(__FUNCTIONW__ L"()=%d\n", gInfo.BltDstEye);
	return gInfo.BltDstEye;
}
