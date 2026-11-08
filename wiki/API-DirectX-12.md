# DirectX 12

⏳ **Coming soon.** No shipping DX12 Stereo-Injector wrapper yet.

## What's planned

- **HD3D DX12 resurrection** — port the old iZ3D DX12 path back to modern Windows / modern toolchains, including an `IDXGISwapChain3` wrapper. Tracked as task #20 in the backlog.
- **DLSS-in-stereo for HD3D DX12 titles** — per-game XML profiles to get DLSS working correctly in stereo. Tracked as task #28.

## Why DX12 is harder than DX11

- Explicit command-list model: constant-buffer edits have to happen before the list is submitted, not between draws.
- Resource states and barriers must be managed by the wrapper when it injects its own draws.
- Multi-queue execution complicates ordering guarantees around per-eye rendering.

The DX11 Feature Engine landed in 0.4.0 took time to get right in a much simpler API. A DX12 equivalent is in the plan but there's no ETA yet.

## Known DX12 titles people ask about

- **Sniper Elite 4** — ships DX11 and DX12. DX11 works via [HD3D](Native-HD3D); DX12 doesn't currently trigger HD3D stereo.
- **World of Warcraft** — 3D support was removed in the 2018 DX12 update; nothing we can do in-wrapper.
- **Shadow of the Tomb Raider** — DX12-capable; probing for HD3D support is tracked as task #27.

## Interim

For DX12 titles you want to see in stereo today, check whether the game offers a DX11 fallback (many do, in the launcher settings) and use [DirectX 11](API-DirectX-11) with that. Otherwise, [Geo-11](https://3dvision-blog.com/forum/viewtopic.php?f=20&t=5523) is often the active fork of 3Dmigoto that targets DX11-era shader fixes and may cover some DX12 titles via injection; see the [Ecosystem Map](project_ecosystem_map.md) in the repo for project positioning.

Want to watch or help? Track task #20 and task #28 in the backlog.
