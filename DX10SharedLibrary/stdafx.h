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

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
#define NOMINMAX

#include <windows.h>

#include "SharedInclude.h" 

// C RunTime Header Files
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>

#include <atlbase.h>
#include <atlcom.h>

typedef LARGE_INTEGER PHYSICAL_ADDRESS, *PPHYSICAL_ADDRESS;

#ifndef _NTDEF_
typedef __success(return >= 0) LONG NTSTATUS, *PNTSTATUS;
#endif

// directX
#include "d3d10umddi.h"
#pragma warning ( disable : 4201 )
#include "Dxgiddi.h"
#pragma warning ( default : 4201 )
#include "d3dcommon.h"
#define D3D11_NO_HELPERS
#include "D3D11.h"
#include "D3D10_1.h"
#include "D3D10.h"
#include "D3DX9Math.h"

// STL
#include <algorithm>
#include <deque>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// BOOST
#include <boost/intrusive_ptr.hpp>
#include <boost/lexical_cast.hpp>
// _CRTDBG_MAP_ALLOC (via SharedInclude.h) redefines 'free' as '_free_dbg',
// which corrupts boost::object_pool::free(). Shield the Pool headers only.
#pragma push_macro("free")
#undef free
#include <boost/pool/object_pool.hpp>
#include <boost/pool/pool.hpp>
#pragma pop_macro("free")
#include <boost/shared_ptr.hpp>
#include <boost/type_traits.hpp>

// utils
#include "memmgr.h"

#include "..\S3DAPI\GlobalData.h"
#include "GlobalData.h"
#include "CallGuard.h"

#include "Trace.h"
#include "Utils.h"
#include "EnumToString.h"

#include "Streamer\CodeGenerator.h"