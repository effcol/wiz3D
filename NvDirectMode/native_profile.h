/* native_profile.h — cross-session state store for native-rendering wrapper
 * paths (3D Vision Direct Mode, HD3D, OpenGL Quad-Buffer). Sibling to iZ3D's
 * UserProfile.xml but scoped to non-injection wrapper paths only, so the
 * injector's per-game profile store stays semantically clean.
 *
 * Header-only so each proxy (NvApiProxy, AmdQbProxy, S3DWrapperOGL when we
 * wire it in) gets its own TU-local copy. Zero linkage / library complexity.
 *
 * Location: %APPDATA%\wiz3D\NativeProfile.xml (source of truth, human-readable,
 *           editable, matches iZ3D UserProfile.xml pattern)
 * Mirror:   HKCU\Software\wiz3D\NativeProfile\<exe> (best-effort registry
 *           mirror so games/tools reading their own registry key see the same
 *           state; XML always wins on conflict)
 *
 * Usage from a proxy DLL's DllMain:
 *
 *     using namespace wiz3D::NativeProfile;
 *     Init("NvDirectMode");   // or "HD3D" / "OpenGLQB"; captures exe name
 *     bool stereoOn = Load(); // reads XML + returns loaded StereoActive
 *     // ... on Activate/Deactivate/Enable/Disable that changes state ...
 *     SetStereoActive(newState);   // writes XML + registry
 *
 * Default on first-boot (no matching profile): StereoActive=1 (stereo on),
 * matching how iZ3D's UserProfile.xml behaves.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace wiz3D {
namespace NativeProfile {

namespace detail {

// TU-local state — each proxy DLL gets its own set. `static` at namespace
// scope gives one copy per translation unit (which is what we want; each
// proxy DLL is its own TU-set) and avoids the C++17 `inline` requirement
// so pre-C++17 projects (AmdQbProxy) can consume this header.
static char        s_gameExeBaseName[MAX_PATH] = {0};
static const char* s_nativePathTag = "unspecified"; // "NvDirectMode" / "HD3D" / "OpenGLQB"
static int         s_stereoActive  = 1;             // in-memory truth; XML+registry mirror it
static bool        s_initialized   = false;

inline int ReadIntTag(const char* xml, const char* tag, int defaultValue)
{
    char needle[64];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "<%s Value=\"", tag);
    const char* p = strstr(xml, needle);
    if (!p) return defaultValue;
    p += strlen(needle);
    return atoi(p);
}

inline bool GetXmlPath(WCHAR* outPath, size_t cch)
{
    if (!outPath || cch < MAX_PATH) return false;
    WCHAR appData[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData)))
        return false;
    _snwprintf_s(outPath, cch, _TRUNCATE, L"%s\\wiz3D\\NativeProfile.xml", appData);
    return true;
}

// Locate <Profile Name="<exe>"> block. Returns pointer to '<Profile' or NULL;
// *outLen set to full span through '</Profile>' end.
inline char* FindProfileBlock(char* buf, const char* exeName, size_t* outLen)
{
    if (!buf || !exeName) return NULL;
    char needle[MAX_PATH + 32];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "<Profile Name=\"%s\"", exeName);
    char* start = strstr(buf, needle);
    if (!start) return NULL;
    char* end = strstr(start, "</Profile>");
    if (!end) return NULL;
    end += strlen("</Profile>");
    if (outLen) *outLen = (size_t)(end - start);
    return start;
}

inline void WriteRegistryMirror()
{
    if (s_gameExeBaseName[0] == '\0') return;

    WCHAR exeW[MAX_PATH] = {0};
    MultiByteToWideChar(CP_UTF8, 0, s_gameExeBaseName, -1, exeW, MAX_PATH);

    WCHAR subKey[MAX_PATH + 64];
    _snwprintf_s(subKey, MAX_PATH + 64, _TRUNCATE,
                 L"Software\\wiz3D\\NativeProfile\\%s", exeW);

    HKEY hKey = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, NULL, 0,
                        KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return;

    DWORD stereoActive = (DWORD)s_stereoActive;
    RegSetValueExW(hKey, L"StereoActive", 0, REG_DWORD,
                   (const BYTE*)&stereoActive, sizeof(stereoActive));

    WCHAR nativePathW[64] = {0};
    MultiByteToWideChar(CP_UTF8, 0, s_nativePathTag, -1, nativePathW, 64);
    RegSetValueExW(hKey, L"NativePath", 0, REG_SZ,
                   (const BYTE*)nativePathW,
                   (DWORD)((wcslen(nativePathW) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
}

inline void SaveXml()
{
    if (s_gameExeBaseName[0] == '\0') return;

    WCHAR xmlPath[MAX_PATH] = {0};
    if (!GetXmlPath(xmlPath, MAX_PATH)) return;

    WCHAR dir[MAX_PATH];
    lstrcpynW(dir, xmlPath, MAX_PATH);
    WCHAR* pSlash = wcsrchr(dir, L'\\');
    if (pSlash) { *pSlash = L'\0'; CreateDirectoryW(dir, NULL); }

    char* buf = NULL;
    size_t bufLen = 0;
    FILE* f = _wfopen(xmlPath, L"rb");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz <= 1024 * 1024)
        {
            buf = (char*)malloc((size_t)sz + 1);
            if (buf)
            {
                bufLen = fread(buf, 1, (size_t)sz, f);
                buf[bufLen] = '\0';
            }
        }
        fclose(f);
    }

    char block[512];
    _snprintf_s(block, sizeof(block), _TRUNCATE,
        "    <Profile Name=\"%s\">\n"
        "        <StereoActive Value=\"%d\"/>\n"
        "        <NativePath Value=\"%s\"/>\n"
        "    </Profile>\n",
        s_gameExeBaseName, s_stereoActive, s_nativePathTag);

    char* out = NULL;
    size_t outLen = 0;
    size_t existingLen = 0;
    char* existing = buf ? FindProfileBlock(buf, s_gameExeBaseName, &existingLen) : NULL;

    if (existing)
    {
        size_t prefix = (size_t)(existing - buf);
        size_t suffixStart = prefix + existingLen;
        size_t suffix = bufLen - suffixStart;
        while (prefix > 0 && (buf[prefix - 1] == ' ' || buf[prefix - 1] == '\t'))
            --prefix;
        outLen = prefix + strlen(block) + suffix;
        out = (char*)malloc(outLen + 1);
        if (out)
        {
            memcpy(out, buf, prefix);
            memcpy(out + prefix, block, strlen(block));
            memcpy(out + prefix + strlen(block), buf + suffixStart, suffix);
            out[outLen] = '\0';
        }
    }
    else
    {
        char* close = buf ? strstr(buf, "</NativeProfiles>") : NULL;
        if (close)
        {
            size_t prefix = (size_t)(close - buf);
            size_t suffix = bufLen - prefix;
            outLen = prefix + strlen(block) + suffix;
            out = (char*)malloc(outLen + 1);
            if (out)
            {
                memcpy(out, buf, prefix);
                memcpy(out + prefix, block, strlen(block));
                memcpy(out + prefix + strlen(block), buf + prefix, suffix);
                out[outLen] = '\0';
            }
        }
        else
        {
            const char* header  = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<NativeProfiles Version=\"1\">\n";
            const char* trailer = "</NativeProfiles>\n";
            outLen = strlen(header) + strlen(block) + strlen(trailer);
            out = (char*)malloc(outLen + 1);
            if (out)
                _snprintf_s(out, outLen + 1, _TRUNCATE, "%s%s%s", header, block, trailer);
        }
    }

    if (buf) free(buf);
    if (!out) return;

    WCHAR tmpPath[MAX_PATH];
    _snwprintf_s(tmpPath, MAX_PATH, _TRUNCATE, L"%s.tmp", xmlPath);
    FILE* w = _wfopen(tmpPath, L"wb");
    if (w)
    {
        fwrite(out, 1, outLen, w);
        fclose(w);
        MoveFileExW(tmpPath, xmlPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    free(out);

    WriteRegistryMirror();
}

} // namespace detail

// Initialize per-DLL state. Captures the game exe basename and tags this
// proxy's native path (used as <NativePath> in the XML and NativePath REG_SZ).
// Idempotent — safe to call more than once.
inline void Init(const char* nativePathTag)
{
    detail::s_nativePathTag = nativePathTag ? nativePathTag : "unspecified";

    WCHAR exePath[MAX_PATH] = {0};
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH))
    {
        WCHAR* leaf = wcsrchr(exePath, L'\\');
        leaf = leaf ? leaf + 1 : exePath;
        WideCharToMultiByte(CP_UTF8, 0, leaf, -1,
                            detail::s_gameExeBaseName, MAX_PATH, NULL, NULL);
    }
    detail::s_initialized = true;
}

// Load StereoActive from NativeProfile.xml (returns default 1 if the file
// doesn't exist or has no entry for this exe). Also updates in-memory state.
// Call after Init().
inline int Load()
{
    if (detail::s_gameExeBaseName[0] == '\0')
    {
        detail::s_stereoActive = 1;
        return 1;
    }

    WCHAR xmlPath[MAX_PATH] = {0};
    if (!detail::GetXmlPath(xmlPath, MAX_PATH))
    {
        detail::s_stereoActive = 1;
        return 1;
    }

    FILE* f = _wfopen(xmlPath, L"rb");
    if (!f)
    {
        detail::s_stereoActive = 1;
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); detail::s_stereoActive = 1; return 1; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); detail::s_stereoActive = 1; return 1; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);

    size_t blockLen = 0;
    char* block = detail::FindProfileBlock(buf, detail::s_gameExeBaseName, &blockLen);
    int loaded = 1;
    if (block)
    {
        char saved = block[blockLen];
        block[blockLen] = '\0';
        loaded = detail::ReadIntTag(block, "StereoActive", 1);
        block[blockLen] = saved;
    }
    free(buf);

    detail::s_stereoActive = loaded ? 1 : 0;
    return detail::s_stereoActive;
}

// Update state and persist (both XML + registry). No-op if new state matches
// current — avoids redundant file writes on games that hammer state calls.
inline void SetStereoActive(bool active)
{
    int v = active ? 1 : 0;
    if (v == detail::s_stereoActive) return;
    detail::s_stereoActive = v;
    detail::SaveXml();
}

// Read current in-memory state.
inline int GetStereoActive()
{
    return detail::s_stereoActive;
}

// Retrieve the resolved XML path (for logging / diagnostic output). Returns
// false if %APPDATA% can't be resolved.
inline bool GetPath(WCHAR* outPath, size_t cch)
{
    return detail::GetXmlPath(outPath, cch);
}

// Manually flush state to XML+registry. Rarely needed — SetStereoActive
// already saves. Useful at DLL_PROCESS_DETACH as a "final safety flush".
inline void Flush()
{
    detail::SaveXml();
}

} // namespace NativeProfile
} // namespace wiz3D
