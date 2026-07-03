/*
 * wiz3D shared version macros.
 *
 * The numeric components, git short SHA, and build timestamp come from
 * bin/temp_version.h (regenerated each build by bin/generate_version.ps1
 * from the repo-root VERSION file). This file just composes them into the
 * macro names .rc files and source code expect.
 *
 * Original framework: iZ3D Stereo Driver. Copyright lines below preserved
 * for legal attribution; the wiz3D fork retains them per iZ3D's license.
 */
#include "..\bin\temp_version.h"
#include "ProductNames.h"

// Pre-release suffix shown next to the version string (e.g. "-rc1", "-beta").
// Leave empty for stable releases.
#define VERSION_NAME_SUFFIX     ""

#define _STR(x) #x
#define STR(x) _STR(x)

// Comma-separated 4-tuple for VS_VERSION_INFO's FILEVERSION/PRODUCTVERSION
#define VERSION_NUMBER          VERSION_MAJOR, VERSION_MINOR, VERSION_BUILD, PRODUCT_VERSION_QFE
#define PRODUCT_VERSION_NUMBER  PRODUCT_VERSION_MAJOR, PRODUCT_VERSION_MINOR, PRODUCT_VERSION_BUILD, PRODUCT_VERSION_QFE

// Dotted strings for VS_VERSION_INFO's "FileVersion"/"ProductVersion" string
// fields. wiz3D uses the same value for both (all DLLs in a release share one
// version); keeping the macro names mirrored to the iZ3D scaffolding so the
// existing .rc files don't all need editing.
#define VERSION_STRING          STR(VERSION_MAJOR) "." STR(VERSION_MINOR) "." STR(VERSION_BUILD) "." STR(PRODUCT_VERSION_QFE)
#define PRODUCT_VERSION_STRING  STR(PRODUCT_VERSION_MAJOR) "." STR(PRODUCT_VERSION_MINOR) "." STR(PRODUCT_VERSION_BUILD) "." STR(PRODUCT_VERSION_QFE)

#define VERSION_COMPANY         "wiz3D"
#define VERSION_COPYRIGHT       "wiz3D contributors. Originally based on iZ3D Stereo Driver, \xA9 iZ3D Inc. 2002-2010."
#define VERSION_TRADEMARK       ""
#define VERSION_BUILD_TIME      WIZ3D_BUILD_TIME
#define VERSION_BUILD_DATE_TIME WIZ3D_BUILD_DATE " " WIZ3D_BUILD_TIME " (git " WIZ3D_GIT_SHA ")"

// Short / display strings for in-app text and About dialogs.
#define VERSION                 STR(PRODUCT_VERSION_MAJOR) "." STR(PRODUCT_VERSION_MINOR) "." STR(PRODUCT_VERSION_BUILD)
#define DISPLAYED_VERSION       STR(PRODUCT_VERSION_MAJOR) "." STR(PRODUCT_VERSION_MINOR) "." STR(PRODUCT_VERSION_BUILD) VERSION_NAME_SUFFIX " (" WIZ3D_GIT_SHA ")"
