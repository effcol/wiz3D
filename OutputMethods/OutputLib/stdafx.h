// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#include "targetver.h"

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files:
#include <windows.h>

#include "SharedInclude.h"

// _CRTDBG_MAP_ALLOC (via SharedInclude.h) redefines 'free' as '_free_dbg',
// which corrupts boost::object_pool::free(). Shield the Pool header only.
#pragma push_macro("free")
#undef free
#include <boost\pool\pool_alloc.hpp>
#pragma pop_macro("free")

// DX10
typedef LARGE_INTEGER PHYSICAL_ADDRESS, *PPHYSICAL_ADDRESS;

#ifndef _NTDEF_
typedef __success(return >= 0) LONG NTSTATUS, *PNTSTATUS;
#endif
