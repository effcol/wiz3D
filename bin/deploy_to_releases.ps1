# wiz3D - deploy_to_releases.ps1
#
# Copies freshly-built DLLs from:
#   - bin/Final Release/<arch>/           (S3DDriver.sln)
#   - wiz3D-proxy/bin/Release/<arch>/     (wiz3D-proxy.sln)
#   - NvDirectMode/bin/Release/<arch>/    (NvDirectMode.sln)
# into the appropriate releases/wiz3D/<api>/<arch>/ subfolders so a release is
# ready to install into a game directory. Run AFTER building all three slns.
#
# S3DDriver ships its Final Release build (FINAL_RELEASE, best optimization).
# The proxy ships its Release build (no Final Release config on that solution).
#
# This script does NOT handle:
# - The DX12 HD3D variant (D3d12VendorProxy) — no longer deployed; AmdQbProxy
#   covers DX12 games via the dxgi.dll path.
# - wiz3D-proxy-d3d12 / wiz3D-proxy-vulkan-1 (d3d12.dll, vulkan-1.dll) — M0a
#   passthrough plumbing only (see S3DWrapper12/PLAN.md). No stereo wrapper
#   DLL ships behind them yet, so they're dev-only and not deployed.
#
# Usage:
#   .\bin\deploy_to_releases.ps1                # both Win32 + x64
#   .\bin\deploy_to_releases.ps1 -Arch Win32    # x86 only
#   .\bin\deploy_to_releases.ps1 -Arch x64      # x64 only

[CmdletBinding()]
param(
    [ValidateSet('Win32', 'x64', 'both')]
    [string]$Arch = 'both',
    # Which S3DDriver.sln build config to pick up DLLs from. 'Final Release'
    # is the shipping default (optimised, and 0.4.0+ keeps zlog live by not
    # defining FINAL_RELEASE in FinalRelease.props). 'Release' is the local
    # diagnostic build that build_and_deploy.ps1 -Diagnostic produces.
    [ValidateSet('Final Release', 'Release')]
    [string]$MainBinDir = 'Final Release'
)

$repoRoot = Split-Path -Parent $PSScriptRoot
$archs = if ($Arch -eq 'both') { @('Win32', 'x64') } else { @($Arch) }

# Common dependency DLLs shared across most release subfolders.
#
# ZLOg.dll handling:
#   0.4.0+ keeps zlog live in both configs (FINAL_RELEASE no longer defined
#   in FinalRelease.props), so S3DAPI.dll statically imports ZLOg.dll in
#   every shipping deploy. Error 126 at wrapper load if ZLOg.dll is missing.
#   The ZLog Build.0 lines stay enabled for Debug/Release/Final Release in
#   S3DDriver.sln so the DLL is produced for every config.
$commonDeps    = @('S3DAPI.dll', 'S3DUtils.dll', 'ZLOg.dll')

# 0.4.0+: Inter TTFs are embedded in S3DWrapperD3D9.dll and S3DWrapperD3D10.dll
# as RT_RCDATA resources (see each wrapper's app.rc). No Fonts/ folder ships
# with the release tree. OFL.txt is also embedded as a resource for licence
# compliance.
# OpenGL wrapper ships no runtime deps -- all its imports are system DLLs
# (opengl32, gdi32, shlwapi) or SR-Lib's own DLLs that the user has already
# installed system-wide. Nothing else goes into OpenGL-Quad-Buffer-Stereo/.
$openglDeps    = @()

# Output methods supported by each render API. Most APIs ship the same set;
# OpenGL only ships the SR weave method (no other OGL implementations exist yet).
#
# Curation rationale: every output method has DX9 + DX10 source under
# OutputMethods/ and most build cleanly. We ship the ones targeting hardware
# someone might plausibly own today — modern HMDs / 3D TVs (SBS), passive
# 3D monitors (Interlaced), DLP-Link projectors (DLP3D / ATIDLP3D / 120Hz),
# generic and iZ3D shutter glasses (Shutter / S3DShutter / CMOShutter), the
# StereoMirror for soft-mirror dual-display rigs, anaglyph for the cardboard
# brigade, debug overlays (S3DMarked), and LeiaSR weave for SR panels.
# Source-only (not shipped): AMDHD3DOutput (its own native HD3D path,
# different code), Avitrid / DualProjection / Lenovo (variant) / Taerim /
# VR920 / Z800 — all target hardware that's effectively dead. Users who
# want any of the source-only ones can drop the built DLL from
# bin/Final Release/.../OutputMethods/ into their game folder manually.
$standardOMs = @(
    'AnaglyphOutput.dll',
    'ATIDLP3DOutput.dll',
    'CMOShutterOutput.dll',
    'DLP3DOutput.dll',
    'InterlacedOutput.dll',
    'S3D120HzProjectorsOutput.dll',
    'S3DMarkedOutput.dll',
    'S3DOutput.dll',
    'S3DShutterOutput.dll',
    'ShutterOutput.dll',
    'SideBySideOutput.dll',
    'SimulatedRealityWeaveOutput.dll',
    'StereoMirrorOutput.dll'
)
# OpenGL has no OutputMethod plugin loader — S3DWrapperOGL has its own built-in
# GLSL compositor selected by OutputMode (0-9: iZ3D/SBS/TAB/Crosseyed/Anaglyph
# variants/Line+Column Interleaved/Checkerboard). The previous deploy shipped
# SimulatedRealityWeaveOutput.dll here but nothing loaded it. SR weave for OGL
# will land as a native mode in S3DWrapperOGL itself, not as a plugin DLL.
$openglOMs = @()

function Copy-Files {
    param(
        [Parameter(Mandatory)] [string] $SrcDir,
        [Parameter(Mandatory)] [string] $DstDir,
        [Parameter(Mandatory)] [string[]] $Files,
        [Parameter(Mandatory)] [string] $Tag,
        [string[]] $FallbackDirs = @()
    )
    if (-not (Test-Path $DstDir)) {
        New-Item -ItemType Directory -Path $DstDir -Force | Out-Null
    }
    $copied  = 0
    $missing = @()
    foreach ($file in $Files) {
        $src = Join-Path $SrcDir $file
        if (-not (Test-Path $src)) {
            foreach ($fb in $FallbackDirs) {
                $cand = Join-Path $fb $file
                if (Test-Path $cand) { $src = $cand; break }
            }
        }
        $dst = Join-Path $DstDir $file
        if (Test-Path $src) {
            Copy-Item -Path $src -Destination $dst -Force
            $copied++
        } else {
            $missing += $file
        }
    }
    Write-Host ("  {0,-22}  copied {1,2}/{2,-2}  -> {3}" -f $Tag, $copied, $Files.Count, $DstDir)
    if ($missing.Count -gt 0) {
        foreach ($m in $missing) { Write-Host "      MISSING: $m" -ForegroundColor Yellow }
    }
}

foreach ($archName in $archs) {
    # 0.4.0+: release-tree leaf folders use '32-bit' / '64-bit' instead of
    # the compiler-oriented 'x86' / 'x64'. The internal $archName (Win32 /
    # x64) keeps its MSBuild-platform spelling; $archAlias is what every
    # release path uses on disk.
    $archAlias = if ($archName -eq 'Win32') { '32-bit' } else { '64-bit' }
    $outDirName = if ($archName -eq 'Win32') { 'Win32' } else { 'Win64' }
    $binDir    = Join-Path $repoRoot "bin\$MainBinDir\$outDirName"
    $omSrcDir  = Join-Path $binDir   'OutputMethods'
    # wiz3D-proxy.sln output (entry-point DLLs games actually load: d3d9.dll, etc)
    $proxyBinDir       = Join-Path $repoRoot "wiz3D-proxy\bin\Release\$archName"
    $nvDirectModeBin   = Join-Path $repoRoot "NvDirectMode\bin\Release\$archName"

    if (-not (Test-Path $binDir)) {
        Write-Warning "Skipping ${archName}: $binDir not found (build first?)"
        continue
    }

    Write-Host ""
    Write-Host "=== $archName -> $archAlias ===" -ForegroundColor Cyan

    # Stereo-Injector/ holds the iZ3D-derived software compositor family
    # (DX7-11, DX12, future Vulkan/OGL-injector siblings). Separate from the
    # vendor-passthrough paths (HD3D, 3D-Vision-Direct, OpenGL-Quad-Buffer-Stereo)
    # which get their own top-level folders because they drive different hardware.

    # --- DX7 (32-bit only — no x64 release) ---
    if ($archAlias -eq '32-bit') {
        $dst = Join-Path $repoRoot 'releases\wiz3D\Stereo-Injector\DX7'
        Copy-Files -SrcDir $binDir   -DstDir $dst                          `
                   -Files (@('S3DWrapperD3D7.dll', 'S3DWrapperD3D9.dll') + $commonDeps)  `
                   -Tag   'DX7 wrappers+deps'
        Copy-Files -SrcDir $omSrcDir -DstDir (Join-Path $dst 'OutputMethods') `
                   -Files $standardOMs -Tag 'DX7 OutputMethods'
        Copy-Files -SrcDir $proxyBinDir -DstDir $dst                          `
                   -Files @('d3d9.dll', 'ddraw.dll')                         `
                   -Tag   'DX7 proxies'
    }

    # --- DX8 (32-bit only) ---
    if ($archAlias -eq '32-bit') {
        $dst = Join-Path $repoRoot 'releases\wiz3D\Stereo-Injector\DX8'
        Copy-Files -SrcDir $binDir   -DstDir $dst                          `
                   -Files (@('S3DWrapperD3D8.dll', 'S3DWrapperD3D9.dll') + $commonDeps)  `
                   -Tag   'DX8 wrappers+deps'
        Copy-Files -SrcDir $omSrcDir -DstDir (Join-Path $dst 'OutputMethods') `
                   -Files $standardOMs -Tag 'DX8 OutputMethods'
        Copy-Files -SrcDir $proxyBinDir -DstDir $dst                          `
                   -Files @('d3d8.dll', 'd3d9.dll')                          `
                   -Tag   'DX8 proxies'
    }

    # --- DX9 (both archs) ---
    $dst = Join-Path $repoRoot "releases\wiz3D\Stereo-Injector\DX9\$archAlias"
    Copy-Files -SrcDir $binDir   -DstDir $dst                          `
               -Files (@('S3DWrapperD3D9.dll') + $commonDeps)          `
               -Tag   "DX9/$archAlias wrappers+deps"
    Copy-Files -SrcDir $omSrcDir -DstDir (Join-Path $dst 'OutputMethods') `
               -Files $standardOMs -Tag "DX9/$archAlias OutputMethods"
    Copy-Files -SrcDir $proxyBinDir -DstDir $dst                          `
               -Files @('d3d9.dll')                                       `
               -Tag   "DX9/$archAlias proxies"

    # --- DX10-11 (both archs) ---
    # d3d9.dll ships here (without S3DWrapperD3D9.dll) as an "early loader" for
    # DX11 games that still statically import d3d9 (GTA V's Epic install is the
    # repro case). The d3d9 proxy's DllMain detects the sibling-DX10/11-wrapper
    # layout and preloads dxgi.dll + d3d11.dll by path, so later by-name loads
    # resolve to our proxies instead of System32's.
    $dst = Join-Path $repoRoot "releases\wiz3D\Stereo-Injector\DX10-11\$archAlias"
    Copy-Files -SrcDir $binDir   -DstDir $dst                                          `
               -Files (@('S3DWrapperD3D10.dll') + $commonDeps)                         `
               -Tag   "DX10-11/$archAlias wrappers+deps"
    Copy-Files -SrcDir $omSrcDir -DstDir (Join-Path $dst 'OutputMethods')              `
               -Files $standardOMs -Tag "DX10-11/$archAlias OutputMethods"
    Copy-Files -SrcDir $proxyBinDir -DstDir $dst                                       `
               -Files @('d3d9.dll', 'd3d10.dll', 'd3d11.dll', 'dxgi.dll')              `
               -Tag   "DX10-11/$archAlias proxies"

    # --- opengl quad-buffer stereo (both archs) ---
    # Folder name spells out the capability: this wrapper captures native
    # OpenGL quad-buffer stereo (PFD_STEREO / GL_BACK_LEFT/RIGHT) from games
    # that produce it (id Tech 3/4, CAD apps, sims). It does NOT generate
    # stereo from mono OpenGL games.
    $dst = Join-Path $repoRoot "releases\wiz3D\OpenGL-Quad-Buffer-Stereo\$archAlias"
    Copy-Files -SrcDir $binDir   -DstDir $dst                          `
               -Files (@('S3DWrapperOGL.dll') + $openglDeps)           `
               -Tag   "OGL-QBS/$archAlias wrappers+deps"
    if ($openglOMs.Count -gt 0) {
        Copy-Files -SrcDir $omSrcDir -DstDir (Join-Path $dst 'OutputMethods') `
                   -Files $openglOMs -Tag "OGL-QBS/$archAlias OutputMethods"
    } else {
        Write-Host ("  {0,-22}  (none - OGL uses built-in modes)" -f "OGL-QBS/$archAlias OutputMethods")
    }
    Copy-Files -SrcDir $proxyBinDir -DstDir $dst                          `
               -Files @('opengl32.dll')                                   `
               -Tag   "OGL-QBS/$archAlias proxies"

    # --- NvDirectMode (3D Vision Direct Mode proxies) ---
    # Layout: releases/wiz3D/3D-Vision-Direct/<API>/<archAlias>/<dll>
    # Each leaf gets its API-specific proxy DLL; nvapi[64].dll gets spread
    # in by the Spread-File loop after the per-arch loops finish.
    if (Test-Path $nvDirectModeBin) {
        $ndmBase = Join-Path $repoRoot "releases\wiz3D\3D-Vision-Direct"
        if (-not (Test-Path $ndmBase)) { New-Item -ItemType Directory -Path $ndmBase -Force | Out-Null }
        # Top-level ReadMe — install instructions, test targets, known limits.
        # Source is checked-in at NvDirectMode\ReadMe.txt; release tree is
        # gitignored, so it gets re-laid each deploy.
        $ndmReadmeSrc = Join-Path $repoRoot 'NvDirectMode\ReadMe.txt'
        if (Test-Path $ndmReadmeSrc) {
            Copy-Item -Path $ndmReadmeSrc -Destination (Join-Path $ndmBase 'ReadMe.txt') -Force
        }

        # Per-leaf payload: proxy DLL + 3DVision_Config.xml + Uninstall_3DVisionDirect.bat.
        # nvapi[64].dll is laid down separately by the Spread-File pass below.
        $cfgSrc       = Join-Path $repoRoot 'NvDirectMode\3DVision_Config.xml'
        $uninstallSrc = Join-Path $repoRoot 'NvDirectMode\Uninstall_3DVisionDirect.bat'
        function Copy-NdmExtras {
            param([string]$LeafDir)
            if (Test-Path $cfgSrc)       { Copy-Item -Path $cfgSrc       -Destination $LeafDir -Force }
            if (Test-Path $uninstallSrc) { Copy-Item -Path $uninstallSrc -Destination $LeafDir -Force }
        }

        $dx9Dst    = Join-Path $ndmBase "DX9\$archAlias"
        $dx10Dst   = Join-Path $ndmBase "DX10\$archAlias"
        $dx11Dst   = Join-Path $ndmBase "DX11\$archAlias"
        # NOTE: no NvDirectMode/opengl release leaf. No shipped OpenGL game uses
        # NvAPI_Stereo_SetActiveEye (Direct Mode is DX-only in practice). OpenGL
        # games that want 3D Vision stereo go through a DX-bridge wrapper like
        # Helifax's OGL-3DVision-Wrapper, and our DX9/10/11 magic-header capture
        # picks that traffic up automatically. The opengl32 proxy stays in source
        # (NvDirectMode/opengl32/) but isn't deployed.

        Copy-Files -SrcDir $nvDirectModeBin -DstDir $dx9Dst   -Files @('d3d9.dll')    -Tag "NDM/DX9/$archAlias"
        Copy-NdmExtras -LeafDir $dx9Dst
        Copy-Files -SrcDir $nvDirectModeBin -DstDir $dx10Dst  -Files @('d3d10.dll')   -Tag "NDM/DX10/$archAlias"
        Copy-NdmExtras -LeafDir $dx10Dst
        # DX11 leaf gets BOTH d3d11.dll AND dxgi.dll — game can use either
        # D3D11CreateDeviceAndSwapChain (caught by d3d11.dll) or
        # CreateDXGIFactory -> CreateSwapChain (caught by dxgi.dll).
        Copy-Files -SrcDir $nvDirectModeBin -DstDir $dx11Dst  -Files @('d3d11.dll','dxgi.dll') -Tag "NDM/DX11/$archAlias"
        Copy-NdmExtras -LeafDir $dx11Dst
        # EOSStub ships alongside NvDirectMode-d3d11 x86 only. It replaces
        # EOSSDK-Win32-Shipping.dll to disable Epic Online Services and its
        # overlay, which crashes SR weave (see AmdQbProxy/HD3D_Troubleshooting_Notes.md).
        # x64 variant does not yet exist — the stub covers 32-bit games only.
        if ($archAlias -eq '32-bit') {
            Copy-Files -SrcDir $proxyBinDir -DstDir $dx11Dst -Files @('EOSSDK-Win32-Shipping.dll') -Tag "NDM/DX11/32-bit EOSStub"
        }
    } else {
        Write-Host ("  NDM/$archAlias            SKIP (NvDirectMode bin not built: $nvDirectModeBin)") -ForegroundColor Yellow
    }

    # --- HD3D (AMD HD3D vendor-path proxies) ---
    # atidxx32/64 (AmdQbProxy), atiadlxy/atiadlxx (AmdAdlProxy), dxgi
    # (DxgiVendorProxy), d3d11 (D3d11VendorProxy) — all built by S3DDriver.sln,
    # shipped from Final Release like the rest of that solution.
    # (Uninstall_HD3D.bat is checked in directly under the HD3D leaves.)
    $hd3dDst   = Join-Path $repoRoot "releases\wiz3D\HD3D\$archAlias"
    $hd3dFiles = if ($archAlias -eq '32-bit') {
        @('atidxx32.dll', 'atiadlxy.dll', 'dxgi.dll', 'd3d11.dll')
    } else {
        @('atidxx64.dll', 'atiadlxx.dll', 'dxgi.dll', 'd3d11.dll')
    }
    Copy-Files -SrcDir $binDir -DstDir $hd3dDst -Files $hd3dFiles -Tag "HD3D/$archAlias"

    # --- DX12 (Stereo-Injector family) ---
    # Shelved for 0.4.0 — wrapper isn't production-ready. Code preserved in-tree;
    # re-enable the block below when S3DWrapperD3D12 is ready to ship.
    # if ($archAlias -eq '64-bit') {
    #     $siDx12Dst = Join-Path $repoRoot "releases\wiz3D\Stereo-Injector\DX12\64-bit"
    #     Copy-Files -SrcDir $binDir -DstDir $siDx12Dst -Files @('atidxx64.dll','atiadlxx.dll','dxgi.dll','d3d12.dll') -Tag "DX12/x64 vendor"
    #     $s3dw12Src = Join-Path $repoRoot "S3DWrapper12\bin\Release\Win64"
    #     if (Test-Path $s3dw12Src) {
    #         Copy-Files -SrcDir $s3dw12Src -DstDir $siDx12Dst -Files @('S3DWrapperD3D12.dll') -Tag "DX12/x64 wrapper"
    #     }
    #     $eosX64Src = Join-Path $repoRoot "wiz3D-proxy\bin\Release\Win64"
    #     if (Test-Path (Join-Path $eosX64Src 'EOSSDK-Win64-Shipping.dll')) {
    #         $eosDst = Join-Path $siDx12Dst 'EOSStub'
    #         Copy-Files -SrcDir $eosX64Src -DstDir $eosDst -Files @('EOSSDK-Win64-Shipping.dll') -Tag "DX12/x64 EOSStub"
    #     }
    # }
}

# --- Spread nvapi[64].dll across all api subfolders that need it ---
# NvApiProxy builds with NvDirectMode.sln into NvDirectMode/bin/Release/<arch>/;
# every DX leaf ships a copy beside its proxy DLL.
# (Other OutputMethods incl. SimulatedRealityWeaveOutput build to the standard
#  bin/Final Release/<arch>/OutputMethods/ and are distributed by the loops above.)
function Spread-File {
    param(
        [Parameter(Mandatory)] [string] $SrcPath,
        [Parameter(Mandatory)] [string[]] $DstDirs,
        [Parameter(Mandatory)] [string] $Tag
    )
    if (-not (Test-Path $SrcPath)) {
        Write-Host ("  {0,-30}  SOURCE MISSING: {1}" -f $Tag, $SrcPath) -ForegroundColor Yellow
        return
    }
    foreach ($d in $DstDirs) {
        if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
        Copy-Item -Path $SrcPath -Destination $d -Force
    }
    Write-Host ("  {0,-30}  spread to {1} dirs" -f $Tag, $DstDirs.Count)
}

Write-Host ""
Write-Host "=== Spreading shared files ===" -ForegroundColor Cyan
$relRoot = Join-Path $repoRoot 'releases\wiz3D'

# --- BaseProfile.xml into every DX wrapper folder ---
$baseProfileSrc = Join-Path $repoRoot 'S3DDriverSetup\Content\BaseProfile.xml'
$profileTargets = @(
    "$relRoot\Stereo-Injector\DX7",
    "$relRoot\Stereo-Injector\DX8",
    "$relRoot\Stereo-Injector\DX9\32-bit",
    "$relRoot\Stereo-Injector\DX9\64-bit",
    "$relRoot\Stereo-Injector\DX10-11\32-bit",
    "$relRoot\Stereo-Injector\DX10-11\64-bit"
)
# DX12 leaf shelved for 0.4.0; BaseProfile fan-out skips it.
# if ($archs -contains 'x64') {
#     $profileTargets += "$relRoot\Stereo-Injector\DX12\64-bit"
# }
Spread-File -SrcPath $baseProfileSrc -DstDirs $profileTargets -Tag 'BaseProfile.xml'

# --- wiz3D_Config.xml into every Stereo-Injector leaf ---
# Source of truth is S3DDriverSetup\Content\Config.xml (what the installer /
# dev loop reads). Before 2026-10-18 the per-leaf release copies could drift
# -- user shipped with ShowFPS=1 for weeks because the leaf copies weren't
# wired through here. HD3D/OpenGL/DX12 keep their own XML schemas and are
# NOT touched by this spread.
$wiz3dConfigSrc = Join-Path $repoRoot 'S3DDriverSetup\Content\Config.xml'
$wiz3dConfigTargets = @(
    "$relRoot\Stereo-Injector\DX7",
    "$relRoot\Stereo-Injector\DX8",
    "$relRoot\Stereo-Injector\DX9\32-bit",
    "$relRoot\Stereo-Injector\DX9\64-bit",
    "$relRoot\Stereo-Injector\DX10-11\32-bit",
    "$relRoot\Stereo-Injector\DX10-11\64-bit"
)
foreach ($t in $wiz3dConfigTargets) {
    $dst = Join-Path $t 'wiz3D_Config.xml'
    if (Test-Path $t) {
        Copy-Item -Path $wiz3dConfigSrc -Destination $dst -Force
    }
}
# Trim DX10-11-only OutputMode listings from the DX7/8/9 Config.xml copies.
# Modes 2 (FullSBS), 3 (HalfTAB), 4 (FullTAB), 7 (ColumnIL) and 8 (Checker)
# have no DX7-9 plugin DLL, so listing them in a DX9 config comment misleads
# a user into picking a mode their wrapper can't deliver. The template keeps
# the full list; this strip is a DX7-9-only post-process.
foreach ($t in "$relRoot\Stereo-Injector\DX7",
               "$relRoot\Stereo-Injector\DX8",
               "$relRoot\Stereo-Injector\DX9\32-bit",
               "$relRoot\Stereo-Injector\DX9\64-bit") {
    $dst = Join-Path $t 'wiz3D_Config.xml'
    if (Test-Path $dst) {
        $xml = [IO.File]::ReadAllText($dst)
        $xml = $xml -replace '(?m)^\s*<!--\s*2 == "FullSBS".*-->\s*\r?\n', ''
        $xml = $xml -replace '(?m)^\s*<!--\s*3 == "HalfTAB".*-->\s*\r?\n', ''
        $xml = $xml -replace '(?m)^\s*<!--\s*4 == "FullTAB".*-->\s*\r?\n', ''
        $xml = $xml -replace '(?m)^\s*<!--\s*7 == "ColumnIL".*-->\s*\r?\n', ''
        $xml = $xml -replace '(?m)^\s*<!--\s*8 == "Checker".*-->\s*\r?\n', ''
        [IO.File]::WriteAllText($dst, $xml)
    }
}

# DisableFullscreenModeEmulation=1 across every modern Stereo-Injector path.
# iZ3D set this to 0 originally because its shutter output needed games
# windowed so the shutter driver could blit over the window surface.
# Nobody runs that hardware in 2026, and the forced window costs: Metro 2033
# Redux rebuilt its DX11 swap chain every second frame (issue #34), and
# plenty of DX9 games have historically misbehaved with forced-windowed too.
# The DX9 wrapper still has shutter output code (SHUTTER_MODE_ATIQB,
# g_KMShutter) that force-sets this flag to FALSE at runtime when a shutter
# mode is actually selected, so flipping the default to 1 is safe: anyone
# who deliberately turns on OutputMethodDll="ATIQBOutput" or similar gets
# the windowed behaviour at launch anyway.
foreach ($t in "$relRoot\Stereo-Injector\DX7",
               "$relRoot\Stereo-Injector\DX8",
               "$relRoot\Stereo-Injector\DX9\32-bit",
               "$relRoot\Stereo-Injector\DX9\64-bit",
               "$relRoot\Stereo-Injector\DX10-11\32-bit",
               "$relRoot\Stereo-Injector\DX10-11\64-bit") {
    $dst = Join-Path $t 'wiz3D_Config.xml'
    if (Test-Path $dst) {
        $xml = [IO.File]::ReadAllText($dst) -replace '<DisableFullscreenModeEmulation Value="0"/>', '<DisableFullscreenModeEmulation Value="1"/>'
        [IO.File]::WriteAllText($dst, $xml)
    }
}

# Local-only diagnostic defaults: flip the three shader-authoring switches
# (CollectDebugInformation / DumpVS / DumpPS) from 0 -> 1 in the deployed
# Stereo-Injector configs so a local test run always captures CB-matrix
# telemetry + the DX{N}ShaderDataCache bulk shader dumps that make
# ShaderHunter captures useful to cross-reference. We detect CI via
# GITHUB_ACTIONS (always set in GitHub Actions runners); any build that
# isn't CI gets the three diagnostic flips. The source Config.xml stays at
# =0 so shipped CI artifacts match the user-facing default (no debug cost
# for players; the per-game dumps are large and would waste disk).
$isCI = ($env:GITHUB_ACTIONS -eq 'true') -or ($env:CI -eq 'true')
if (-not $isCI) {
    foreach ($t in $wiz3dConfigTargets) {
        $dst = Join-Path $t 'wiz3D_Config.xml'
        if (Test-Path $dst) {
            $xml = [IO.File]::ReadAllText($dst) `
                -replace '<CollectDebugInformation Value="0"/>', '<CollectDebugInformation Value="1"/>' `
                -replace '<DumpVS Value="0"/>', '<DumpVS Value="1"/>' `
                -replace '<DumpPS Value="0"/>', '<DumpPS Value="1"/>'
            [IO.File]::WriteAllText($dst, $xml)
        }
    }
    Write-Host "  CollectDebugInformation + DumpVS + DumpPS = 1 (local build; CI deploys keep the defaults 0)"
}
Write-Host ("  {0,-30}  spread to {1} dirs" -f 'wiz3D_Config.xml', $wiz3dConfigTargets.Count)

# --- Root docs: license + notices + user quick-start ---
# Canonical copies live at the repo root (they're part of the GitHub repo);
# the release root gets LICENSE renamed to LICENSE.txt for Windows users.
Copy-Item -Path (Join-Path $repoRoot 'LICENSE')                  -Destination (Join-Path $relRoot 'LICENSE.txt') -Force
Copy-Item -Path (Join-Path $repoRoot 'THIRD_PARTY_NOTICES.txt')  -Destination $relRoot -Force
Copy-Item -Path (Join-Path $repoRoot 'S3DDriverSetup\Content\release-readme.txt') -Destination (Join-Path $relRoot 'README.txt') -Force
Write-Host ("  {0,-30}  copied to release root" -f 'LICENSE.txt, NOTICES, README')

# --- nvapi[64].dll across DX subfolders that need it ---
foreach ($archName in $archs) {
    $archAlias = if ($archName -eq 'Win32') { '32-bit' } else { '64-bit' }
    $nvapiName = if ($archAlias -eq '32-bit') { 'nvapi.dll' } else { 'nvapi64.dll' }
    $srcNvapi  = Join-Path $repoRoot "NvDirectMode\bin\Release\$archName\$nvapiName"
    # nvapi spreads into:
    #   - regular wiz3D DX9 and DX10-11 (3D Vision-aware games using passive)
    #   - all three 3D-Vision-Direct/<API>/<arch>/ leaves (Direct Mode games need NvApiProxy
    #     beside the NvDirectMode proxy DLL because they call NvAPI_Stereo_SetActiveEye etc.)
    # NOT into OpenGL-Quad-Buffer-Stereo: the OGL wrapper never touches NvAPI.
    $nvapiTargets = @(
        "$relRoot\Stereo-Injector\DX9\$archAlias",
        "$relRoot\Stereo-Injector\DX10-11\$archAlias",
        "$relRoot\3D-Vision-Direct\DX9\$archAlias",
        "$relRoot\3D-Vision-Direct\DX10\$archAlias",
        "$relRoot\3D-Vision-Direct\DX11\$archAlias"
    )
    Spread-File -SrcPath $srcNvapi -DstDirs $nvapiTargets -Tag "$nvapiName ($archAlias)"
}

Write-Host ""
Write-Host "Deploy complete." -ForegroundColor Green
