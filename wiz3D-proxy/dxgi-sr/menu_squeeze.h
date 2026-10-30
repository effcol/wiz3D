/*
 * wiz3D Simulated Reality -- menu-squeeze module
 *
 * Fixes CP2077's "menus render mono inside SBSDebug stereo" symptom:
 * the game's own stereo-rendering path writes half-SBS to its own
 * backbuffer for 3D frames and writes full-width flat content for
 * menu / cutscene / loading / UI frames. On a half-SBS display pipeline
 * those flat frames render as mono. The squeeze detects flat frames
 * and remaps the full-width content into BOTH halves of the backbuffer
 * at half-width each, so the display sees "flat but in both eyes" --
 * which through an SBS stereo pipeline reads as zero-disparity flat UI
 * pinned to screen depth. Exactly what you want for menus.
 *
 * Detection: diffs the Device vtable call counts from our Odyssey3DSDK
 * stand-in each present. On CP2077, GetEyePosition is polled ONLY on
 * stereo-rendering frames; its delta tells us flat-vs-stereo cleanly
 * with no GPU readback. We import the counter via GetProcAddress on
 * our own Odyssey3DSDK.dll once it's loaded by the game. Reference
 * for the heuristic: vl3c/cyberpunk2077-3d-braindance-edition
 * (MIT 2026); we re-implement clean-room against that spec.
 *
 * DX12 pipeline: lazy-init on first squeeze -- intermediate texture
 * (one copy of the backbuffer), 1-SRV descriptor heap, 1-slot RTV
 * heap, static-sampler root signature, graphics PSO, ring of 4
 * command allocators + 1 command list + 1 fence. All resources
 * allocated on the game's own ID3D12Device and submitted to the
 * game's direct ID3D12CommandQueue (captured from the first arg of
 * IDXGIFactory2::CreateSwapChainForHwnd -- on DX12 games that first
 * IUnknown* is the direct command queue).
 */

#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>

namespace wiz3d_sr {

// Called once from the dxgi.dll bootstrap's DllMain(DLL_PROCESS_ATTACH).
// Initialises the module's global state. Does not touch the game's
// D3D12 device yet -- that happens lazily on the first flat-frame
// squeeze, once we have the swap chain + queue.
void MenuSqueezeInit();

// Called once from DllMain(DLL_PROCESS_DETACH). Cleans up any pipeline
// resources we created on the game's device.
void MenuSqueezeShutdown();

// Called after the bootstrap successfully forwards CreateDXGIFactory /
// CreateDXGIFactory1 / CreateDXGIFactory2 to the real dxgi.dll with
// the returned factory. We patch the factory's CreateSwapChain*
// vtable slots so we can intercept the next CreateSwapChain* call,
// capture the IDXGISwapChain + ID3D12CommandQueue the game is using,
// then patch Present/Present1 on the returned swap chain.
//
// Safe to call multiple times with the same or different factories --
// each gets patched independently.
void OnFactoryCreated(IDXGIFactory* factory);

}  // namespace wiz3d_sr
