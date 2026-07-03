#include "..\bin\temp_version.h"

#include "..\Shared\ProductNames.h"

#ifndef setupType
	// Anaglyph, CMO
	#define setupType = "iZ3D"
#endif

#if setupType == "iZ3D"
	#define EDITION ""
#elif setupType == "Anaglyph"
	#define EDITION ".Anaglyph"
#elif setupType == "CMO"
	#define EDITION ".CMO"
#endif

#ifndef OUTDIR
	#define OUTDIR "..\bin\Final Release\"
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
AppVerName={#DISPLAYED_VERSION} Update
AppPublisher={#COMPANY_NAME}
AppPublisherURL={#COMPANY_SITE}
AppVersion={#DISPLAYED_VERSION}
DefaultDirName={pf}\{#PRODUCT_NAME}
DefaultGroupName={#PRODUCT_NAME}
DisableProgramGroupPage=false
PrivilegesRequired=admin
AllowNoIcons=true
OutputDir=..\BuildSetup\iZ3D
OutputBaseFilename={#SETUP_FILE_NAME}{#EDITION}_ShutterOuputUpdate.{#DISPLAYED_VERSION}{#POSTFIX}
UninstallDisplayIcon={app}\Control center.exe
UninstallDisplayName={#PRODUCT_NAME} Remove
MinVersion=0,5.01.2600
VersionInfoVersion={#VERSION}
VersionInfoCompany={#COMPANY_NAME}
VersionInfoDescription={#PRODUCT_NAME}
VersionInfoTextVersion={#VERSION}
VersionInfoCopyright={#COMPANY_NAME}
SetupLogging=false
WizardImageFile=SetupModern.bmp
Compression=lzma2/ultra
SolidCompression=true
InternalCompressLevel=ultra
ShowLanguageDialog=yes
LanguageDetectionMethod=locale
AlwaysShowComponentsList=false
AllowCancelDuringInstall=false
WizardSmallImageFile=SetupModernSmall.bmp
LicenseFile=Licenses\License-EN.rtf
AppSupportURL=http://iz3d.com/support/
AppUpdatesURL=http://www.iz3d.com/t-dcdriver.aspx
#ifndef ANAGLYPH
AppID={{30BEF9F2-CD3F-4B13-9E5C-BFE2F9544572}
#else
AppID={{30BEF9F2-AAAA-4B13-9E5C-BFE2F9544572}
ShowComponentSizes=false
FlatComponentsList=false
UsePreviousSetupType=false
UsePreviousTasks=false
DirExistsWarning=no
AllowUNCPath=false
#endif

UsePreviousAppDir=yes
CreateUninstallRegKey=no
UpdateUninstallLogAppName=no

[CustomMessages]
ShortProductName=iZ3D
DriverVersion=1.99.0097

[Languages]
Name: english; MessagesFile: compiler:Default.isl, Languages\Default.isl
Name: chineseSimp; MessagesFile: compiler:Languages\ChineseSimp.isl; LicenseFile: Licenses\License-SC.rtf
Name: chineseTrad; MessagesFile: compiler:Languages\ChineseTrad.isl; LicenseFile: Licenses\License-TC.rtf
#if setupType != "CMO"
Name: russian; MessagesFile: compiler:Languages\Russian.isl, Languages\Russian.isl
Name: ukrainian; MessagesFile: compiler:Languages\Ukrainian.isl
Name: german; MessagesFile: compiler:Languages\German.isl
Name: italian; MessagesFile: compiler:Languages\Italian.isl
#endif

[Files]
//Source: {#FINAL_RELEASE}\Win32\OutputMethods\Shutter120HzOutput_protected.dll; DestName: Shutter120HzOutput.dll; DestDir: {app}\Win32\OutputMethods; Flags: ignoreversion
Source: {#FINAL_RELEASE}\Win32\OutputMethods\S3D120HzProjectorsOutput_protected.dll; DestName: S3D120HzProjectorsOutput.dll; DestDir: {app}\Win32\OutputMethods; Flags: ignoreversion

[Code]
const
 RegistryKeyName = 'SOFTWARE\iZ3D\iZ3D Driver\';

procedure CurStepChanged(CurStep: TSetupStep);
var
  Version : String;
  CurVersion : String;
begin
  if (CurStep = ssInstall) then
  begin
	CurVersion := CustomMessage('DriverVersion');
	if(not RegQueryStringValue(HKLM, RegistryKeyName, 'CurrentVersion', Version) or (CompareStr(Version, CurVersion) <> 0)) then begin
		SuppressibleMsgBox('Current installed iZ3D Version not supported', mbError, MB_OK, idOk);
		Abort;
	end;
  end;
end;
