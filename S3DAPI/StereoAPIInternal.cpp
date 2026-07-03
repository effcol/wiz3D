// S3DAPI.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#include "GlobalData.h"
#include "StereoAPIInternal.h"
#include "Shared\version.h"

HRESULT StereoAPIInternal::MakeScreenshot(TCHAR *path,MakeScreenshotCallback callback)
{
	DEBUG_TRACE3(__FUNCTIONW__ L"(%s, %p)\n", path, callback);
	if (gInfo.MakeScreenshot) return E_FAIL;

	wcscpy_s(gInfo.MakeScreenshotPath,path);
	gInfo.MakeScreenshotCallback = callback;
	gInfo.MakeScreenshot = TRUE;

	return S_OK;
}

HRESULT StereoAPIInternal::SetEID(UINT64 eid)
{
	DEBUG_TRACE3(__FUNCTIONW__ L"(%d)\n", eid);
	gInfo.EID = eid;
	return S_OK;
}