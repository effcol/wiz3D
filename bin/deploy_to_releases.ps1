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
    [string]$Arch = 'both'
)

$repoRoot = Split-Path -Parent $PSScriptRoot
$archs = if ($Arch -eq 'both') { @('Win32', 'x64') } else { @($Arch) }

# Common dependency DLLs shared across most release subfolders
$commonDeps    = @('S3DAPI.dll', 'S3DUtils.dll', 'ZLOg.dll')
# OpenGL wrapper only ships ZLOg.dll (statically imported via zlog::VldReportHook).
# It has no LoadLibrary calls and never references S3DAPI/S3DUtils — those are
# DX-wrapper helpers. nvapi is also excluded (no NvAPI usage on the OGL path).
$openglDeps    = @('ZLOg.dll')

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
    $archAlias = if ($archName -eq 'Win32') { 'x86' } else { 'x64' }
    $outDirName = if ($archName -eq 'Win32') { 'Win32' } else { 'Win64' }
    $binDir    = Join-Path $repoRoot "bin\Final Release\$outDirName"
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

    # --- dx7 (32-bit only — no x64 release) ---
    if ($archAlias -eq 'x86') {
        $dst = Join-Path $repoRoot 'releases\wiz3D\dx7'
        Copy-Files -SrcDir $binDir   -DstDir $dst                          `
                   -Files (@('S3DWrapperD3D7.dll', 'S3DWrapperD3D9.dll') + $commonDeps)  `
                   -Tag   'dx7 wrappers+deps'
        Copy-Files -SrcDir $omSrcDir -DstDir (Join-Path $dst 'OutputMethods') `
                   -Files $standardOMs -Tag 'dx7 OutputMethods'
        Copy-Files -SrcDir $proxyBinDir -DstDir $dst                          `
                   -Files @('d3d9.dll', 'ddraw.dll')                         `
                   -Tag   'dx7 proxies'
    }

    # --- dx8 (32-bit only) ---
    if ($archAlias -eq 'x86') {
        $dst = Join-Path $repoRoot 'releases\wiz3D\dx8'
        Copy-Files -SrcDir $binDir   -DstDir $dst                          `
                   -Files (@('S3DWrapperD3D8.dll', 'S3DWrapperD3D9.dll') + $commonDeps)  `
                   -Tag   'dx8 wrappers+deps'
        Copy-Files -SrcDir $omSrcDir -DstDir (Join-Path $dst 'OutputMethods') `
                   -Files $standardOMs -Tag 'dx8 OutputMethods'
        Copy-Files -SrcDir $proxyBinDir -DstDir $dst                          `
                   -Files @('d3d8.dll', 'd3d9.dll')                          `
                   -Tag   'dx8 proxies'
    }

    # --- dx9 (both archs) ---
    $dst = Join-Path $repoRoot "releases\wiz3D\dx9\$archAlias"
    Copy-Files -SrcDir $binDir   -DstDir $dst                          `
               -Files (@('S3DWrapperD3D9.dll') + $commonDeps)          `
               -Tag   "dx9/$archAlias wrappers+deps"
    Copy-Files -SrcDir $omSrcDir -DstDir (Join-Path $dst 'OutputMethods') `
               -Files $standardOMs -Tag "dx9/$archAlias OutputMethods"
    Copy-Files -SrcDir $proxyBinDir -DstDir $dst                          `
               -Files @('d3d9.dll')                                       `
               -Tag   "dx9/$archAlias proxies"

    # --- dx10-11 (both archs) ---
    $dst = Join-Path $repoRoot "releases\wiz3D\dx10-11\$archAlias"
    Copy-Files -SrcDir $binDir   -DstDir $dst                                          `
               -Files (@('S3DWrapperD3D10.dll') + $commonDeps)                         `
               -Tag   "dx10-11/$archAlias wrappers+deps"
    Copy-Files -SrcDir $omSrcDir -DstDir (Join-Path $dst 'OutputMethods')              `
               -Files $standardOMs -Tag "dx10-11/$archAlias OutputMethods"
    Copy-Files -SrcDir $proxyBinDir -DstDir $dst                                       `
               -Files @('d3d10.dll', 'd3d11.dll', 'dxgi.dll')                          `
               -Tag   "dx10-11/$archAlias proxies"

    # --- opengl quad-buffer stereo (both archs) ---
    # Folder name spells out the capability: this wrapper captures native
    # OpenGL quad-buffer stereo (PFD_STEREO / GL_BACK_LEFT/RIGHT) from games
    # that produce it (id Tech 3/4, CAD apps, sims). It does NOT generate
    # stereo from mono OpenGL games.
    $dst = Join-Path $repoRoot "releases\wiz3D\opengl-quad-buffer-stereo\$archAlias"
    Copy-Files -SrcDir $binDir   -DstDir $dst                          `
               -Files (@('S3DWrapperOGL.dll') + $openglDeps)           `
               -Tag   "opengl-qbs/$archAlias wrappers+deps"
    if ($openglOMs.Count -gt 0) {
        Copy-Files -SrcDir $omSrcDir -DstDir (Join-Path $dst 'OutputMethods') `
                   -Files $openglOMs -Tag "opengl-qbs/$archAlias OutputMethods"
    } else {
        Write-Host ("  {0,-22}  (none - OGL uses built-in modes)" -f "opengl-qbs/$archAlias OutputMethods")
    }
    Copy-Files -SrcDir $proxyBinDir -DstDir $dst                          `
               -Files @('opengl32.dll')                                   `
               -Tag   "opengl-qbs/$archAlias proxies"

    # --- NvDirectMode (3D Vision Direct Mode proxies) ---
    # Layout: releases/wiz3D/3d-vision-direct/<api>/<archAlias>/<dll>
    # Each leaf gets its API-specific proxy DLL; nvapi[64].dll gets spread
    # in by the Spread-File loop after the per-arch loops finish.
    if (Test-Path $nvDirectModeBin) {
        $ndmBase = Join-Path $repoRoot "releases\wiz3D\3d-vision-direct"
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

        $dx9Dst    = Join-Path $ndmBase "dx9\$archAlias"
        $dx10Dst   = Join-Path $ndmBase "dx10\$archAlias"
        $dx11Dst   = Join-Path $ndmBase "dx11\$archAlias"
        # NOTE: no NvDirectMode/opengl release leaf. No shipped OpenGL game uses
        # NvAPI_Stereo_SetActiveEye (Direct Mode is DX-only in practice). OpenGL
        # games that want 3D Vision stereo go through a DX-bridge wrapper like
        # Helifax's OGL-3DVision-Wrapper, and our DX9/10/11 magic-header capture
        # picks that traffic up automatically. The opengl32 proxy stays in source
        # (NvDirectMode/opengl32/) but isn't deployed.

        Copy-Files -SrcDir $nvDirectModeBin -DstDir $dx9Dst   -Files @('d3d9.dll')    -Tag "ndm/dx9/$archAlias"
        Copy-NdmExtras -LeafDir $dx9Dst
        Copy-Files -SrcDir $nvDirectModeBin -DstDir $dx10Dst  -Files @('d3d10.dll')   -Tag "ndm/dx10/$archAlias"
        Copy-NdmExtras -LeafDir $dx10Dst
        # dx11 leaf gets BOTH d3d11.dll AND dxgi.dll — game can use either
        # D3D11CreateDeviceAndSwapChain (caught by d3d11.dll) or
        # CreateDXGIFactory -> CreateSwapChain (caught by dxgi.dll).
        Copy-Files -SrcDir $nvDirectModeBin -DstDir $dx11Dst  -Files @('d3d11.dll','dxgi.dll') -Tag "ndm/dx11/$archAlias"
        Copy-NdmExtras -LeafDir $dx11Dst
        # EOSStub ships alongside NvDirectMode-d3d11 x86 only. It replaces
        # EOSSDK-Win32-Shipping.dll to disable Epic Online Services and its
        # overlay, which crashes SR weave (see AmdQbProxy/HD3D_Troubleshooting_Notes.md).
        # x64 variant does not yet exist — the stub covers 32-bit games only.
        if ($archAlias -eq 'x86') {
            Copy-Files -SrcDir $proxyBinDir -DstDir $dx11Dst -Files @('EOSSDK-Win32-Shipping.dll') -Tag "ndm/dx11/x86 EOSStub"
        }
    } else {
        Write-Host ("  ndm/$archAlias            SKIP (NvDirectMode bin not built: $nvDirectModeBin)") -ForegroundColor Yellow
    }

    # --- hd3d (AMD HD3D vendor-path proxies) ---
    # atidxx32/64 (AmdQbProxy), atiadlxy/atiadlxx (AmdAdlProxy), dxgi
    # (DxgiVendorProxy), d3d11 (D3d11VendorProxy) — all built by S3DDriver.sln,
    # shipped from Final Release like the rest of that solution.
    # (Uninstall_HD3D.bat is checked in directly under the hd3d leaves.)
    $hd3dDst   = Join-Path $repoRoot "releases\wiz3D\hd3d\$archAlias"
    $hd3dFiles = if ($archAlias -eq 'x86') {
        @('atidxx32.dll', 'atiadlxy.dll', 'dxgi.dll', 'd3d11.dll')
    } else {
        @('atidxx64.dll', 'atiadlxx.dll', 'dxgi.dll', 'd3d11.dll')
    }
    Copy-Files -SrcDir $binDir -DstDir $hd3dDst -Files $hd3dFiles -Tag "hd3d/$archAlias"
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
    "$relRoot\dx7",
    "$relRoot\dx8",
    "$relRoot\dx9\x86",
    "$relRoot\dx9\x64",
    "$relRoot\dx10-11\x86",
    "$relRoot\dx10-11\x64"
)
Spread-File -SrcPath $baseProfileSrc -DstDirs $profileTargets -Tag 'BaseProfile.xml'

# --- Root docs: license + notices + user quick-start ---
# Canonical copies live at the repo root (they're part of the GitHub repo);
# the release root gets LICENSE renamed to LICENSE.txt for Windows users.
Copy-Item -Path (Join-Path $repoRoot 'LICENSE')                  -Destination (Join-Path $relRoot 'LICENSE.txt') -Force
Copy-Item -Path (Join-Path $repoRoot 'THIRD_PARTY_NOTICES.txt')  -Destination $relRoot -Force
Copy-Item -Path (Join-Path $repoRoot 'README.txt')               -Destination $relRoot -Force
Write-Host ("  {0,-30}  copied to release root" -f 'LICENSE.txt, NOTICES, README')

# --- nvapi[64].dll across DX subfolders that need it ---
foreach ($archName in $archs) {
    $archAlias = if ($archName -eq 'Win32') { 'x86' } else { 'x64' }
    $nvapiName = if ($archAlias -eq 'x86') { 'nvapi.dll' } else { 'nvapi64.dll' }
    $srcNvapi  = Join-Path $repoRoot "NvDirectMode\bin\Release\$archName\$nvapiName"
    # nvapi spreads into:
    #   - regular wiz3D dx9 and dx10-11 (3D Vision-aware games using passive)
    #   - all three 3d-vision-direct/<api>/<arch>/ leaves (Direct Mode games need NvApiProxy
    #     beside the NvDirectMode proxy DLL because they call NvAPI_Stereo_SetActiveEye etc.)
    # NOT into opengl-quad-buffer-stereo: the OGL wrapper never touches NvAPI.
    $nvapiTargets = @(
        "$relRoot\dx9\$archAlias",
        "$relRoot\dx10-11\$archAlias",
        "$relRoot\3d-vision-direct\dx9\$archAlias",
        "$relRoot\3d-vision-direct\dx10\$archAlias",
        "$relRoot\3d-vision-direct\dx11\$archAlias"
    )
    Spread-File -SrcPath $srcNvapi -DstDirs $nvapiTargets -Tag "$nvapiName ($archAlias)"
}

Write-Host ""
Write-Host "Deploy complete." -ForegroundColor Green
