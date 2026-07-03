/* IZ3D_FILE: $Id$ 
*
* Project : iZ3D Stereo Driver
* Copyright (C) iZ3D Inc. 2002 - 2010
*
* $Author$
* $Revision$
* $Date$
* $LastChangedBy$
* $URL$
*/
#include "stdafx.h"
#include <stdlib.h>
#include <stdio.h>
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <string>
#include <set>
#include "tinyxml.h"
#include "ProductNames.h"
#include "CommonUtils\System.h"
#include "CommonUtils\StringUtils.h"
#include "CommonUtils\CommonResourceFolders.h"

using namespace std;

static CHAR BMFontConfigProlog[] = ("\
# AngelCode Bitmap Font Generator configuration file	\n\
fileVersion=1											\n\
														\n\
# font settings											\n\
fontName=");

static CHAR BMFontConfig[] = ("							\n\
charSet=0												\n\
fontSize=22												\n\
aa=1													\n\
scaleH=100												\n\
useSmoothing=1											\n\
isBold=0												\n\
isItalic=0												\n\
useUnicode=1											\n\
disableBoxChars=1										\n\
outputInvalidCharGlyph=0								\n\
														\n\
# character alignment									\n\
paddingDown=0											\n\
paddingUp=0												\n\
paddingRight=0											\n\
paddingLeft=0											\n\
spacingHoriz=1											\n\
spacingVert=1											\n\
														\n\
# output file											\n\
outWidth=256											\n\
outHeight=256											\n\
outBitDepth=32											\n\
fontDescFormat=0										\n\
fourChnlPacked=0										\n\
textureFormat=png										\n\
textureCompression=0									\n\
alphaChnl=1												\n\
redChnl=0												\n\
greenChnl=0												\n\
blueChnl=0												\n\
invA=0													\n\
invR=0													\n\
invG=0													\n\
invB=0													\n\
														\n\
# outline												\n\
outlineThickness=2										\n\
														\n\
# selected chars										\n\
chars=32-126,169");

void __declspec(dllexport) CALLBACK GenerateFont(HWND hwnd, HINSTANCE hinst, LPSTR lpszCmdLine, int nCmdShow)
{
	TCHAR	DriverDataPath[MAX_PATH];
	TCHAR	XMLFileName[MAX_PATH];
	TCHAR	TempPath[MAX_PATH];
	TCHAR	TempFileName[MAX_PATH];
	TCHAR	FontFileName[MAX_PATH];
	TCHAR	FontGeneratorDir[MAX_PATH];
	TCHAR	FontGeneratorExe[MAX_PATH];
	TCHAR	commandLine[4 * MAX_PATH];
	set<WCHAR> LocalSharSet;

	//--- make driver data path ---
	if(iz3d::resources::GetAllUsersDirectory( DriverDataPath )) 
	{		
#ifdef FINAL_RELEASE
		_tcscpy_s(XMLFileName, DriverDataPath);
#else
		if(GetModuleFileName(g_hModule, XMLFileName, _countof(XMLFileName)) != 0)
		{
			PathRemoveFileSpec(XMLFileName);	//--- remove filename ---
			PathRemoveFileSpec(XMLFileName);
		}
#endif

		std::string font;
		// Prefer wiz3D_Config.xml, fall back to legacy Config.xml.
		TCHAR XMLFileNameDir[MAX_PATH];
		_tcscpy_s(XMLFileNameDir, MAX_PATH, XMLFileName);
		PathAppend( XMLFileName, _T("wiz3D_Config.xml"));
		if (!PathFileExists(XMLFileName))
		{
			_tcscpy_s(XMLFileName, MAX_PATH, XMLFileNameDir);
			PathAppend( XMLFileName, _T("Config.xml"));
		}
		TiXmlDocument ConfigFile( common::utils::to_multibyte( XMLFileName ) );
		if(ConfigFile.LoadFile())
		{
			TiXmlHandle   docHandle( &ConfigFile );
			TiXmlElement* pItemElement = docHandle.FirstChild("Config").FirstChild("GlobalSettings").FirstChild("Language").Element();
			std::wstring languageFile = _T("English");
			if(pItemElement)
				languageFile = common::utils::to_unicode_simple( pItemElement->Attribute( "Value" ) );

			if(_tcsicmp(languageFile.c_str(), _T("English") ) != 0)	
			{
				if(iz3d::resources::GetAllUsersDirectory( XMLFileName )) 
				{
					_stprintf_s(XMLFileName + _tcslen(XMLFileName), _countof(XMLFileName) - _tcslen(XMLFileName),  _T("Language\\%s.xml"), languageFile.c_str());
					//--- read localization file ---
					std::string ansiXmlFileName = common::utils::to_multibyte( XMLFileName );
					TiXmlDocument StringList( ansiXmlFileName.c_str() );
					if(StringList.LoadFile())
					{
						TiXmlHandle docHandle( &StringList );
						pItemElement = docHandle.FirstChild("LanguagePack").ToElement();
						const char* pFont = pItemElement->Attribute( "font" );
						font = pFont ? pFont : "";
						pItemElement = pItemElement->FirstChild("Driver")->FirstChildElement();
						for( ; pItemElement; pItemElement = pItemElement->NextSiblingElement())
						{
							if(const char* s = pItemElement->GetText())
							{
								WCHAR textString[MAX_PATH];
								common::utils::UTF8ToUCS2(s, textString);
								for(size_t i=0; i< wcslen(textString); i++)
									if (textString[i] > 126)
										LocalSharSet.insert(textString[i]);
							}
						}
					}
				}
			}
		}
		if(font.empty())
			font = "Arial";
		
		//--- create font directory if not existed ---
		_stprintf_s(FontFileName, TEXT("%s\\Font"), DriverDataPath);
		if(!PathFileExists(FontFileName))
			CreateDirectory(FontFileName, 0);
		PathAppend(FontFileName, TEXT("OSDFont.fnt"));

		if(GetTempPath(_countof(TempPath), TempPath))
		{
			if(GetTempFileName(TempPath, _T("S3D"), 0, TempFileName))
			{
				FILE* fpTextFile = _tfopen(TempFileName, _T("w+") );
				if(fpTextFile)
				{
					//--- select characters ---
					fprintf(fpTextFile, BMFontConfigProlog);
					fprintf(fpTextFile, font.c_str());
					fprintf(fpTextFile, BMFontConfig);
					if(LocalSharSet.size())
						fprintf(fpTextFile, ",");
					set <WCHAR>::iterator it, kt;
					for(it = LocalSharSet.begin(); it != LocalSharSet.end(); )
					{
						int i = 0;
						kt = it;
						while(++kt != LocalSharSet.end())
						{
							if(*kt != *it + (++i))
								break;
						}
						if(i > 1)
						{
							kt--;
							fprintf(fpTextFile, "%i-%i", *it, *kt);
							it = kt;
						}
						else
							fprintf(fpTextFile, "%i", *it);
							
						if(++it != LocalSharSet.end())
							fprintf(fpTextFile, ",");
					}
					fclose(fpTextFile);

					//--- BMFont crashed when running from foreign directory ---
					GetModuleFileName(g_hModule, FontGeneratorDir, _countof(FontGeneratorDir));
					PathRemoveFileSpec(FontGeneratorDir);	//--- remove filename ---
					PathRemoveFileSpec(FontGeneratorDir);	//--- remove last dir ---
					PathAppend(FontGeneratorDir, TEXT("Win32"));
					_stprintf_s(FontGeneratorExe, TEXT("%s\\BMFont.exe"), FontGeneratorDir);
					
					//--- make command line ---
					_stprintf_s(commandLine, TEXT("\"%s\" -o \"%s\" -c \"%s\""), FontGeneratorExe, FontFileName, TempFileName);

					//--- execute font generator ---
					STARTUPINFO si;
					PROCESS_INFORMATION pi;
					ZeroMemory( &si, sizeof(si) );
					ZeroMemory( &pi, sizeof(pi) );
					si.cb = sizeof(si);
					//--- always run BMFont.exe from Win32 directory ---
					if(CreateProcess(0, commandLine, 0, 0, 0, 0, 0, FontGeneratorDir, &si, &pi))
					{
						// Wait until child process exits.
						WaitForSingleObject( pi.hProcess, INFINITE );

						SetUserSecurityLevel(FontFileName);
						PathRemoveExtension(FontFileName);
						StrCat(FontFileName, _T("_0.png"));
						SetUserSecurityLevel(FontFileName);

						// Close process and thread handles. 
						CloseHandle( pi.hThread );
						CloseHandle( pi.hProcess );
					}
				}
				//--- because GetTempFileName() create empty file ---
				DeleteFile(TempFileName);
			}
		}
	}
}
