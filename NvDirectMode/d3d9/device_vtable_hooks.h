/* NvDirectMode/d3d9 - layout-stable IDirect3DDevice9 vtable hot-patch
 *
 * Task #68 — extends task #61 from factory-only (IDirect3D9 vtable patch +
 * Device9Proxy wrap) to also patch the IDirect3DDevice9 vtable. Necessary
 * for games (GTA IV) that sniff the device's internals after CreateDevice
 * returns; with only the factory patch, the game receives our Device9Proxy
 * (different layout) and crashes inside Reset() when the sniff fails.
 *
 * When enabled (UseLayoutStableProxy=2), Hook_CreateDevice creates the real
 * device, registers shadow state in this module's primary slot, hot-patches
 * the device vtable's Reset / Present / GetBackBuffer slots (and the Ex
 * variants when isEx), and returns the *real* device pointer to the game.
 * Game's anti-tamper sniff sees real layout — passes. When game later calls
 * Reset/Present/GetBackBuffer, our hook fires with pThis = real device,
 * looks up shadow state, and does the doubled-BB / per-eye / composite work
 * inline (no Device9Proxy involved on this path).
 *
 * One game = one device assumption: a single primary state slot, same model
 * as Device9Proxy. Vtable patches are global — covers every IDirect3DDevice9
 * created by the (single) IDirect3D9 we patched.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

namespace NvDirectMode
{
    // Install the device vtable patch and register shadow state for this
    // device. Called from Hook_CreateDevice / Hook_CreateDeviceEx when
    // UseLayoutStableProxy >= 2. Returns true on success — caller should
    // then return the real device pointer to the game. Returns false if
    // patching fails — caller should fall back to wrapping with Device9Proxy.
    bool InstallDeviceVtablePatch(IDirect3DDevice9* realDevice, bool isEx,
                                   UINT logicalW, UINT logicalH);

    // Tear down all MinHook hooks — restores each patched function's
    // original prologue bytes. Safe to call on DLL_PROCESS_DETACH so that
    // if this DLL is unloaded while d3d9.dll stays resident, we don't
    // leave dangling jumps into freed memory.
    void UninstallDeviceVtablePatches();
}
