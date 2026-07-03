#include "..\bin\temp_version.h"
#include "..\Shared\ProductNames.h"

#define VERSION_NAME_SUFFIX ""

#define VERSION					Str(PRODUCT_VERSION_MAJOR) + "." + Str(PRODUCT_VERSION_MINOR) + "." + Str(PRODUCT_VERSION_QFE)
#define DISPLAYED_VERSION       Str(PRODUCT_VERSION_MAJOR) + "." + Str(PRODUCT_VERSION_MINOR) + VERSION_NAME_SUFFIX + "(" + Str(PRODUCT_VERSION_QFE) + ")"

#ifndef setupType
	// Anaglyph, CMO
	#define setupType = "iZ3D"
	//#define setupType = "CMO"
#endif

#if setupType == "iZ3D"
	#define EDITION ""
  #define InstallKMDriver
#elif setupType == "Anaglyph"
	#define EDITION ".Anaglyph"
#elif setupType == "CMO"
	#define EDITION ".CMI"
#elif setupType == "Lenovo"
	#define EDITION ".Lenovo"
#elif setupType == "YGT"
	#define EDITION ".YGT"
#elif setupType == "Taerim"
	#define EDITION ".3D24InchLED"
#endif

#ifndef OUTDIR   
  #ifndef DEBUG
    #define OUTDIR "..\bin\Final Release\"  
  #else        
    #define OUTDIR "..\bin\Release\"
  #endif
#endif
#ifndef FILESDIR
	#define FILESDIR OUTDIR
#endif
#ifndef SETUPFILESDIR
	#define SETUPFILESDIR "..\bin"
#endif

#ifndef DEBUG
	#if setupType != "Anaglyph"
		#define FINAL_RELEASE FILESDIR
	#else
		#define FINAL_RELEASE FILESDIR + "\..\final release"
	#endif
	#define POSTFIX ""
#else
	#define FINAL_RELEASE FILESDIR + "\..\final release"
	#define POSTFIX ".[debug]"
#endif

[Setup]
AppName={#FULL_PRODUCT_NAME}
AppVerName={#DISPLAYED_VERSION}
AppPublisher={#COMPANY_NAME}
AppPublisherURL={#COMPANY_SITE}
AppVersion={#DISPLAYED_VERSION}
DefaultDirName={pf}\{#PRODUCT_NAME}
DefaultGroupName={#PRODUCT_NAME}
DisableWelcomePage=true
PrivilegesRequired=admin
AllowNoIcons=true
OutputDir=..\BuildSetup\iZ3D
OutputBaseFilename={#SETUP_FILE_NAME}{#EDITION}.{#DISPLAYED_VERSION}{#POSTFIX}
//UninstallDisplayIcon={app}\Control center.exe
UninstallDisplayIcon={app}\ControlCenter.exe
UninstallDisplayName={#PRODUCT_NAME} Remove
MinVersion=0,5.01.2600
// ToDo: Doesn't really match to use 4 byte Git commit numbers here.
//  but otherwise this fails
VersionInfoVersion={#VERSION}
VersionInfoCompany={#COMPANY_NAME}
VersionInfoDescription={#PRODUCT_NAME}
VersionInfoTextVersion={#VERSION}
VersionInfoCopyright={#COMPANY_NAME}
SetupLogging=false
WizardImageFile=SetupModern.bmp
Compression=lzma2/Ultra
SolidCompression=true
InternalCompressLevel=Ultra
ShowLanguageDialog=yes
LanguageDetectionMethod=locale
AlwaysShowComponentsList=false
AllowCancelDuringInstall=false
WizardSmallImageFile=SetupModernSmall.bmp
LicenseFile=Licenses\License-EN.rtf
InfoBeforeFile={#SETUPFILESDIR}\ChangeLog.rtf
AppSupportURL=http://iz3d.com/support/
AppUpdatesURL=http://www.iz3d.com/driver
AppID={{30BEF9F2-CD3F-4B13-9E5C-BFE2F9544572}
//SignTool=signtool sign /f ..\lib\sign\verisign.pfx /p 1 /t http://timestamp.verisign.com/scripts/timstamp.dll $f

[CustomMessages]
ShortProductName=iZ3D

[Languages]
Name: english; MessagesFile: compiler:Default.isl, Languages\Default.isl
Name: chineseSimp; MessagesFile: compiler:Languages\ChineseSimp.isl; LicenseFile: Licenses\License-SC.rtf
Name: chineseTrad; MessagesFile: compiler:Languages\ChineseTrad.isl; LicenseFile: Licenses\License-TC.rtf
#if setupType != "CMO"
Name: russian; MessagesFile: compiler:Languages\Russian.isl, Languages\Russian.isl
Name: ukrainian; MessagesFile: compiler:Languages\Ukrainian.isl
Name: german; MessagesFile: compiler:Languages\German.isl
Name: italian; MessagesFile: compiler:Languages\Italian.isl
Name: french; MessagesFile: compiler:Languages\French.isl
Name: spanish; MessagesFile: compiler:Languages\Spanish.isl
Name: japanese; MessagesFile: compiler:Languages\Japanese.isl 
#endif

[Registry]
Root: HKLM; Subkey: SOFTWARE\iZ3D\iZ3D Driver; ValueType: string; ValueData: {#VERSION}; Flags: uninsdeletekey; ValueName: CurrentVersion

[Types]
#if setupType == "iZ3D"
Name: full; Description: {cm:FullInstallation}
Name: custom; Description: {cm:CustomInstallation}; Flags: iscustom
#elif setupType == "CMO"
Name: full; Description: {cm:FullInstallation}
Name: Anaglyph; Description: {cm:Anaglyph}
#else
Name: full; Description: {cm:FullInstallation}
#endif

[Components]
#if setupType == "iZ3D"
Name: Drivers; Description: {cm:Drivers}; Types: full custom
Name: Drivers\D3D89Driver; Description: {cm:D3D89Driver}; Types: full custom; Flags: fixed
Name: Drivers\D3D10Driver; Description: {cm:D3D10Driver}; Types: full; MinVersion: 0,6.0.6000
//Name: Drivers\OGLQBDriver; Description: {code:cm2|OpenGLQBDriver}; Types: OGLQBDriver full
Name: Outputs; Description: {cm:Outputs}; Types: full
Name: Outputs\S3DOutput; Description: {code:cm2|S3DOutput}; Types: full
Name: Outputs\iZShutterOutput; Description: {cm:iZShutterOutput}; Types: full
Name: Outputs\AnaglyphOutput; Description: {cm:AnaglyphOutput}; Types: full custom; Flags: fixed
Name: Outputs\ATIDLP3DOutput; Description: {cm:ATIDLP3DOutput}; Types: full
//Name: Outputs\ColorCodeOutput; Description: {cm:ColorCodeOutput}; Types: full
Name: Outputs\ZalmanOutput; Description: {cm:ZalmanOutput}; Types: full
Name: Outputs\InterlacedOutput; Description: {cm:InterlacedOutput}; Types: full
Name: Outputs\DLP3DOutput; Description: {cm:DLP3DOutput}; Types: full
//Name: Outputs\S3DShutterOutput; Description: {code:cm2|S3DShutterOutput}; Types: full
Name: Outputs\DualProjOutput; Description: {cm:DualProjectionOutput}; Types: full
//Name: Outputs\ShutterOutput; Description: {cm:ShutterOutput}; Types: full
Name: Outputs\StereoMirrorOutput; Description: {cm:StereoMirrorOutput}; Types: full
Name: Outputs\SideBySideOutput; Description: {cm:SideBySideOutput}; Types: full
Name: Outputs\VR920Output; Description: {cm:VR920Output}; Types: full
//Name: Outputs\Z800Output; Description: {cm:Z800Output}; Types: full
#ifdef DEBUG
Name: Outputs\CMOShutterOutput; Description: {cm:CMOShutterOutput}; Types: full
Name: Outputs\AvitridOutput; Description: {cm:AvitridOutput}; Types: full
Name: Outputs\TaerimOutput; Description: {cm:TaerimOutput}; Types: full
#endif
//Name: Outputs\Shutter120HzOutput; Description: {cm:Shutter120HzOutput}; Types: full
Name: Outputs\S3D120HzProjectorsOutput; Description: {cm:S3D120HzProjectorsOutput}; Types: full
Name: Outputs\AMDHD3DOutput; Description: {cm:AMDHD3DOutput}; Types: full
Name: S3DStereoWallpaperCreator; Description: {code:cm2|S3DStereoWallpaperCreator}; Types: full
#elif setupType == "CMO"
Name: Drivers; Description: {cm:Drivers}; Types: Anaglyph full
Name: Drivers\D3D89Driver; Description: {cm:D3D89Driver}; Types: Anaglyph full; Flags: fixed 
Name: Drivers\D3D10Driver; Description: {cm:D3D10Driver}; Types: Anaglyph full; MinVersion: 0,6.0.6000
Name: Outputs; Description: {cm:Outputs}; Types: Anaglyph full
Name: Outputs\CMOShutterOutput; Description: {cm:CMOShutterOutput}; Types: full
Name: Outputs\AnaglyphOutput; Description: {cm:AnaglyphOutput}; Types: full Anaglyph
#elif setupType == "Lenovo"
Name: Drivers; Description: {cm:Drivers}; Types: full
Name: Drivers\D3D89Driver; Description: {cm:D3D89Driver}; Types: full; Flags: fixed 
Name: Drivers\D3D10Driver; Description: {cm:D3D10Driver}; Types: full; MinVersion: 0,6.0.6000
Name: Outputs; Description: {cm:Outputs}; Types: full
Name: Outputs\LenovoOutput; Description: {cm:LenovoOutput}; Types: full
#elif setupType == "YGT"
Name: Drivers; Description: {cm:Drivers}; Types: full
Name: Drivers\D3D89Driver; Description: {cm:D3D89Driver}; Types: full; Flags: fixed 
Name: Drivers\D3D10Driver; Description: {cm:D3D10Driver}; Types: full; MinVersion: 0,6.0.6000
Name: Outputs; Description: {cm:Outputs}; Types: full
Name: Outputs\AvitridOutput; Description: {cm:AvitridOutput}; Types: full; Flags: fixed
Name: Outputs\AnaglyphOutput; Description: {cm:AnaglyphOutput}; Types: full
#elif setupType == "Taerim"
Name: Drivers; Description: {cm:Drivers}; Types: full
Name: Drivers\D3D89Driver; Description: {cm:D3D89Driver}; Types: full; Flags: fixed 
Name: Drivers\D3D10Driver; Description: {cm:D3D10Driver}; Types: full; MinVersion: 0,6.0.6000
Name: Outputs; Description: {cm:Outputs}; Types: full
Name: Outputs\TaerimOutput; Description: {cm:TaerimOutput}; Types: full; Flags: fixed
Name: Outputs\AnaglyphOutput; Description: {cm:AnaglyphOutput}; Types: full
#else
Name: Drivers; Description: {cm:Drivers}; Types: full
Name: Drivers\D3D89Driver; Description: {cm:D3D89Driver}; Types: full; Flags: fixed 
Name: Drivers\D3D10Driver; Description: {cm:D3D10Driver}; Types: full; MinVersion: 0,6.0.6000
Name: Outputs; Description: {cm:Outputs}; Types: full
Name: Outputs\AnaglyphOutput; Description: {cm:AnaglyphOutput}; Types: full
#endif

[Files]
//Source: {#OUTDIR}\Control center.exe; DestDir: {app}; Flags: ignoreversion
Source: {#OUTDIR}\ControlCenter.exe; DestDir: {app}; Flags: ignoreversion
Source: ..\ControlCenter\Krypton\ComponentFactory.Krypton.Toolkit.dll; DestDir: {app}; Flags: ignoreversion
Source: ..\ControlCenter\Krypton\ComponentFactory.Krypton.Navigator.dll; DestDir: {app}; Flags: ignoreversion
Source: ..\ControlCenter\Krypton\ComponentFactory.Krypton.Navigator.KryptonNavigator.lic; DestDir: {app}; Flags: ignoreversion
Source: ..\bin\resources.zip; DestDir: {commonappdata}\{#PRODUCT_NAME}\; Permissions: everyone-full
Source: {#SETUPFILESDIR}\ChangeLog.rtf; DestDir: {app}

Source: Content\language\English.xml; DestDir: {commonappdata}\{#PRODUCT_NAME}\Language; Permissions: everyone-full
Source: Content\language\ChineseSimp.xml; DestDir: {commonappdata}\{#PRODUCT_NAME}\Language; Permissions: everyone-full
Source: Content\language\ChineseTrad.xml; DestDir: {commonappdata}\{#PRODUCT_NAME}\Language; Permissions: everyone-full
#if setupType != "CMO"
Source: Content\language\Russian.xml; DestDir: {commonappdata}\{#PRODUCT_NAME}\Language; Permissions: everyone-full
Source: Content\language\Ukrainian.xml; DestDir: {commonappdata}\{#PRODUCT_NAME}\Language; Permissions: everyone-full
Source: Content\language\German.xml; DestDir: {commonappdata}\{#PRODUCT_NAME}\Language; Permissions: everyone-full
Source: Content\language\Italian.xml; DestDir: {commonappdata}\{#PRODUCT_NAME}\Language; Permissions: everyone-full
Source: Content\language\French.xml; DestDir: {commonappdata}\{#PRODUCT_NAME}\Language; Permissions: everyone-full
Source: Content\language\Spanish.xml; DestDir: {commonappdata}\{#PRODUCT_NAME}\Language; Permissions: everyone-full
Source: Content\language\Japanese.xml; DestDir: {commonappdata}\{#PRODUCT_NAME}\Language; Permissions: everyone-full
#endif

#ifndef DEBUG
Source: Content\Config.xml; DestDir: {commonappdata}\{#PRODUCT_NAME}; Permissions: everyone-full
#else
Source: {#SETUPFILESDIR}\DebugConfig.xml; DestName: Config.xml; DestDir: {commonappdata}\{#PRODUCT_NAME}; Permissions: everyone-full
#endif
#if setupType == "CMO"
Source: Content\data\CMI\Description*.chm; DestDir: {commonappdata}\{#PRODUCT_NAME}
#else
Source: Content\data\iZ3D\Description*.chm; DestDir: {commonappdata}\{#PRODUCT_NAME}
#endif
Source: Content\data\Troubleshooting*.*; DestDir: {commonappdata}\{#PRODUCT_NAME}
Source: Content\BaseProfile.xml; DestDir: {commonappdata}\{#PRODUCT_NAME}; Components: Drivers; Permissions: everyone-full
Source: Content\Statistic.xml; DestDir: {commonappdata}\{#PRODUCT_NAME}; Flags: onlyifdoesntexist uninsneveruninstall setntfscompression; Components: Drivers; Permissions: everyone-full
Source: Content\DefaultUserProfile.xml; DestDir: {userappdata}\{#PRODUCT_NAME}; DestName: UserProfile.xml; Flags: onlyifdoesntexist uninsneveruninstall setntfscompression; Components: Drivers; Permissions: everyone-full
//Source: Content\bin\Profile Converter.exe; Flags: dontcopy ignoreversion
#if setupType != "Lenovo"
Source: Content\bin\dxwebsetup.exe; Flags: dontcopy
#else
Source: Content\DirectX\*; Flags: deleteafterinstall nocompression; DestDir: {tmp}\DirectX
#endif
Source: Content\bin\XmlPreprocess.exe; DestDir: {tmp}; Flags: deleteafterinstall
#if setupType == "iZ3D"
Source: Content\inf\{#MONITOR_INF_FILE}; DestDir: {tmp}; Flags: deleteafterinstall
Source: Content\inf\{#SHORT_COMPANY_NAME}.cat; DestDir: {tmp}; Flags: deleteafterinstall
Source: Content\devcon\i386\devcon.exe; DestDir: {tmp}; Flags: deleteafterinstall; Check: not IsWin64()
Source: Content\devcon\amd64\devcon.exe; DestDir: {tmp}; Flags: deleteafterinstall; Check: IsWin64()
Source: Content\iZ3DStereoWallpaperCreator\*; DestDir: {app}\iZ3DStereoWallpaperCreator; Components: S3DStereoWallpaperCreator; Flags: ignoreversion
#endif

Source: {#FINAL_RELEASE}\Win32\S3DInjectionDriver.sys; DestDir: {app}\Win32; Flags: ignoreversion
Source: {#FINAL_RELEASE}\Win64\S3DInjectionDriver.sys; DestDir: {app}\Win64; Check: IsWin64(); Flags: ignoreversion

Source: {#OUTDIR}\Win32\DiagTool.dll; DestDir: {app}\Win32; Flags: ignoreversion
//Source: {#OUTDIR}\Win64\DiagTool.dll; DestDir: {app}\Win64; Check: IsWin64(); Flags: ignoreversion
Source: {#OUTDIR}\Win32\S3DUtils.dll; DestDir: {app}\Win32; Flags: ignoreversion
Source: {#OUTDIR}\Win64\S3DUtils.dll; DestDir: {app}\Win64; Check: IsWin64(); Flags: ignoreversion

Source: {#FILESDIR}\Win32\S3DCService.exe; DestDir: {app}\Win32; Flags: ignoreversion
Source: {#FILESDIR}\Win64\S3DCService.exe; DestDir: {app}\Win64; Check: IsWin64(); Flags: ignoreversion
Source: {#FILESDIR}\Win32\S3DAPI.dll; DestDir: {app}\Win32; Components: Drivers; Flags: ignoreversion
Source: {#FILESDIR}\Win64\S3DAPI.dll; DestDir: {app}\Win64; Components: Drivers; Check: IsWin64(); Flags: ignoreversion
#ifdef DEBUG
Source: {#FILESDIR}\Win32\ZLOg.dll; DestDir: {app}\Win32; Flags: ignoreversion
Source: {#FILESDIR}\Win64\ZLOg.dll; DestDir: {app}\Win64; Check: IsWin64(); Flags: ignoreversion
Source: ..\lib\DbgHelp\dll\x86\dbghelp.dll; DestDir: {app}\Win32; Components: Drivers; Flags: ignoreversion
Source: ..\lib\DbgHelp\dll\x64\dbghelp.dll; DestDir: {app}\Win64; Components: Drivers; Check: IsWin64(); Flags: ignoreversion
#endif
Source: {#FILESDIR}\Win32\dtest.exe; DestDir: {app}\Win32; Flags: ignoreversion
Source: {#FILESDIR}\Win64\dtest.exe; DestDir: {app}\Win64; Check: IsWin64(); Flags: ignoreversion
Source: ..\DTest\data\*.*; DestDir: {commonappdata}\{#PRODUCT_NAME}\DTest\data\; Flags: recursesubdirs
Source: {#FILESDIR}\Win32\S3DLocker.exe; DestDir: {app}\Win32; Flags: ignoreversion
Source: {#FILESDIR}\Win64\S3DLocker.exe; DestDir: {app}\Win64; Check: IsWin64(); Flags: ignoreversion
Source: {#FILESDIR}\Win32\S3DMouseLock32.dll; DestDir: {app}\Win32; Flags: ignoreversion
Source: {#FILESDIR}\Win64\S3DMouseLock64.dll; DestDir: {app}\Win64; Check: IsWin64(); Flags: ignoreversion
Source: {#FILESDIR}\Win32\S3DInjector.dll; DestDir: {app}\Win32; Components: Drivers; Flags: ignoreversion
Source: {#FILESDIR}\Win64\S3DInjector.dll; DestDir: {app}\Win64; Components: Drivers; Check: IsWin64(); Flags: ignoreversion
Source: {#FILESDIR}\Win32\S3DWrapperD3D8.dll; DestDir: {app}\Win32; Components: Drivers\D3D89Driver; Flags: ignoreversion
Source: {#FILESDIR}\Win32\S3DWrapperD3D9.dll; DestDir: {app}\Win32; Components: Drivers\D3D89Driver; Flags: ignoreversion
Source: {#FILESDIR}\Win64\S3DWrapperD3D9.dll; DestDir: {app}\Win64; Components: Drivers\D3D89Driver; Check: IsWin64(); Flags: ignoreversion
Source: {#FILESDIR}\Win32\S3DWrapperD3D10.dll; DestDir: {app}\Win32; Components: Drivers\D3D10Driver; Flags: ignoreversion
Source: {#FILESDIR}\Win64\S3DWrapperD3D10.dll; DestDir: {app}\Win64; Components: Drivers\D3D10Driver; Check: IsWin64(); Flags: ignoreversion
Source: ..\lib\d3d9\dll\x32\D3D9VistaNoSP1.dll; DestDir: {app}\Win32; Components: Drivers\D3D89Driver; Flags: ignoreversion
Source: ..\lib\d3d9\dll\x64\D3D9VistaNoSP1.dll; DestDir: {app}\Win64; Components: Drivers\D3D89Driver; Check: IsWin64(); Flags: ignoreversion
Source: Content\bin\bmfont.exe; DestDir: {app}\Win32; Components: Drivers; Flags: ignoreversion

Source: Content\bin\PCGW32.DLL; DestDir: {sys}; Components: Outputs

//KMPageFlipper
#ifdef InstallKMDriver  
Source: KMPageFlipper\installdriver.exe; DestDir: {app}\KMPageFlipper; Flags: dontcopy
Source: KMPageFlipper\vista_x32\iZ3DShutterService.sys; DestDir: {sys}\drivers; Check: IsWinVista() and not IsWin64();
Source: KMPageFlipper\vista_x64\iZ3DShutterService.sys; DestDir: {sys}\drivers; Check: IsWinVista() and IsWin64();
Source: KMPageFlipper\win7_x32\iZ3DShutterService.sys;  DestDir: {sys}\drivers; Check: IsWin7()     and not IsWin64();
Source: KMPageFlipper\win7_x64\iZ3DShutterService.sys;  DestDir: {sys}\drivers; Check: IsWin7()     and IsWin64();
#endif

#if setupType == "Lenovo"
Source: {#FILESDIR}\Win32\OutputMethods\LenovoOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\LenovoOutput; Flags: ignoreversion
Source: {#FILESDIR}\Win64\OutputMethods\LenovoOutput.dll; DestDir: {app}\Win64\OutputMethods; Components: Outputs\LenovoOutput; Check: IsWin64(); Flags: ignoreversion
#else
Source: {#FILESDIR}\Win32\OutputMethods\AnaglyphOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\AnaglyphOutput; Flags: ignoreversion
Source: {#FILESDIR}\Win64\OutputMethods\AnaglyphOutput.dll; DestDir: {app}\Win64\OutputMethods; Components: Outputs\AnaglyphOutput; Check: IsWin64(); Flags: ignoreversion
#endif
#if setupType == "CMO"
Source: {#FILESDIR}\Win32\OutputMethods\CMOShutterOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\CMOShutterOutput; Flags: ignoreversion
Source: {#FILESDIR}\Win64\OutputMethods\CMOShutterOutput.dll; DestDir: {app}\Win64\OutputMethods; Components: Outputs\CMOShutterOutput; Check: IsWin64(); Flags: ignoreversion
#elif setupType == "YGT"
Source: {#FILESDIR}\Win32\OutputMethods\AvitridOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\AvitridOutput; Flags: ignoreversion
Source: {#FILESDIR}\Win64\OutputMethods\AvitridOutput.dll; DestDir: {app}\Win64\OutputMethods; Components: Outputs\AvitridOutput; Check: IsWin64(); Flags: ignoreversion
#elif setupType == "Taerim"
Source: {#FILESDIR}\Win32\OutputMethods\TaerimOutput_protected.dll; DestName: TaerimOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\TaerimOutput; Flags: ignoreversion
//Source: {#FILESDIR}\Win64\OutputMethods\TaerimOutput.dll; DestDir: {app}\Win64\OutputMethods; Components: Outputs\TaerimOutput; Check: IsWin64(); Flags: ignoreversion
#elif setupType != "Anaglyph"
#if setupType != "Lenovo"
Source: {#FILESDIR}\Win32\OutputMethods\S3DOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\S3DOutput; Flags: ignoreversion
Source: {#FILESDIR}\Win64\OutputMethods\S3DOutput.dll; DestDir: {app}\Win64\OutputMethods; Components: Outputs\S3DOutput; Check: IsWin64(); Flags: ignoreversion
Source: {#FINAL_RELEASE}\Win32\OutputMethods\ATIDLP3DOutput_protected.dll; DestName: ATIDLP3DOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\ATIDLP3DOutput; Flags: ignoreversion
Source: {#FILESDIR}\Win64\OutputMethods\ATIDLP3DOutput.dll; DestDir: {app}\Win64\OutputMethods; Components: Outputs\ATIDLP3DOutput; Check: IsWin64(); Flags: ignoreversion
#endif
#ifdef DEBUG
Source: {#FILESDIR}\Win32\OutputMethods\CMOShutterOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\CMOShutterOutput; Flags: ignoreversion
Source: {#FILESDIR}\Win64\OutputMethods\CMOShutterOutput.dll; DestDir: {app}\Win64\OutputMethods; Components: Outputs\CMOShutterOutput; Check: IsWin64(); Flags: ignoreversion
Source: {#FILESDIR}\Win32\OutputMethods\AvitridOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\AvitridOutput; Flags: ignoreversion
Source: {#FILESDIR}\Win64\OutputMethods\AvitridOutput.dll; DestDir: {app}\Win64\OutputMethods; Components: Outputs\AvitridOutput; Check: IsWin64(); Flags: ignoreversion
Source: {#FILESDIR}\Win32\OutputMethods\TaerimOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\TaerimOutput; Flags: ignoreversion
Source: {#FILESDIR}\Win64\OutputMethods\TaerimOutput.dll; DestDir: {app}\Win64\OutputMethods; Components: Outputs\TaerimOutput; Check: IsWin64(); Flags: ignoreversion
#endif
#if setupType != "Lenovo" && setupType != "YGT" && setupType != "Taerim"
//Source: {#FINAL_RELEASE}\Win32\OutputMethods\Shutter120HzOutput_protected.dll; DestName: Shutter120HzOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\Shutter120HzOutput; Flags: ignoreversion
Source: {#FINAL_RELEASE}\Win32\OutputMethods\iZShutterOutput_protected.dll; DestName: iZShutterOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\iZShutterOutput; Flags: ignoreversion
Source: {#FINAL_RELEASE}\Win32\OutputMethods\S3D120HzProjectorsOutput_protected.dll; DestName: S3D120HzProjectorsOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\S3D120HzProjectorsOutput; Flags: ignoreversion
Source: {#FINAL_RELEASE}\Win32\OutputMethods\AMDHD3DOutput_protected.dll; DestName: AMDHD3DOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\AMDHD3DOutput; Flags: ignoreversion
//Source: {#FILESDIR}\Win32\OutputMethods\ColorCodeOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\ColorCodeOutput; Flags: ignoreversion
//Source: {#FILESDIR}\Win64\OutputMethods\ColorCodeOutput.dll; DestDir: {app}\Win64\OutputMethods; Components: Outputs\ColorCodeOutput; Check: IsWin64(); Flags: ignoreversion
Source: {#FINAL_RELEASE}\Win32\OutputMethods\ZalmanOutput_protected.dll; DestName: ZalmanOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\ZalmanOutput; Flags: ignoreversion
//Source: {#FILESDIR}\Win64\OutputMethods\ZalmanOutput.dll; DestDir: {app}\Win64\OutputMethods; Components: Outputs\ZalmanOutput; Check: IsWin64(); Flags: ignoreversion
Source: {#FINAL_RELEASE}\Win32\OutputMethods\DualProjectionOutput_protected.dll; DestName: DualProjectionOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\DualProjOutput; Flags: ignoreversion skipifsourcedoesntexist
//Source: {#FINAL_RELEASE}\Win32\OutputMethods\S3DShutterOutput_protected.dll; DestName: S3DShutterOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\S3DShutterOutput; Flags: ignoreversion
Source: {#FINAL_RELEASE}\Win32\OutputMethods\DLP3DOutput_protected.dll; DestName: DLP3DOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\DLP3DOutput; Flags: ignoreversion
Source: {#FINAL_RELEASE}\Win32\OutputMethods\InterlacedOutput_protected.dll; DestName: InterlacedOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\InterlacedOutput; Flags: ignoreversion
//Source: {#FINAL_RELEASE}\Win32\OutputMethods\ShutterOutput_protected.dll; DestName: ShutterOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\ShutterOutput; Flags: ignoreversion
Source: {#FINAL_RELEASE}\Win32\OutputMethods\SideBySideOutput_protected.dll; DestName: SideBySideOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\SideBySideOutput; Flags: ignoreversion
Source: {#FINAL_RELEASE}\Win32\OutputMethods\StereoMirrorOutput_protected.dll; DestName: StereoMirrorOutput.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\StereoMirrorOutput; Flags: ignoreversion
Source: {#FINAL_RELEASE}\Win32\OutputMethods\VR920Output_protected.dll; DestName: VR920Output.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\VR920Output; Flags: ignoreversion
//Source: {#FINAL_RELEASE}\Win32\OutputMethods\Z800Output_protected.dll; DestName: Z800Output.dll; DestDir: {app}\Win32\OutputMethods; Components: Outputs\Z800Output; Flags: ignoreversion
//Source: ..\OutputMethods\S3DShutter\MarkingSpec.xml; DestDir: {commonappdata}\{#PRODUCT_NAME}; Components: Outputs\S3DShutterOutput; Permissions: everyone-full
//Source: {#FILESDIR}\Win32\S3DOGLInjector.dll; DestDir: {app}\Win32; Components: Drivers\OGLQBDriver; Flags: ignoreversion
// should be on for 32-bit wrapper: 
//Source: {#FILESDIR}\Win64\S3DOGLInjector.dll; DestDir: {app}\Win64; Components: Drivers\OGLQBDriver; Check: IsWin64(); Flags: ignoreversion
//Source: {#FINAL_RELEASE}\Win32\S3DWrapperOGL_protected.dll; DestName: S3DWrapperOGL.dll; DestDir: {app}\Win32; Components: Drivers\OGLQBDriver; Flags: ignoreversion skipifsourcedoesntexist
//Source: {#FINAL_RELEASE}\Win64\S3DWrapperOGL_protected.dll; DestName: S3DWrapperOGL.dll; DestDir: {app}\Win64; Components: Drivers\OGLQBDriver; Check: IsWin64(); Flags: ignoreversion skipifsourcedoesntexist
#endif
#endif

[Dirs]
Name: {code:MyPictures}\{#PRODUCT_NAME} Screenshots; Flags: uninsneveruninstall; Components: Drivers
#ifdef DEBUG
Name: {app}\Win32\Logs; Permissions: everyone-full; Flags: uninsalwaysuninstall
Name: {app}\Win64\Logs; Permissions: everyone-full; Flags: uninsalwaysuninstall; Check: IsWin64()
#endif

[Icons]
//Name: {group}\{#PRODUCT_NAME}; Filename: {app}\Control Center.exe; WorkingDir: {app}
Name: {group}\{#PRODUCT_NAME}; Filename: {app}\ControlCenter.exe; WorkingDir: {app}
Name: {group}\{#PRODUCT_NAME} Screenshots; Filename: {win}\explorer.exe; Parameters: {code:MyPictures}\{#PRODUCT_NAME} Screenshots; IconFilename: {sys}\shell32.dll; IconIndex: 3; Flags: excludefromshowinnewinstall
#if setupType == "iZ3D"
Name: {group}\{#PRODUCT_NAME} Stereo Wallpaper Creator; Filename: {app}\iZ3DStereoWallpaperCreator\iZ3DStereoWallpaperCreator.exe; WorkingDir: {app}\iZ3DStereoWallpaperCreator; Components: S3DStereoWallpaperCreator
#endif
Name: {group}\{#PRODUCT_NAME} User Guide; Filename: {commonappdata}\{#PRODUCT_NAME}\Description.chm; Flags: excludefromshowinnewinstall
Name: {group}\Uninstall; Filename: {uninstallexe}; Flags: excludefromshowinnewinstall

[Run]
Filename: {app}\Win64\S3DCService.exe; Parameters: /install; Check: IsWin64(); Flags: runhidden
Filename: {app}\Win32\S3DCService.exe; Parameters: /install; Flags: runhidden

[UninstallRun]
Filename: {app}\Win32\S3DCService.exe; Parameters: /stopAndUninstall; Flags: runhidden
Filename: {app}\Win64\S3DCService.exe; Parameters: /stopAndUninstall; Check: IsWin64(); Flags: runhidden

[UninstallDelete]
Name: {app}\cc.log; Type: files
Name: {commonappdata}\{#PRODUCT_NAME}\Font; Type: FilesAndOrDirs
#ifdef DEBUG
Name: {app}\Win32\Logs; Type: FilesAndOrDirs
Name: {app}\Win64\Logs; Type: FilesAndOrDirs; Check: IsWin64()
#endif

[Code]
const
  CSIDL_MYPICTURES = $0027; { My Pictures, new for Win2K }
  RegistryKeyName = 'SOFTWARE\iZ3D\iZ3D Driver\';
var
  CCWnd: HWND;
  Counter: Integer;
  hProcess: Integer;
  ProcessId: Integer;
  ResultCode: Integer;
  FileName: String;
  NeedRestartBeforeInstall: Boolean;
  NeedRestartAfterInstall: Boolean;
  KMFlipper_Version_old : String;

  CheckListBox1, CheckListBox2, CheckListBox3: TNewCheckListBox;
  RunCCIndex, EnableCCMouseLock, EnableDriverIndex, DisableDriverIndex: Integer;
  MonitorSetupParameter: String;
  GammaSetupParameter: Integer;

  procedure SetGamma(ShowWarning: Integer);
	external 'SetGamma@files:S3DUtils.dll stdcall delayload setuponly';

  function  GetWindowThreadProcessId(hWnd: HWND; var lpProcessId : Integer) : Integer;
	external 'GetWindowThreadProcessId@user32.dll stdcall uninstallonly';
  function  TerminateProcess(hProcess: Integer; uExitCode: Integer) : Integer;
	external 'TerminateProcess@kernel32.dll stdcall uninstallonly';
  function  OpenProcess(dwDesiredAccess : Integer; bInheritHandle : Integer; dwProcessId : Integer) : Integer;
	external 'OpenProcess@kernel32.dll stdcall uninstallonly';
  function  CloseHandle(hObject : Integer) : Integer;
	external 'CloseHandle@kernel32.dll stdcall uninstallonly';
  function  WaitForSingleObject(hHandle : Integer; Milliseconds : Integer) : Integer;
	external 'WaitForSingleObject@kernel32.dll stdcall uninstallonly';

function MyPictures(Param: String): String;
var
  Path: String;
begin
  Path := GetShellFolderByCSIDL(CSIDL_MYPICTURES, true);
  if Path <> '' then
	Result := Path
  else
	Result := ExpandConstant('{userdocs}\My Pictures\');
end;

function ExpandConstant2(Param: String): String;
begin
  Result := ExpandConstant(ExpandConstant(Param));
end;

function cm2(Param: String): String;
begin
  Result := ExpandConstant(CustomMessage(Param));
end;

procedure CloseApp(AppName: String; WM_Message: DWORD);
begin
   // try to close normally
   Counter := 0;
   CCWnd := FindWindowByWindowName(AppName);
   while(CCWnd <> 0) do
   begin
		if(Counter >= 100) then
			break;
		Counter := Counter + 1;
		PostMessage(CCWnd, WM_Message, 0, 0);
		Sleep(0);
		CCWnd := FindWindowByWindowName(AppName);
   end;

   // force to close extremely
   if(CCWnd <> 0) then
   begin
		GetWindowThreadProcessId(CCWnd, ProcessId);
		if(ProcessId <> 0) then
		begin
			hProcess := OpenProcess($0401, 0, ProcessId);
			if(hProcess <> 0) then
			begin
				TerminateProcess(hProcess, -1);
				WaitForSingleObject(hProcess, 1000);
				CloseHandle(hProcess);
			end
		end
   end;
end;

{
Operating system			Version number 
Windows 7					6.1 
Windows Server 2008 R2		6.1 
Windows Server 2008			6.0 
Windows Vista				6.0 
Windows Server 2003 R2		5.2 
Windows Server 2003			5.2 
Windows XP 64-Bit Edition	5.2 
Windows XP					5.1 
Windows 2000				5.0 
}

function IsWinVista(): Boolean;
var
	Version: TWindowsVersion;
begin
	Result := False;
	GetWindowsVersionEx(Version);
	if Version.NTPlatform and (Version.Major = 6) and (Version.Minor = 0)
	then Result := True;
end;

function IsWin7(): Boolean;
var
	Version: TWindowsVersion;
begin
	Result := False;
	GetWindowsVersionEx(Version);
	if Version.NTPlatform and (Version.Major = 6) and (Version.Minor = 1)
	then Result := True;
end;

function IsWinVistaOrHigher(): Boolean;
var
	Version: TWindowsVersion;
begin
	Result := False;
	GetWindowsVersionEx(Version);
	if Version.NTPlatform and (Version.Major >= 6)
	then Result := True;
end;

//KMPageFlipper
#ifdef InstallKMDriver  
function NeedRestart(): Boolean;
begin
  if(NeedRestartAfterInstall and FileExists(ExpandConstant('{sys}\drivers\iZ3DShutterService.sys'))) then
    if(GetMD5OfFile(ExpandConstant('{sys}\drivers\iZ3DShutterService.sys')) = KMFlipper_Version_old) then
      NeedRestartAfterInstall := False; 
  Result := NeedRestartAfterInstall;
end;

function UninstallNeedRestart(): Boolean;
begin
  Result := True;
end;
#endif

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
	ResultCode: Integer;
	OldState: Boolean;
	i: Integer;
    isUpdate: Boolean;
begin
  case CurUninstallStep of
	usUninstall: begin
		ResultCode := 0;
		Exec(ExpandConstant('{app}\Win32\S3DCService.exe'), '/ShowUninstallWindow', '', SW_SHOW, ewWaitUntilTerminated, ResultCode);
		if (ResultCode <> 0) then
			Abort;
		if (IsWin64()) then
		begin
			Exec(ExpandConstant('{app}\Win64\S3DCService.exe'), '/ShowUninstallWindow', '', SW_SHOW, ewWaitUntilTerminated, ResultCode);
			if (ResultCode <> 0) then
				Abort;
		end;
		CloseApp('{#SHORT_COMPANY_NAME} Dynamic Test', 18);
#if setupType != "Anaglyph"
		CloseApp('{#CONTROL_CENTER}', 16);
#else
		CloseApp('{#CONTROL_CENTER} - c''t edition', 16);
#endif
		CloseApp('iZ3D Stereo Wallpaper Creator', 16);
		CloseApp('S3DLocker32Window', 18);
		if (IsWin64()) then
			CloseApp('S3DLocker64Window', 18);

		DelTree(ExpandConstant('{commonappdata}\{#PRODUCT_NAME}\DX9ShaderDataCache'), True, True, True);
		DelTree(ExpandConstant('{commonappdata}\{#PRODUCT_NAME}\MiniDumps'), True, True, True);

		if(SuppressibleMsgBox(CustomMessage('RemoveDriverSettings'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2, idNo) = idYes) then
		begin
			RegDeleteKeyIncludingSubkeys(HKLM, RegistryKeyName);
			RegDeleteKeyIncludingSubkeys(HKCU, RegistryKeyName);
			DeleteFile(ExpandConstant('{commonappdata}\{#PRODUCT_NAME}\Statistic.xml'));
			DeleteFile(ExpandConstant('{commonappdata}\{#PRODUCT_NAME}\Update.xml'));
			DeleteFile(ExpandConstant('{commonappdata}\{#PRODUCT_NAME}\Report.txt'));
			if(SuppressibleMsgBox(CustomMessage('RemoveGameProfile'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2, idNo) = idYes) then
				DeleteFile(ExpandConstant('{userappdata}\{#PRODUCT_NAME}\UserProfile.xml'));
		end
		else begin
			RegDeleteValue(HKLM, RegistryKeyName, 'UninstallExeName');
		end;

//KMPageFlipper
#ifdef InstallKMDriver  
        isUpdate := false;
        for i := 1 to ParamCount() do begin 
            if(Uppercase(ParamStr(i)) = '/UPDATE') then begin
              isUpdate := true;
              break;
            end;
        end;
        if( not isUpdate ) then
            RegDeleteKeyIncludingSubkeys(HKLM, 'SYSTEM\ControlSet001\Services\iZ3DShutterService\');
        	
        //--- KM shutter don't support XP ---
        if (IsWin64() and IsWinVistaOrHigher()) then begin 
          OldState := EnableFsRedirection(False);
          if(FileExists(ExpandConstant('{sys}\drivers\iZ3DShutterService.sys'))) then 
            DeleteFile(ExpandConstant('{sys}\drivers\iZ3DShutterService.sys'));
          EnableFsRedirection(OldState);
        end;
#endif
	    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;  
  FileDir : String;
  ERROR_SERVICE_MARKED_FOR_DELETE : Integer;
  OldState: Boolean;
begin	
	//KMPageFlipper
	NeedRestartAfterInstall := False;
#ifdef InstallKMDriver
	if(IsWinVistaOrHigher()) then 
		NeedRestartAfterInstall := True;

	//--- KM shutter don't support XP ---
	if (IsWin64() and IsWinVistaOrHigher()) then  
	  OldState := EnableFsRedirection(False);
	if(FileExists(ExpandConstant('{sys}\drivers\iZ3DShutterService.sys'))) then
		KMFlipper_Version_old := GetMD5OfFile(ExpandConstant('{sys}\drivers\iZ3DShutterService.sys'));
	if (IsWin64() and IsWinVistaOrHigher()) then 
	  EnableFsRedirection(OldState);
#endif

	if(RegQueryStringValue(HKLM, RegistryKeyName, 'UninstallExeName', FileName)) then begin
		FileDir := ExtractFileDir(FileName);
		FileCopy(FileDir + '\Profiles.txt', ExpandConstant('{tmp}\Profiles.txt'), false);
		if(FileExists(FileName)) then
		begin
#ifdef InstallKMDriver  
			Exec(FileName, '/SILENT /SUPPRESSMSGBOXES /NORESTART /UPDATE', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
#else    
			Exec(FileName, '/SILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
#endif    
			if(ResultCode <> 0) then
			  if (SuppressibleMsgBox(CustomMessage('ContinueInstallation'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2, idNo) = idNo) then
				Abort;
		end;
	end;

	NeedRestartBeforeInstall := False;
	ERROR_SERVICE_MARKED_FOR_DELETE := 1072;
	Exec('sc.exe', 'delete S3DSvc32', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
	if ResultCode = ERROR_SERVICE_MARKED_FOR_DELETE then
		NeedRestartBeforeInstall := True
	else if IsWin64() then begin
		Exec('sc.exe', 'delete S3DSvc64', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
		if ResultCode = ERROR_SERVICE_MARKED_FOR_DELETE then
			NeedRestartBeforeInstall := True;
	end;
	NeedsRestart := NeedRestartBeforeInstall;
	if NeedsRestart then
	begin
		NeedRestartAfterInstall := False;
		Result := CustomMessage('RestartRequiredToContinue');
	end;
end;

function Preinstall(): Boolean;
var
  ErrorCode: Integer;
  NetFrameWorkInstalled : Boolean;
begin
  Result := True;

	NetFrameWorkInstalled := RegKeyExists(HKLM,'SOFTWARE\Microsoft\.NETFramework\policy\v2.0');
	if not NetFrameWorkInstalled then
	begin
		Result := false;
		if (idYes = MsgBox(CustomMessage('DotNetFrameworkSetup'), mbConfirmation, MB_YESNO)) then
		begin
			if (not IsWin64()) then
				ShellExec('open',
					'http://www.microsoft.com/downloads/en/details.aspx?FamilyID=0856eacb-4362-4b0d-8edd-aab15c5e04f5',
					'','',SW_SHOWNORMAL,ewNoWait,ErrorCode)
			else
				ShellExec('open',
					'http://www.microsoft.com/downloads/en/details.aspx?FamilyID=B44A0000-ACF8-4FA1-AFFB-40E78D788B00',
					'','',SW_SHOWNORMAL,ewNoWait,ErrorCode);
		end;
		Exit;
	end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpReady then begin
    WizardForm.ReadyMemo.WordWrap := True;
    WizardForm.ReadyMemo.ScrollBars := ssNone;
    WizardForm.ReadyMemo.Lines.Text := CustomMessage('AntiVirusClose');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  XMLPreprocessRunningParameter: String;
  Output: String;
 	OldState: Boolean;
begin
  case CurStep of
	ssInstall: begin
		if not Preinstall then
			Abort;
	end;
	ssPostInstall: begin							
#if setupType == "YGT"
		SuppressibleMsgBox(CustomMessage('AvitridMonitorNotFounded'), mbInformation, MB_OK, idOk);
#endif

		//ExtractTemporaryFile('Profile Converter.exe');
		//ExecAsOriginalUser(ExpandConstant('{tmp}\Profile Converter.exe'), '', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

//KMPageFlipper
#ifdef InstallKMDriver
		if(IsWinVistaOrHigher()) then
        begin
		  if (IsWin64()) then begin
			OldState := EnableFsRedirection(False);
			if not FileExists(ExpandConstant('{sys}\drivers\iZ3DShutterService.sys')) and
			    FileExists(ExpandConstant('{syswow64}\drivers\iZ3DShutterService.sys')) then begin
			  FileCopy(ExpandConstant('{syswow64}\drivers\iZ3DShutterService.sys'), ExpandConstant('{sys}\drivers\iZ3DShutterService.sys'), true);
			  DeleteFile(ExpandConstant('{syswow64}\drivers\iZ3DShutterService.sys'));
			end;
			EnableFsRedirection(OldState);
		  end; 
		  ExtractTemporaryFile('installdriver.exe');
		  Exec(ExpandConstant('{tmp}\installdriver.exe'), '-install', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
		end;
#endif

		if (not FileExists(ExpandConstant('{sys}\D3DX9_43.dll')) or
			not FileExists(ExpandConstant('{sys}\d3dcompiler_43.dll'))) then
		begin
			WizardForm.Hide();
			if(MsgBox(CustomMessage('DXUpdate'), mbConfirmation, MB_YESNO) = idYes) then begin
#if setupType != "Lenovo"
				ExtractTemporaryFile('dxwebsetup.exe');
				Exec(ExpandConstant('{tmp}\dxwebsetup.exe'), '', '', SW_SHOW, ewWaitUntilTerminated, ResultCode);
#else
				Exec(ExpandConstant('{tmp}\dxsetup.exe'), '', '', SW_SHOW, ewWaitUntilTerminated, ResultCode);
#endif
			end;
			WizardForm.Show();
		end;

#if setupType == "iZ3D"
		if IsComponentSelected('Outputs\S3DOutput') then
		begin
			MonitorSetupParameter := ExpandConstant('"{app}\Win32\S3DUtils.dll", SetupS3DMonitor');
			GammaSetupParameter := 1;
			if (WizardSilent) then begin
			  MonitorSetupParameter := MonitorSetupParameter + ' /SILENT';
			  GammaSetupParameter := 0;
			end;
			ExecAsOriginalUser(ExpandConstant('{sys}\rundll32.exe'), MonitorSetupParameter, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
			//SetGamma(GammaSetupParameter);
			Exec(ExpandConstant('{tmp}\devcon.exe'), 'update {#MONITOR_INF_FILE} "Monitor\CMO3228"', ExpandConstant('{tmp}\'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
			Exec(ExpandConstant('{tmp}\devcon.exe'), 'update {#MONITOR_INF_FILE} "Monitor\CMO3229"', ExpandConstant('{tmp}\'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
			Exec(ExpandConstant('{tmp}\devcon.exe'), 'update {#MONITOR_INF_FILE} "Monitor\CMO3239"', ExpandConstant('{tmp}\'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
			Exec(ExpandConstant('{tmp}\devcon.exe'), 'update {#MONITOR_INF_FILE} "Monitor\CMO3238"', ExpandConstant('{tmp}\'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
			Exec(ExpandConstant('{tmp}\devcon.exe'), 'update {#MONITOR_INF_FILE} "Monitor\CMO3237"', ExpandConstant('{tmp}\'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
		end;
#endif

		RegWriteStringValue(HKLM, RegistryKeyName, 'UninstallExeName', ExpandConstant('{uninstallexe}'));

		if not NeedRestartAfterInstall then begin 
			//--- setup initial driver state controls for next Wizard page ---
			CheckListBox1 := TNewCheckListBox.Create(WizardForm.InfoAfterPage);
			CheckListBox1.Left := ScaleX(240);
			CheckListBox1.Top := ScaleY(160);
			CheckListBox1.Width := ScaleX(230);
			CheckListBox1.Height := ScaleY(20);
			CheckListBox1.Flat := True;
			CheckListBox1.Parent := WizardForm;
			CheckListBox1.BorderStyle := bsNone;
			RunCCIndex := CheckListBox1.AddCheckBox(CustomMessage('RunControlCenter'), '', 0, True, True, False, False, nil);
		end;

#if setupType == "iZ3D"
		CheckListBox2 := TNewCheckListBox.Create(WizardForm.InfoAfterPage);
		CheckListBox2.Left := ScaleX(240);
		CheckListBox2.Top := ScaleY(180);
		CheckListBox2.Width := ScaleX(230);
		CheckListBox2.Height := ScaleY(20);
		CheckListBox2.Flat := True;
		CheckListBox2.Parent := WizardForm;
		CheckListBox2.BorderStyle := bsNone;
		EnableCCMouseLock := CheckListBox2.AddCheckBox(CustomMessage('MouseLock'), '', 0, False, True, False, False, nil);
#endif

		CheckListBox3 := TNewCheckListBox.Create(WizardForm.InfoAfterPage);
		CheckListBox3.Left := ScaleX(240);
		CheckListBox3.Top := ScaleY(210);
		CheckListBox3.Width := ScaleX(230);
		CheckListBox3.Height := ScaleY(90);
		CheckListBox3.Flat := True;
		CheckListBox3.Parent := WizardForm;
		CheckListBox3.BorderStyle := bsNone;
		EnableDriverIndex := CheckListBox3.AddRadioButton(CustomMessage('EnableDriver'), '', 0, True, True, nil);
		DisableDriverIndex := CheckListBox3.AddRadioButton(CustomMessage('DisableDriver'), '', 0, False, True, nil);
	end;
	ssDone: begin
		if NeedRestartBeforeInstall then
		  exit;
		XMLPreprocessRunningParameter := ExpandConstant('/c /q /nologo /i "{commonappdata}\{#PRODUCT_NAME}\Config.xml"');

#ifdef DEBUG
		Output := 'AnaglyphOutput'
#else
		if IsComponentSelected('Outputs\CMOShutterOutput') then
			Output := 'CMOShutterOutput'
		else if IsComponentSelected('Outputs\AvitridOutput') then
			Output := 'AvitridOutput'
		else if IsComponentSelected('Outputs\TaerimOutput') then
			Output := 'TaerimOutput'
		else 
			Output := 'AnaglyphOutput';
#endif

#if setupType == "iZ3D"
		if(CheckListBox2.Checked[EnableCCMouseLock]) then
		begin
			XMLPreprocessRunningParameter := XMLPreprocessRunningParameter + ' /d lockCursorValue=1'
			ExecAsOriginalUser(ExpandConstant('{app}\Win32\S3DLocker.exe'), '', '', SW_HIDE, ewNoWait, ResultCode);
			if(IsWin64) then
				ExecAsOriginalUser(ExpandConstant('{app}\Win64\S3DLocker.exe'), '', '', SW_HIDE, ewNoWait, ResultCode);
		end
		else
			XMLPreprocessRunningParameter := XMLPreprocessRunningParameter + ' /d lockCursorValue=0';
#elif setupType == "CMO"
		XMLPreprocessRunningParameter := XMLPreprocessRunningParameter + ' /d lockCursorValue=0';

		if IsComponentSelected('Outputs\CMOShutterOutput') then
			XMLPreprocessRunningParameter := XMLPreprocessRunningParameter + ' /d displayScalingMode=4';
#elif setupType == "Lenovo"
		XMLPreprocessRunningParameter := XMLPreprocessRunningParameter + ' /d lockCursorValue=0';
#elif setupType == "YGT"
		XMLPreprocessRunningParameter := XMLPreprocessRunningParameter + ' /d lockCursorValue=0';
		XMLPreprocessRunningParameter := XMLPreprocessRunningParameter + ' /d displayScalingMode=4';
#elif setupType == "Taerim"
		XMLPreprocessRunningParameter := XMLPreprocessRunningParameter + ' /d lockCursorValue=0';
		XMLPreprocessRunningParameter := XMLPreprocessRunningParameter + ' /d displayScalingMode=4';;
#else
		XMLPreprocessRunningParameter := XMLPreprocessRunningParameter + ' /d lockCursorValue=0';
#endif

		XMLPreprocessRunningParameter := XMLPreprocessRunningParameter + ' /d outputMethodDll=' + Output;

		if(CheckListBox3.Checked[EnableDriverIndex]) then
			XMLPreprocessRunningParameter := XMLPreprocessRunningParameter + ' /d runAtStartupValue=1'
		else
			XMLPreprocessRunningParameter := XMLPreprocessRunningParameter + ' /d runAtStartupValue=0';

		XMLPreprocessRunningParameter := XMLPreprocessRunningParameter + ' /d languageName=' + ActiveLanguage;
		XMLPreprocessRunningParameter := XMLPreprocessRunningParameter + ' /d {#setupType}';

		ExecAsOriginalUser(ExpandConstant('{tmp}\XmlPreprocess.exe'), XMLPreprocessRunningParameter, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

		ExecAsOriginalUser(ExpandConstant('{sys}\rundll32.exe'), ExpandConstant('"{app}\Win32\S3DUtils.dll", GenerateFont'), '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

		ExecAsOriginalUser(ExpandConstant('{sys}\rundll32.exe'), ExpandConstant('"{app}\Win32\S3DUtils.dll", GetAndWriteToRegistryDriverModuleName'), '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
		if IsWin64() then
			ExecAsOriginalUser(ExpandConstant('{sys}\rundll32.exe'), ExpandConstant('"{app}\Win64\S3DUtils.dll", GetAndWriteToRegistryDriverModuleName'), '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

		// because KM driver need restart
		if IsWin64() then
			Exec(ExpandConstant('{app}\Win64\S3DCService.exe'), '/start', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
		Exec(ExpandConstant('{app}\Win32\S3DCService.exe'), '/start', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

		if(not WizardSilent and not NeedRestartAfterInstall and CheckListBox1.Checked[RunCCIndex]) then
//				ExecAsOriginalUser(ExpandConstant('{app}\Control center.exe'), '', '', SW_SHOW, ewNoWait, ResultCode);
			ExecAsOriginalUser(ExpandConstant('{app}\ControlCenter.exe'), '', '', SW_SHOW, ewNoWait, ResultCode);
	end;
  end;
end;
