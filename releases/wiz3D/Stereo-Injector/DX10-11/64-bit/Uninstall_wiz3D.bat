@echo off
echo ============================================
echo  Wiz3D Uninstaller - DX10/DX11 (x64)
echo ============================================
echo.
echo This will remove ONLY wiz3D files from the current directory.
echo No game files will be modified or deleted.
echo.
echo Current directory: %CD%
echo.
pause

echo.
echo Removing wiz3D proxy and wrapper files...
if exist "dxgi.dll" del /f /q "dxgi.dll"
if exist "d3d9.dll" del /f /q "d3d9.dll"
if exist "d3d10.dll" del /f /q "d3d10.dll"
if exist "d3d11.dll" del /f /q "d3d11.dll"
if exist "nvapi64.dll" del /f /q "nvapi64.dll"
if exist "S3DWrapperD3D10.dll" del /f /q "S3DWrapperD3D10.dll"
if exist "S3DAPI.dll" del /f /q "S3DAPI.dll"
if exist "S3DDevIL.dll" del /f /q "S3DDevIL.dll"
if exist "S3Dilu.dll" del /f /q "S3Dilu.dll"
if exist "S3DUtils.dll" del /f /q "S3DUtils.dll"
if exist "ZLOg.dll" del /f /q "ZLOg.dll"

echo Removing wiz3D config files...
if exist "wiz3D_Config.xml" del /f /q "wiz3D_Config.xml"
if exist "BaseProfile.xml" del /f /q "BaseProfile.xml"
if exist "CommunityProfile.xml" del /f /q "CommunityProfile.xml"

echo Removing wiz3D runtime files...
if exist "Statistic.xml" del /f /q "Statistic.xml"
if exist "wiz3D_proxy.log" del /f /q "wiz3D_proxy.log"
if exist "wiz3D_frame_trace.log" del /f /q "wiz3D_frame_trace.log"
if exist "NvApiProxy.log" del /f /q "NvApiProxy.log"
if exist "wiz3D_crash.dmp" del /f /q "wiz3D_crash.dmp"

echo Removing OutputMethods folder...
if exist "OutputMethods" rmdir /s /q "OutputMethods"

echo Removing shader cache...
if exist "DX9ShaderDataCache" rmdir /s /q "DX9ShaderDataCache"

echo Removing wiz3D_Shaders folder (per-game shader dumps + Hunter captures)...
if exist "wiz3D_Shaders" rmdir /s /q "wiz3D_Shaders"

echo Removing wiz3D_Logs folder (per-game logs + shader dumps)...
if exist "wiz3D_Logs" rmdir /s /q "wiz3D_Logs"

echo.
echo Wiz3D files removed successfully.
echo.

REM Delete this uninstaller last
del /f /q "%~f0"
