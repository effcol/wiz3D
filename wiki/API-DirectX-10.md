# DirectX 10

⚠️ **In Progress.** The DX10 vendor proxy was revived for 0.4.0 (SR stub + Win11 SDK fix) and builds again. It inherits the DX11 scaffolding (same install folder, same enum, same OSD / hotkey system) but the DX10-specific analyser coverage is minimal, so no per-game profiles currently work on DX10-exclusive titles. **Testing DX10 titles is one of the biggest open contribution areas.**

## How the wrapper works

DX10 and DX11 share a single DLL set under `Stereo-Injector/DX10-11/{32,64}-bit/`. The proxy is `dxgi.dll` + `d3d10.dll` + `d3d11.dll` + supporting pieces — load order routes DX10 titles through `d3d10.dll` and DX11 titles through `d3d11.dll`, but both go through the same `SwapChain11Proxy` for composition.

The constant-buffer analyser / Feature Engine ("Akshay's PR #31") that lights up per-game stereo on DX11 ships for both APIs from 0.4.3 onward (task #91/#92 landed the DX10 feature-engine dispatch), but the per-game profiles in `BaseProfile.xml` were all authored against DX11 captures. DX10 games may need profile re-auditing to pick up the right CB slots.

## Install path

Same as DX11: `Stereo-Injector/DX10-11/32-bit/` or `Stereo-Injector/DX10-11/64-bit/`. Copy the contents (DLLs + `OutputMethods/` + `BaseProfile.xml` + `wiz3D_Config.xml`) next to the game's `.exe`.

## Games

Most titles tagged DX10/11 in the README ran on DX11 at test time. Pure-DX10 testing results are what we need. See [DirectX 11](API-DirectX-11) for the shared game list.

Please report DX10-exclusive titles via [GitHub Issues](https://github.com/effcol/WiZ3D/issues) so we can split them out here.

## Related

- The constant-buffer shift strategy for DX10 is tracked as task #62 (layout-aware CB shift in Buffer10Proxy).
