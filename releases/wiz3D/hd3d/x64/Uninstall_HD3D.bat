@echo off
echo ============================================
echo  Wiz3D HD3D Uninstaller (x64)
echo ============================================
echo.
echo This will remove ONLY wiz3D HD3D proxy files from the current directory.
echo No game files will be modified or deleted.
echo.
echo Current directory: %CD%
echo.
pause

echo.
echo Removing AMD QB / ADL proxy files...
if exist "atidxx64.dll" del /f /q "atidxx64.dll"
if exist "atiadlxx.dll" del /f /q "atiadlxx.dll"
if exist "d3d11.dll"    del /f /q "d3d11.dll"
if exist "dxgi.dll"     del /f /q "dxgi.dll"

echo Removing HD3D config files...
if exist "HD3D_Config.xml" del /f /q "HD3D_Config.xml"

echo Removing HD3D runtime files...
if exist "HD3D_atidxx.log" del /f /q "HD3D_atidxx.log"
if exist "HD3D_dxgi.log"   del /f /q "HD3D_dxgi.log"
if exist "HD3D_adl.log"    del /f /q "HD3D_adl.log"
REM Legacy log name from wiz3D 0.2.x (renamed to HD3D_dxgi.log in 0.3.0):
if exist "3DV_dxgi.log"    del /f /q "3DV_dxgi.log"

echo.
echo Wiz3D HD3D files removed successfully.
echo.

REM Delete this uninstaller last
del /f /q "%~f0"
