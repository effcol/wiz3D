// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#include "targetver.h"

#define WIN32_LEAN_AND_MEAN		// Exclude rarely-used stuff from Windows headers

#include <windows.h>
#include <tchar.h>
#include <strstream>

#include <atlbase.h>
#include <atlcom.h>

typedef LARGE_INTEGER PHYSICAL_ADDRESS, *PPHYSICAL_ADDRESS;

#ifndef _NTDEF_
typedef __success(return >= 0) LONG NTSTATUS, *PNTSTATUS;
#endif

#include <d3d10umddi.h>
#pragma warning ( disable : 4201 )
#include <Dxgiddi.h>
#pragma warning ( default : 4201 )
#include <D3D10.h>
#include <D3DX10Math.h>

#include "SharedInclude.h" 

#include <vector>
#include "memmgr.h"

// _CRTDBG_MAP_ALLOC (via SharedInclude.h) redefines 'free' as '_free_dbg',
// which corrupts boost::object_pool::free(). Shield the Pool header only.
#pragma push_macro("free")
#undef free
#include <boost\pool\pool_alloc.hpp>
#pragma pop_macro("free")

#include <boost\intrusive_ptr.hpp>
#include <boost\noncopyable.hpp>

#include "GlobalData.h"
#include "trace.h"

#include "Resources\resource.h"
