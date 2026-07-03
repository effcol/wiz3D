/* 
* Project : iZ3D Stereo Driver
* Copyright (C) iZ3D Inc. 2002 - 2010
*/

// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#include "targetver.h"

#include <stdio.h>
#include <stdarg.h>
#include <tchar.h>
#include <windows.h>

#include "SharedInclude.h"
#include "..\..\S3DWrapper9\Trace.h"

#define SHADERTEST
#undef	GINFO_DEBUG
#define GINFO_DEBUG		TRUE
#undef	DEBUG_MESSAGE
#define DEBUG_MESSAGE	__noop

#include "..\..\CommonUtils\System.h"
