/* IZ3D_FILE: $Id$ 
*
* Project : iZ3D Stereo Driver
* Copyright (C) iZ3D Inc. 2002 - 2010
*
* $Author$
* $Revision$
* $Date$
* $LastChangedBy$
* $URL$
*/
#include "StdAfx.h"
#include <Shlobj.h>
#include <Shlwapi.h>
#include "Globals.h"
#include "..\S3DAPI\ReadData.h"
#include "ShaderAnalyzer.h"
#include <tinyxml.h>
#include "CommonUtils/CommonResourceFolders.h"
#include "CommandDumper.h"

void InitDirectories()
{
#ifdef ZLOG_ALLOW_TRACING
	InitializeLogging();	
	_tcscpy(gData.DumpDirectory, gInfo.LogFileDirectory);
	PathAppend(gData.DumpDirectory, _T("DX9"));
	CreateDirectory(gData.DumpDirectory, 0);
	ClearDirectory(gData.DumpDirectory);
	CCommandDumper::GetInstance().Init();
#endif
}

void InitGammaRamp()
{
	HDC dc = GetDC(0);
	//DEBUG_MESSAGE("DeviceGammaRamp\n");
	//for(int i=0; i< 256; i++)
	//{
	//    DEBUG_MESSAGE("%d;%d;%d;%d\n",i,g_DeviceGammaRamp.red[i],g_DeviceGammaRamp.green[i],g_DeviceGammaRamp.blue[i]);
	//}
	D3DGAMMARAMP IdentityGammaRamp;
	//--- set linear GDI gammaRAMP ---
	for(int i=0; i< 256; i++)
	{
		IdentityGammaRamp.red[i]   = i << 8 | i;
		IdentityGammaRamp.green[i] = i << 8 | i;
		IdentityGammaRamp.blue[i]  = i << 8 | i;
	}
	//--- force to apply gamma and disable NVidia or ATI CPL gamma while app work ---
	IdentityGammaRamp.red[0]   = 1;
	IdentityGammaRamp.green[0] = 1;
	IdentityGammaRamp.blue[0]  = 1;
	SetDeviceGammaRamp(dc, &IdentityGammaRamp);
	//--- we must call function SetDeviceGammaRamp() twice for correct working ---
	IdentityGammaRamp.red[0]   = 0;
	IdentityGammaRamp.green[0] = 0;
	IdentityGammaRamp.blue[0]  = 0;
	SetDeviceGammaRamp(dc, &IdentityGammaRamp);
	ReleaseDC(0, dc);
}
