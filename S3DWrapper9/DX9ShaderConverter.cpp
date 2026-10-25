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
#include "StdAfx.h"
#include <io.h>
#include "DX9ShaderConverter.h"
#include "version.h"
#include "Globals.h"
#include "..\CommonUtils\CommonResourceFolders.h"
#include "ProxyDevice9-inl.h"

using namespace std;

template<> ShaderCache DX9ShaderConverter<VS_PRIVATE_DATA, IDirect3DVertexShader9>::m_ShaderCacheData{};
template<> DWORD DX9ShaderConverter<VS_PRIVATE_DATA, IDirect3DVertexShader9>::m_GlobalCounter = 0;
template<> DWORD DX9ShaderConverter<VS_PRIVATE_DATA, IDirect3DVertexShader9>::m_ModifiedCounter = 0;

template<> ShaderCache DX9ShaderConverter<PS_PRIVATE_DATA, IDirect3DPixelShader9>::m_ShaderCacheData{};
template<> DWORD DX9ShaderConverter<PS_PRIVATE_DATA, IDirect3DPixelShader9>::m_GlobalCounter = 0;
template<> DWORD DX9ShaderConverter<PS_PRIVATE_DATA, IDirect3DPixelShader9>::m_ModifiedCounter = 0;


//------------------------------------ class DX9ShaderConverter -----------------------------------
template <typename T, typename Q>
DX9ShaderConverter<T, Q>::DX9ShaderConverter()
{
}

template <typename T, typename Q>
void DX9ShaderConverter<T, Q>::Initialize(ProxyDevice9* pD3DDevice)
{
	m_pD3DDevice = pD3DDevice;
	bool bCacheFound = m_ShaderCacheData.Load( );
}

template <typename T, typename Q>
char* DX9ShaderConverter<T, Q>::PrepareShaderText(char* inputShader, ID3DXBuffer** outputText, ID3DXBuffer** pComment)
{
	char *p, *s, *m, *pShaderText;
	CComPtr<ID3DXBuffer> pShader;

	D3DXCreateBuffer((DWORD)strlen(inputShader)+1, &pShader);
	pShaderText = (char*)pShader->GetBufferPointer();

	// remove comments
	m = pShaderText;
	p = inputShader;
	s = strstr(p, "//");
	while(s)
	{
		m += sprintf(m, "%.*s", s-p, p);
		while(*s && *s != '\n')
			s++;
		p = s;
		s = strstr(p, "//");
	}  
	strcpy(m, p);
		 
	// replace #line strings to spaces
	m = strstr(pShaderText, "#line");
	while(m)
	{
		while(*m && *m != '\n')
			*m++ = ' ';
		m = strstr(m, "#line");
	}	   
	
	// skip preshader commands
	m = GetShaderStartPosition(pShaderText);
	if(SUCCEEDED(D3DXCreateBuffer((DWORD)strlen(m)+1, outputText)))
		strcpy((char*)(*outputText)->GetBufferPointer(), m);

	// parse comments
	if (pComment)
	{
		*pComment = NULL;
		char* params = "// Registers:\n//\n";
		s = strstr(inputShader, params);
		if (s)
		{
			s += strlen(params);
			char* defValues = "//\n//\n// Default values:\n";
			p = strstr(s, defValues);
			if (!p)
				p = strrstr(s, "//");
			size_t cnt = p - s;
			D3DXCreateBuffer((DWORD)cnt+1, pComment);
			char* pCommentText = (char*)(*pComment)->GetBufferPointer();
			strncpy(pCommentText, s, cnt);
			pCommentText[cnt] = '\0';
		}
	}

	return m;
}

template <typename T, typename Q>
bool DX9ShaderConverter<T, Q>::FindFreeRegister( DWORD &maxTmpRegister, char* tmpRegisterName, char * first_oPos, char * after_last_oPos, char * shaderText )
{
	char *p;
	for(int i = maxTmpRegister; i >= 0; i--)
	{
		sprintf(tmpRegisterName, "r%i", i);
		p = strstr(first_oPos, tmpRegisterName);
		if(!p)
		{
			maxTmpRegister = i - 1;
			return true;  // register not used after first oPos
		}

		if(p < after_last_oPos)
			continue;  // register used inside oPos block

		p = strstr(shaderText, tmpRegisterName);
		if(p >= after_last_oPos)
		{
			maxTmpRegister = i - 1;
			return true;  // register not used before first oPos and inside oPos block
		}            

		// if register initialized again after last oPos we use it
		p = strstr(after_last_oPos, tmpRegisterName);
		char *s = p;
		// return to command
		while(*--s != '\n');
		char operand1Buffer[32];
		sscanf(s, "%*s %s", operand1Buffer);
		if(!strchr(operand1Buffer, '.'))
		{
			maxTmpRegister = i - 1;			
			return true; // all register component initialized again after last oPos
		}
	}
	return false;
}

template <typename T, typename Q>
HRESULT	DX9ShaderConverter<T, Q>::CreateShader(CONST DWORD *pFunction, IDirect3DVertexShader9** ppShader)
{
	return m_pD3DDevice->CreateVertexShader(pFunction, ppShader);
}

template <typename T, typename Q>
HRESULT	DX9ShaderConverter<T, Q>::CreateShader(CONST DWORD *pFunction, IDirect3DPixelShader9** ppShader)
{
	return m_pD3DDevice->CreatePixelShader(pFunction, ppShader);
}

template <typename T, typename Q>
HRESULT	DX9ShaderConverter<T, Q>::CreateStereoShader(CONST DWORD *pMonoFunction, UINT Size, char *shaderText, Q** ppShader)
{
	CComPtr<ID3DXBuffer> pCompiledShader;
	CComPtr<ID3DXBuffer> pCompilationErrors;
	if(shaderText)
	{
		if(SUCCEEDED(D3DXAssembleShader(shaderText, (UINT)strlen(shaderText), NULL, NULL, 0, &pCompiledShader, &pCompilationErrors)))
		{
			RouterTypeHookCallGuard<ProxyDevice9> RTdev(m_pD3DDevice);

			//--- create stereo shader ---
			DWORD* modifiedShader = (DWORD*)pCompiledShader->GetBufferPointer();
			if(FAILED(CreateShader(modifiedShader, &m_CurrentShaderData.stereoShader)))
			{
				DEBUG_MESSAGE(_T("Failed create shader\n"));
				return E_FAIL;
			}
			m_ModifiedCounter++;
		}
		else
		{
			DEBUG_MESSAGE(_T("Failed assemble shader\n"));
			DEBUG_MESSAGE(_T("Shader: %S\n"), shaderText);
			DEBUG_MESSAGE(_T("Error: %S\n"), (char*)pCompilationErrors->GetBufferPointer());
			return E_FAIL;
		}
	}

	// pack shader private data into original shader code
	DWORD  dataSizeInDwords = m_CurrentShaderData.GetDataSizeInDwords();
	DWORD  shaderDataSize = (dataSizeInDwords + 1) * sizeof(DWORD);
	CComPtr<ID3DXBuffer> newShader;
	if(SUCCEEDED(D3DXCreateBuffer(Size + shaderDataSize, &newShader)))
	{
		DWORD* p = (DWORD*)newShader->GetBufferPointer();
		p[0] = pMonoFunction[0];											//--- copy shader version ---
		p[1] = (dataSizeInDwords << 16) + 0xFFFE;						    //--- mark data as comment ---
		memcpy(&p[2], &m_CurrentShaderData, sizeof(m_CurrentShaderData));	//--- copy private data ---
		memcpy(&p[2+dataSizeInDwords], &pMonoFunction[1], Size - sizeof(DWORD));
		if(SUCCEEDED(CreateShader(p, ppShader))) {
			return S_OK;
		}
		else {
			ZLOG_MESSAGE( zlog::SV_ERROR, _T("%s: Can't create modified shader\n"), _T(__FUNCTION__) );
		}
	}
	else {
		ZLOG_MESSAGE( zlog::SV_ERROR, _T("%s: Can't create buffer for modified shader code\n"), _T(__FUNCTION__) );
	}

	HRESULT hr = CreateShader(pMonoFunction, ppShader);
	if (FAILED(hr)) {
		DEBUG_MESSAGE(_T("%s: Can't assemble stereo shader #%i"), _T(__FUNCTION__), m_CurrentShaderData.shaderGlobalIndex);
	}
	return hr;
}

template <typename T, typename Q>
void DX9ShaderConverter<T, Q>::WriteShaderTextToFile(TCHAR* fileName, char* shaderText, DWORD Size, bool complieShader)
{
#ifndef FINAL_RELEASE
	FILE * fp;

	if (m_CurrentShaderData.shaderGlobalIndex == 0) 
		fp = _tfopen( fileName, _T("w") );
	else 
		fp = _tfopen( fileName, _T("a") );

	if(!shaderText) 
	{
		if ( fp ) 
			fclose( fp );
		return;
	}
	if ( fp ) 
	{
		fprintf( fp, ";*******************************************************************************\n"
			"; Shader #%d\n\n", m_CurrentShaderData.shaderGlobalIndex);
		fprintf( fp, "ShaderCRC = 0x%X\n", m_CurrentShaderData.CRC32);
		fprintf( fp, "ShaderSize = %u\n", Size);
		fprintf( fp, "Used textures mask = 0x%X\n", m_CurrentShaderData.textures);
		WriteShaderDataToFile(fp);

		if(complieShader)
		{
			CComPtr<ID3DXBuffer> pCompiledShader;
			CComPtr<ID3DXBuffer> pCompilationErrors;
			D3DXAssembleShader(shaderText, (UINT)strlen(shaderText), NULL, NULL, 0, &pCompiledShader, &pCompilationErrors);
			if (pCompilationErrors)
			{
				char *szError = (char*)pCompilationErrors->GetBufferPointer();
				fprintf( fp, "\n;D3DXAssembleShader() failed for Shader #%d\n%s\n", m_CurrentShaderData.shaderGlobalIndex, szError);
			}
		}

		fprintf( fp, "\n%s\n", shaderText);

		fclose( fp );
	}

#endif
}

template <typename T, typename Q>
const ShaderProfileData* DX9ShaderConverter<T, Q>::FindProfile(ShaderProfileDataMap &privateDataMap)
{
#ifndef SHADERTEST	
	ShaderProfileDataMap::const_iterator i = privateDataMap.find(m_CurrentShaderData.CRC32);
	if (i != privateDataMap.end())
		return &i->second;
	i = privateDataMap.find(0);
	if (i != privateDataMap.end())
		return &i->second;
#endif
	return NULL;
}

template <typename T, typename Q >
bool DX9ShaderConverter<T, Q>::AddMatrix( int registerIndex )
{
	bool f = false;
	int i = 0;
	auto& data = m_CurrentShaderData.matricesData;
	for (; i < data.matrixSize; i++)
	{
		if (data.matrix[i].matrixRegister == registerIndex)
		{
			f = true;
			break;
		}
	}
	if (!f && data.matrixSize < SHADER_MATRIX_SIZE)
	{
		data.matrixSize++;
		data.matrix[i].matrixRegister		= registerIndex;
		data.matrix[i].matrixIsTransposed	= data.matrix[0].matrixIsTransposed;
		data.matrix[i].incorrectProjection	= FALSE;
		data.matrix[i].inverse				= FALSE;
		return true;
	}
	return false;
}


template <typename T, typename Q >
void DX9ShaderConverter<T, Q>::SearchMatrixAndAdd( char* commentText, const char* matrixName )
{
	char* s = strstr(commentText, matrixName);
	while(s)
	{
		char* begin = s;
		s += strlen(matrixName);
		if (*(begin - 1)==' ' && *s==' ')
		{
			char* c = strstr(s, "c") + 1;
			int n = atoi(c);
			c = strstr(c, " ");
			do{ c++;							
			}while(c[0] == ' ');
			int size = atoi(c);
			char* p = strstr(s, "\n");
			if (s < p && (size == 3 || size == 4))
				AddMatrix(n);
		}
		s = strstr(s, matrixName);
	}
}


template <typename T, typename Q >
void DX9ShaderConverter<T, Q>::CalculateCRCEx( CONST DWORD * pFunction, UINT Size )
{
	bool bSkipComments = false;
	CONST DWORD *command = pFunction;
	command++; // skip version
	DWORD unknownId = 0;
	DWORD unknownLength = 0;
	while (*command != D3DVS_END())
	{
		D3DSHADER_INSTRUCTION_OPCODE_TYPE opCode = (D3DSHADER_INSTRUCTION_OPCODE_TYPE)(*command & D3DSI_OPCODE_MASK);
		DWORD length = 0;
		if (opCode == D3DSIO_COMMENT) {
			length = ((*command & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT);
			DWORD id = *(command + 1);
			if (id != 0x42415443 && id != 0x53455250 && id != 0x47554244 && id != 0x58443344)
			{
				// 0x42415443 - constants table 'CTAB'
				// 0x53455250 - pre-shader 'PRES'
				// 0x47554244 - debug information 'DBUG'
				// 0x58443344 - D3DX information 'D3DX'
				bSkipComments = true;				
				unknownId = id;
				unknownLength = length;
			}
		} else if (opCode == D3DSIO_END)
			break;
		else {
			//length = ((*command & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT);
			break; // we found shader code!
		}
		command += length + 1;
	}

	if (!bSkipComments)
		m_CurrentShaderData.CRC32 = CalculateCRC32((CONST BYTE*)pFunction, Size);
	else { // for NFS Shift
		UINT codeSize = Size - (UINT)((CONST BYTE*)command - (CONST BYTE*)pFunction);
		_ASSERT(codeSize < Size);
		m_CurrentShaderData.CRC32 = CalculateCRC32((CONST BYTE*)command, codeSize,
			CalculateCRC32((CONST BYTE*)pFunction, sizeof(DWORD)));
#ifdef ZLOG_ALLOW_TRACING
		TCHAR szBuffer[8] = _T("");
		if (unknownId > 0x30303030)
		{
			_stprintf_s(szBuffer, 8, _T("\'%c%c%c%c\'"),
				(TCHAR)(unknownId & 0xFF), (TCHAR)((unknownId >> 8) & 0xFF), 
				(TCHAR)((unknownId >> 16) & 0xFF), (TCHAR)((unknownId >> 24) & 0xFF));
		}
		DEBUG_TRACE1(_T("\tShader CRC 0x%X: Comment 0x%X %s(length %d)\n"), 
			m_CurrentShaderData.CRC32, unknownId, szBuffer, unknownLength);
#endif
	}
}

DX9VertexShaderConverter::DX9VertexShaderConverter()
{
	m_NumTempRegisters = 0;
	m_FreeConstantRegister = 0;
	m_FreeConstantRegister2 = 0;
	m_ShaderCacheData.Init(_T("vsc"));
}

void DX9VertexShaderConverter::Initialize(ProxyDevice9* pD3DDevice)
{
	DX9ShaderConverter<VS_PRIVATE_DATA, IDirect3DVertexShader9>::Initialize(pD3DDevice);
	D3DCAPS9 D3DCaps;
	pD3DDevice->GetDeviceCaps( &D3DCaps );
	m_NumTempRegisters = D3DCaps.VS20Caps.NumTemps;
	m_FreeConstantRegister = (WORD)D3DCaps.MaxVertexShaderConst - 2;
	m_FreeConstantRegister2 = m_FreeConstantRegister - 1;
}

ShaderAnalysisResult DX9VertexShaderConverter::Analyze(DWORD shaderVersion, char *shaderText)
{
	m_CurrentShaderData.textures = 0;
	ShaderAnalyzer sa;
	ShaderAnalysisResult result = sa.Analyze(shaderVersion, shaderText, m_CurrentShaderData.matricesData, m_CurrentShaderData.textures);
	//--- incorrect shader is pixel shader in CreateVertexShader() function ---
	//--- we MUST not try to modify such shaders ---
	if(result == AnalysisSharedIsMono || result == AnalysisIncorrectShader)
		m_CurrentShaderData.shaderIsMono = true;
	return result;
}

// TODO:  ('-') not supported yet
// TODO:  abs(Cxx) not supported yet
void DX9VertexShaderConverter::Convert_1_x_ShaderTo_2_x(DWORD shaderVersion, char* shaderText, ID3DXBuffer** outputText)
{
	char tmpRegisterName[16];
	char operand1Buffer[32];
	char operand2Buffer[32];
	char swizzle1Buffer[32];
	D3DXCreateBuffer((DWORD)(16 * (strlen(shaderText)+1)), outputText);
	char *convertedShaderText = (char*)(*outputText)->GetBufferPointer();

	// expp work in other manner
	// mov a - incorrect round in vs.1.1 and cut  in vs.2.x
	// do not search of free temporary register
	sprintf(tmpRegisterName, "r%u", m_NumTempRegisters - 1);

	_ASSERT(shaderVersion < 0x200);
	char *p = GetShaderStartPosition(shaderText);
	if(p[0] != '\0')
	{
		p += 6; // skip vs.1.x tag
		char *m = convertedShaderText; 
		m += sprintf(m, "vs.2.x");
		while(1)
		{
			// find next command need to converted
			char *s1 = strstr(p, "mov a");
			char *s2 = strstr(p, "expp");
			if(!s1 && !s2)
				break;
			if(s1 && s1 < s2)
			{
				// modify "mov a0.x, reg" command
				char *s = s1;
				*swizzle1Buffer = 0;
				sscanf(s, "mov a0.x, %s", operand1Buffer); // get source register for mov a0.x, ..  command

				char *t = strchr(operand1Buffer, '.');
				if(t)
					strcpy(swizzle1Buffer, t);
				while(*(--s-1) != '\n');
				m += sprintf(m, "%.*s", s-p, p);
				m += sprintf(m, "frc %s%s, %s\n", tmpRegisterName,swizzle1Buffer, operand1Buffer);
				m += sprintf(m, "sub %s%s, %s, %s%s\n", tmpRegisterName,swizzle1Buffer, operand1Buffer, tmpRegisterName,swizzle1Buffer);
				m += sprintf(m, "mova a0.x, %s%s\n", tmpRegisterName,swizzle1Buffer);
				while(*s && *s != '\n')  // go to next command
					s++;
				p = s + 1;
			}
			else
			{
				// modify "expp" command
				char *s = s2;
				*swizzle1Buffer = 0;
				sscanf(s, "expp %s %s", operand1Buffer, operand2Buffer); // get source and destimation register for expp command
				char *t = strchr(operand1Buffer, '.');
				if(t)
				{
					strcpy(swizzle1Buffer, t);
					swizzle1Buffer[strlen(swizzle1Buffer)-1] = 0; // remove ',' at the end of string
				}
				while(*(--s-1) != '\n');
				m += sprintf(m, "%.*s", s-p, p);
				m += sprintf(m, "frc %s.y, %s\n", tmpRegisterName, operand2Buffer);
				m += sprintf(m, "sub %s.x, %s, %s.y\n", tmpRegisterName, operand2Buffer, tmpRegisterName);
				m += sprintf(m, "exp %s.x, %s.x\n", tmpRegisterName, tmpRegisterName);
				m += sprintf(m, "exp %s.z, %s\n", tmpRegisterName, operand2Buffer);
				m += sprintf(m, "mov %s.w, c%i.x\n", tmpRegisterName, m_FreeConstantRegister);
				m += sprintf(m, "mov %s %s%s\n", operand1Buffer, tmpRegisterName,swizzle1Buffer);
				while(*s && *s != '\n')  // go to next command
					s++;
				p = s + 1;
			}
		}
		strcpy(m, p);
	}
	else
		strcpy(convertedShaderText, shaderText);
}

bool DX9VertexShaderConverter::Modify(DWORD shaderVersion, char *shaderText, ID3DXBuffer** outputText, bool AddZNearCheck)
{
	char *p, *s, *m;
	char tmpRegisterName[16] = "";
	char tmpRegisterName2[16] = "";
	char oPosName[16];
	if(!GetShaderoPosName(shaderVersion, shaderText, oPosName))
	{
		DEBUG_MESSAGE(_T("%s: Warning! oPos register not found!\n"), _T(__FUNCTION__));
		return false;
	}

	*outputText = NULL;
	CComPtr<ID3DXBuffer> pOutputData;
	// reserve additional space for conversion
	D3DXCreateBuffer((DWORD)(16 * (strlen(shaderText)+1)), &pOutputData);
	char *modifiedShaderText  = (char*)pOutputData->GetBufferPointer();

	char *first_oPos = shaderText;
	if(shaderVersion >= 0x300)  // in vs.3.0 first oPos is declaration 
	{
		first_oPos = strstr(first_oPos + 1, oPosName);
		if(!first_oPos)
		{
			DEBUG_MESSAGE(_T("%s: Error! Can't find oX register in vs.3.0. Shader #%i\n"), _T(__FUNCTION__),m_CurrentShaderData.shaderGlobalIndex);
			return false;
		}
	}

	// find unused temporary register
	first_oPos = strstr(first_oPos + 1, oPosName);
	if(!first_oPos)
	{
		DEBUG_MESSAGE(_T("%s: Error! Can't find oPos/oX register. Shader #%i\n"), _T(__FUNCTION__),m_CurrentShaderData.shaderGlobalIndex);
		return false;
	}

	char *after_last_oPos = strrstr(shaderText, oPosName);
	while(*after_last_oPos &&  *after_last_oPos != '\n')
		after_last_oPos++;

	DWORD maxTmpRegister = 31;
	if(shaderVersion < 0x300)
		maxTmpRegister = m_NumTempRegisters - 1;
	if(shaderVersion <= 0x200)
		maxTmpRegister = 12 - 1;

	bool registerFound = FindFreeRegister(maxTmpRegister, tmpRegisterName, first_oPos, after_last_oPos, shaderText);

	if (registerFound && (AddZNearCheck || gInfo.VertexShaderModificationMethod != 0))
	{
		registerFound = FindFreeRegister(maxTmpRegister, tmpRegisterName2, first_oPos, after_last_oPos, shaderText);
	}

	if(!registerFound)
	{
		DEBUG_MESSAGE(_T("%s: Error! Can't find free temporary register. Shader #%i\n"), _T(__FUNCTION__),m_CurrentShaderData.shaderGlobalIndex);
		return false;
	}

	m = modifiedShaderText;  
	m += sprintf(m, "%.*s", first_oPos-shaderText, shaderText);

	// TODO: need more careful in finding an unused const
	// Serious bug on NVida card!  Do not use c255 register 
	m_CurrentShaderData.dp4VectorRegister = m_FreeConstantRegister;

	// modify vertex shader for stereo rendering
	p = first_oPos;
	s = strstr(p, oPosName);
	while(s)
	{                                  
		m += sprintf(m, "%.*s%s", s-p, p, tmpRegisterName);
		p = s+strlen(oPosName);
		while(*p && *p != '\n')
			m += sprintf(m, "%c", *p++);
		s = strstr(p, oPosName);
	}
	int n = sprintf(m, p);
	m += n;
	if (!AddZNearCheck)
	{
		switch(gInfo.VertexShaderModificationMethod)
		{
		case 1: // WoW Hybrid 2D Objects
			// mad tmpReg2.x, tmpReg.w, const.z, tmpReg.x
			m += sprintf(m, "\nmad %s.x, %s.w, c%i.z, %s.x\n", tmpRegisterName2, tmpRegisterName, m_FreeConstantRegister, tmpRegisterName);
			// add oPos.x, tmpReg2.x, const.w
			m += sprintf(m, "add %s.x, %s.x, c%i.w\n", oPosName, tmpRegisterName2, m_FreeConstantRegister);
			// mov oPos.yzw, tmpReg
			m += sprintf(m, "mov %s.yzw, %s\n", oPosName, tmpRegisterName);
			break;
		case 2: // NVidia-like method
			// add tmpReg2.x, tmpReg.w, -const.y
			m += sprintf(m, "\nadd %s.x, %s.w, -c%i.y\n", tmpRegisterName2, tmpRegisterName, m_FreeConstantRegister);
			// mad oPos.x, tmpReg2.x, const.x, tmpReg.x
			m += sprintf(m, "mad %s.x, %s.x, c%i.x, %s.x\n", oPosName, tmpRegisterName2, m_FreeConstantRegister, tmpRegisterName);
			// mov oPos.yzw, tmpReg
			m += sprintf(m, "mov %s.yzw, %s\n", oPosName, tmpRegisterName);
			break;
		default:
			// dp4 oPos.x, tmpReg, const
			m += sprintf(m, "\ndp4 %s.x, %s, c%i\n", oPosName, tmpRegisterName, m_FreeConstantRegister);
			// mov oPos.yzw, tmpReg
			m += sprintf(m, "mov %s.yzw, %s\n", oPosName, tmpRegisterName);
			break;
		}
	}
	else
	{
		switch(gInfo.VertexShaderModificationMethod)
		{
		case 1:
			m_CurrentShaderData.ZNearRegister = m_FreeConstantRegister2;
			// mad tmpReg2.x, tmpReg.w, const.z, tmpReg.x
			m += sprintf(m, "\nmad %s.x, %s.w, c%i.z, %s.x\n", tmpRegisterName2, tmpRegisterName, m_FreeConstantRegister, tmpRegisterName);
			// add tmpReg2.x, tmpReg2.x, const.w
			m += sprintf(m, "add %s.x, %s.x, c%i.w\n", tmpRegisterName2, tmpRegisterName2, m_FreeConstantRegister);
			// add tmpReg2.x, -tmpReg.x, tmpReg2.x
			m += sprintf(m, "add %s.x, -%s.x, %s.x\n", tmpRegisterName2, tmpRegisterName, tmpRegisterName2);
			// slt tmpReg2.y, const2.w, tmpReg.w
			m += sprintf(m, "slt %s.y, c%i.w, %s.w\n", tmpRegisterName2, m_FreeConstantRegister2, tmpRegisterName);
			// mad oPos.x, tmpReg2.y, tmpReg2.x, tmpReg.x
			m += sprintf(m, "mad %s.x, %s.y, %s.x, %s.x\n", oPosName, tmpRegisterName2, tmpRegisterName2, tmpRegisterName);
			// mov oPos.yzw, tmpReg
			m += sprintf(m, "mov %s.yzw, %s\n", oPosName, tmpRegisterName);
			break;
		case 2: // NVidia-like Ex method
			m_CurrentShaderData.ZNearRegister = m_FreeConstantRegister;
			// slt tmpReg2.y, const.w, tmpReg.w
			m += sprintf(m, "\nslt %s.y, c%i.w, %s.w\n", tmpRegisterName2, m_FreeConstantRegister, tmpRegisterName);
			// mul tmpReg2.y, tmpReg2.y, const.x
			m += sprintf(m, "mul %s.y, %s.y, c%i.x\n", tmpRegisterName2, tmpRegisterName2, m_FreeConstantRegister);
			// add tmpReg2.x, tmpReg.w, -const.y
			m += sprintf(m, "add %s.x, %s.w, -c%i.y\n", tmpRegisterName2, tmpRegisterName, m_FreeConstantRegister);
			// mad oPos.x, tmpReg2.x, tmpReg2.y, tmpReg.x
			m += sprintf(m, "mad %s.x, %s.x, %s.y, %s.x\n", oPosName, tmpRegisterName2, tmpRegisterName2, tmpRegisterName);
			// mov oPos.yzw, tmpReg
			m += sprintf(m, "mov %s.yzw, %s\n", oPosName, tmpRegisterName);
			break;
		default:
			m_CurrentShaderData.ZNearRegister = m_FreeConstantRegister2;
			// dp4 tmpReg2.x, tmpReg, const
			m += sprintf(m, "\ndp4 %s.x, %s, c%i\n", tmpRegisterName2, tmpRegisterName, m_FreeConstantRegister);
			// add tmpReg2.x, -tmpReg.x, tmpReg2.x
			m += sprintf(m, "add %s.x, -%s.x, %s.x\n", tmpRegisterName2, tmpRegisterName, tmpRegisterName2);
			// slt tmpReg2.y, const2.w, tmpReg.w
			m += sprintf(m, "slt %s.y, c%i.w, %s.w\n", tmpRegisterName2, m_FreeConstantRegister2, tmpRegisterName);
			// mad oPos.x, tmpReg2.y, tmpReg2.x, tmpReg.x
			m += sprintf(m, "mad %s.x, %s.y, %s.x, %s.x\n", oPosName, tmpRegisterName2, tmpRegisterName2, tmpRegisterName);
			// mov oPos.yzw, tmpReg
			m += sprintf(m, "mov %s.yzw, %s\n", oPosName, tmpRegisterName);
			break;
		}
	}
	*outputText = pOutputData;
	pOutputData.Detach();
	return true;
}

HRESULT DX9VertexShaderConverter::Convert(CONST DWORD *pFunction, IDirect3DVertexShader9** ppShader, IDirect3DVertexShader9** ppModifiedShader )
{
	HRESULT hResult = E_FAIL;
	*ppModifiedShader = NULL;
	CComPtr<ID3DXBuffer> pModifiedShader;
	UINT Size = D3DXGetShaderSize(pFunction);
	m_CurrentShaderData.Clear();
	m_CurrentShaderData.shaderGlobalIndex = m_GlobalCounter;
	CalculateCRCEx(pFunction, Size);

	if(ProcessShader(pFunction, Size, pModifiedShader))
	{
		char* modifiedShaderText = pModifiedShader ? (char*)pModifiedShader->GetBufferPointer() : NULL;
		hResult = CreateStereoShader(pFunction, Size, modifiedShaderText, ppShader);		
		*ppModifiedShader = m_CurrentShaderData.stereoShader;
	}
	else
	{
		hResult = CreateShader(pFunction, ppShader);
	}

	m_GlobalCounter++;
	return hResult;
}

bool DX9VertexShaderConverter::ProcessShader( CONST DWORD * pFunction, UINT Size, CComPtr<ID3DXBuffer> &pModifiedShader )
{
#ifndef FINAL_RELEASE
	LARGE_INTEGER currentTime;
	QueryPerformanceCounter(&currentTime);
#endif
	char* pureShaderText = 0;
	char* inputShaderText = 0;
	char* convertedShaderText = 0;
	CComPtr<ID3DXBuffer> pureShader;
	CComPtr<ID3DXBuffer> pDisassembledShader;
	CComPtr<ID3DXBuffer> pConvertedShader;
	CComPtr<ID3DXBuffer> pComment;
	SHADER_CACHE_DATA_MAP_ITERATOR iCacheData;
	bool bCached = false;
	bool bDisassembled = false;

	// 0. Skip invalid shaders (non-vertex type).  The Wither 2 game bug.
	DWORD shaderType =  D3DXGetShaderVersion(pFunction) >> 16 & 0xFFFF;
	if (shaderType != VS_SHADER_TYPE)
	{
		DEBUG_MESSAGE(_T("WARNING! Incorrect shader type (pixel instead of vertex) in %s.\n"), _T(__FUNCTION__));
		return false;
	}

	const ShaderProfileData* profile = FindProfile(g_ProfileData.VSCRCData);
	// 1. Check for "shader is mono"
	if (profile && profile->m_Multiplier == 0.0f)
		m_CurrentShaderData.shaderIsMono = TRUE;
	if (!m_CurrentShaderData.shaderIsMono)
	{
		iCacheData = m_ShaderCacheData.find( m_CurrentShaderData.CRC32 );
		if( iCacheData != m_ShaderCacheData.end() )
		{
			m_CurrentShaderData.shaderIsMono = (iCacheData->second.flags & SHADER_CACHE_DATA::isShaderMono) != 0;
			bCached = true;
		}
	}

	// 2. Analyze VS
#ifndef FINAL_RELEASE
	bDisassembled = SUCCEEDED(D3DXDisassembleShader(pFunction, 0, 0, &pDisassembledShader));
	if( bDisassembled )
	{
		inputShaderText = (char*)pDisassembledShader->GetBufferPointer();
		PrepareShaderText(inputShaderText, &pureShader, &pComment);
		pureShaderText = (char*)pureShader->GetBufferPointer();
	}
#endif

	// for example:  version = 0x200
	DWORD shaderVersion = D3DXGetShaderVersion(pFunction) & 0xFFFF;
	if (shaderVersion > 0x300) // shader code bug
		m_CurrentShaderData.shaderIsMono = true;
	bool bModifyShader = false;
	bool AddZNearCheck = false;
	float ZNear = 1.0f;

	if(!m_CurrentShaderData.shaderIsMono)
	{
		if (!bModifyShader && profile && profile->m_bModifyShader)
		{
			bModifyShader = true;
			AddZNearCheck = profile->m_bAddZNearCheck;
			ZNear = profile->m_ZNear;
		}

		if(!bModifyShader)
		{
			bool bAnalyzeOkay = false;
			ShaderAnalysisResult analysisResult = AnalysisIncorrectShader;
			if (profile && profile->m_pMatrices)
			{
				m_CurrentShaderData.matricesData = *profile->m_pMatrices;
				analysisResult = AnalysisMatrixFound;
				bAnalyzeOkay = true;
			}
			if (!bAnalyzeOkay)
			{
				if( !bCached )
				{
#ifdef FINAL_RELEASE
					bDisassembled = SUCCEEDED(D3DXDisassembleShader(pFunction, 0, 0, &pDisassembledShader));
					if( bDisassembled )
					{
						inputShaderText = (char*)pDisassembledShader->GetBufferPointer();
						PrepareShaderText(inputShaderText, &pureShader, &pComment);
						pureShaderText = (char*)pureShader->GetBufferPointer();
					}
#endif
					analysisResult = Analyze(shaderVersion, pureShaderText);
					//--- do not save incorrect shader to cache ---
					if(analysisResult != AnalysisIncorrectShader)
					{
						bAnalyzeOkay = ((analysisResult == AnalysisMatrixFound) || (analysisResult == AnalysisSharedIsMono)) ? true : false;

						// save VS data to VS cache
						SHADER_CACHE_DATA		cacheData;
						cacheData.CRC32				= m_CurrentShaderData.CRC32;
						cacheData.matrixSize		= std::min<WORD>(m_CurrentShaderData.matricesData.matrixSize, 1);
						cacheData.matrixRegister	= (WORD) m_CurrentShaderData.matricesData.matrix[0].matrixRegister;
						cacheData.flags				= (WORD)(m_CurrentShaderData.matricesData.matrix[0].incorrectProjection ? SHADER_CACHE_DATA::isIncorrectProj : 0) \
							| (m_CurrentShaderData.matricesData.matrix[0].matrixIsTransposed ? SHADER_CACHE_DATA::isTransposed : 0) \
							| (m_CurrentShaderData.shaderIsMono ? SHADER_CACHE_DATA::isShaderMono : 0) \
							| (bAnalyzeOkay	? SHADER_CACHE_DATA::isAnalyzeOkay : 0);
						cacheData.textures = m_CurrentShaderData.textures;
						cacheData.reserved = 0;
						m_ShaderCacheData.insert( cacheData.CRC32, cacheData );
					}
				} 
				else
				{
					m_CurrentShaderData.textures								= iCacheData->second.textures;
					auto& data = m_CurrentShaderData.matricesData;
					data.matrixSize						= (BYTE)iCacheData->second.matrixSize;
					data.matrix[0].inverse				= FALSE;
					data.matrix[0].matrixRegister		= iCacheData->second.matrixRegister;
					data.matrix[0].incorrectProjection	= (iCacheData->second.flags & SHADER_CACHE_DATA::isIncorrectProj) != 0;
					data.matrix[0].matrixIsTransposed	= (iCacheData->second.flags & SHADER_CACHE_DATA::isTransposed) != 0;
					//m_CurrentVSData.shaderIsMono		= (iCacheData->second.flags & VS_SHADER_CACHE_DATA::isShaderMono) != 0;
					bAnalyzeOkay						= (iCacheData->second.flags & SHADER_CACHE_DATA::isAnalyzeOkay) != 0;
				}
			}
			if (bAnalyzeOkay)
			{
				if(gInfo.RenderIncorrectProjectionShadersInMono)
				{
					BOOL Stereo = FALSE;
					auto& data = m_CurrentShaderData.matricesData;
					for(DWORD i = 0; i < m_CurrentShaderData.matricesData.matrixSize; i++)
						if (!m_CurrentShaderData.matricesData.matrix[i].incorrectProjection)
							Stereo = TRUE;
					if (!Stereo)
						m_CurrentShaderData.shaderIsMono = true;
				}
				if (!m_CurrentShaderData.shaderIsMono)
				{
					// 0.4.0+: wiz3D_Config.xml -> TryModifyAllShaders=1 is a global
					// user-opt-in equivalent of a per-game <AlwaysModifyShaders=1>, so
					// a user hitting a flat-shader symptom in an untuned game can flip
					// the switch without having to patch BaseProfile.xml by hand.
					if(gInfo.AlwaysModifyShaders || gInfo.TryModifyAllShaders)
						bModifyShader = true;
					else if(gInfo.ModifyIncorrectProjectionShaders)
					{
						auto& data = m_CurrentShaderData.matricesData;
						for(DWORD i = 0; i < data.matrixSize; i++)
							if (data.matrix[i].incorrectProjection)
							{
								bModifyShader = true;
								break;
							}
					}
					if (!bModifyShader && !g_ProfileData.AdditionalMatrixName.empty())
					{
#ifdef FINAL_RELEASE
						if( !bDisassembled )
						{
							bDisassembled = SUCCEEDED(D3DXDisassembleShader(pFunction, 0, 0, &pDisassembledShader));
							if( bDisassembled )
							{
								inputShaderText = (char*)pDisassembledShader->GetBufferPointer();
								PrepareShaderText(inputShaderText, &pureShader, &pComment);
								pureShaderText = (char*)pureShader->GetBufferPointer();
							}
						}
#endif
						char* commentText = pComment ? (char*) pComment->GetBufferPointer() : NULL;
						if (commentText)
						{
							std::list<std::string>::iterator it = g_ProfileData.AdditionalMatrixName.begin();
							for (; it != g_ProfileData.AdditionalMatrixName.end(); ++it)
							{
								const char* matrixName = (*it).c_str();
								SearchMatrixAndAdd(commentText, matrixName);
							}
						}
					}
				}
			}
			else
			{
				DEBUG_MESSAGE(_T("%s: Warning! Matrix search unsuccessful in shader #%i.\n"), _T(__FUNCTION__), m_CurrentShaderData.shaderGlobalIndex);
				if(analysisResult != AnalysisIncorrectShader)
				{
					if(gInfo.RenderNonMatrixShadersInMono)
						m_CurrentShaderData.shaderIsMono = true;
					else
						bModifyShader = true;
				}
			}
		}
	}

	// 3. Modify the VS
	if (bModifyShader)
	{
#ifdef FINAL_RELEASE
		if( !bDisassembled )
		{
			bDisassembled = SUCCEEDED(D3DXDisassembleShader(pFunction, 0, 0, &pDisassembledShader));
			if( bDisassembled )
			{
				inputShaderText = (char*)pDisassembledShader->GetBufferPointer();
				PrepareShaderText(inputShaderText, &pureShader);
				pureShaderText = (char*)pureShader->GetBufferPointer();
			}
		}
#endif
		bool bSucceded = Modify(shaderVersion, pureShaderText, &pModifiedShader, AddZNearCheck);
		if(!bSucceded && shaderVersion < 0x200)
		{
			Convert_1_x_ShaderTo_2_x(shaderVersion, pureShaderText, &pConvertedShader);
			convertedShaderText = (char*)pConvertedShader->GetBufferPointer();
			shaderVersion = 0x200;
			Modify(shaderVersion, convertedShaderText, &pModifiedShader, AddZNearCheck);
		}
	}

	char* modifiedShaderText = pModifiedShader ? (char*)pModifiedShader->GetBufferPointer() : NULL;

#ifndef FINAL_RELEASE
	LARGE_INTEGER stop;
	QueryPerformanceCounter(&stop);
	analyzingTime.QuadPart = stop.QuadPart - currentTime.QuadPart;
#endif

#ifdef ZLOG_ALLOW_TRACING
	if(GINFO_DEBUG && gInfo.DumpVS) 
	{
		TCHAR shader_file[ MAX_PATH ];
		_tcscpy(shader_file, gData.DumpDirectory);
		PathAppend(shader_file, _T("vs.client.txt"));
		WriteShaderTextToFile(shader_file, inputShaderText, Size);

		//_tcscpy( shader_file, gData.LogFileName );
		//_tcscpy( &shader_file[ _tcslen( shader_file ) - 4 ], _T("_vs.no.comments.txt") );
		//WriteVSTextToFile(shader_file, pureShaderText, &m_CurrentVSData, Size);

		_tcscpy(shader_file, gData.DumpDirectory);
		PathAppend(shader_file, _T("vs.converted.txt"));
		WriteShaderTextToFile(shader_file, convertedShaderText, Size, true);
		
		_tcscpy(shader_file, gData.DumpDirectory);
		PathAppend(shader_file, _T("vs.modified.txt"));
		WriteShaderTextToFile(shader_file, modifiedShaderText, Size, true);
	}
#endif

	return m_CurrentShaderData.shaderIsMono || m_CurrentShaderData.matricesData.matrixSize != 0 || modifiedShaderText;
}

void DX9VertexShaderConverter::WriteShaderDataToFile( FILE * fp )
{
	if(m_CurrentShaderData.shaderIsMono)
		fprintf(fp, "Shader is mono.\n");
	if(m_CurrentShaderData.dp4VectorRegister != 0)
		fprintf(fp, "Shader is modified.\n");
	auto& data = m_CurrentShaderData.matricesData;
	if(data.matrixSize != 0)
	{
		fprintf(fp, "Total %i matricesData.\n", data.matrixSize);
		for (DWORD i = 0; i < data.matrixSize; i++)
		{
			fprintf(fp, "Matrix %u is %s\n", i, data.matrix[i].matrixIsTransposed ? "transposed" : "normal");
			fprintf(fp, "Matrix %u start register: %u\n", i, data.matrix[i].matrixRegister);
			fprintf(fp, "Matrix %u projection is: %s\n", i, data.matrix[i].incorrectProjection ?  "incorrect" : "correct");
		}
	}

	if(!m_CurrentShaderData.shaderIsMono && data.matrixSize == 0)
		fprintf(fp, "Warning! Shader type is unknown.\n");
}

DX9PixelShaderConverter::DX9PixelShaderConverter()
{
	m_ShaderCacheData.Init(_T("psc"));
}

void DX9PixelShaderConverter::Initialize(ProxyDevice9* pD3DDevice)
{
	DX9ShaderConverter<PS_PRIVATE_DATA, IDirect3DPixelShader9>::Initialize(pD3DDevice);
}

template <size_t _Size>
extern inline bool isStrEqual(const char *str, const char (&subStr)[_Size], bool ignoreEnd = false);
extern const char *skipSpaces(const char *text);

HRESULT DX9PixelShaderConverter::Convert(CONST DWORD *pFunction, IDirect3DPixelShader9** ppShader, IDirect3DPixelShader9** ppModifiedShader )
{
	HRESULT hResult = E_FAIL;
	*ppModifiedShader = NULL;
	CComPtr<ID3DXBuffer> pModifiedShader;
	UINT Size = D3DXGetShaderSize(pFunction);
	m_CurrentShaderData.Clear();
	m_CurrentShaderData.shaderGlobalIndex = m_GlobalCounter;
	CalculateCRCEx(pFunction, Size);

	if(ProcessShader(pFunction, Size, pModifiedShader))
	{
		char* modifiedShaderText = pModifiedShader ? (char*)pModifiedShader->GetBufferPointer() : NULL;
		hResult = CreateStereoShader(pFunction, Size, modifiedShaderText, ppShader);		
		*ppModifiedShader = m_CurrentShaderData.stereoShader;
	}
	else
	{
		hResult = CreateShader(pFunction, ppShader);
	}

	m_GlobalCounter++;
	return hResult;
}

bool DX9PixelShaderConverter::ProcessShader( CONST DWORD * pFunction, UINT Size, CComPtr<ID3DXBuffer> &pModifiedShader )
{
#ifndef FINAL_RELEASE
	LARGE_INTEGER currentTime;
	QueryPerformanceCounter(&currentTime);
#endif
	bool bAddCRC = false;
	//#ifndef FINAL_RELEASE
	char* inputShaderText = 0;
	char* pureShaderText = 0;
	CComPtr<ID3DXBuffer> pureShader;
	CComPtr<ID3DXBuffer> pDisassembledShader;

	//--- skip non-pixel shaders ---
	DWORD shaderType =  D3DXGetShaderVersion(pFunction) >> 16 & 0xFFFF;
	if (shaderType != PS_SHADER_TYPE)
	{
		DEBUG_MESSAGE(_T("WARNING! Incorrect shader type (vertex instead of pixel) in %s.\n"), _T(__FUNCTION__));
		return false;
	}

	DWORD shaderVersion = D3DXGetShaderVersion(pFunction) & 0xFFFF;

	const ShaderProfileData* profile = FindProfile(g_ProfileData.PSCRCData);
	if (profile && profile->m_pMatrices)
		m_CurrentShaderData.matricesData = *profile->m_pMatrices;

	bAddCRC = true;
#ifndef SHADERTEST	
	bool bDisassembled = false;
#ifndef FINAL_RELEASE
	bDisassembled = SUCCEEDED(D3DXDisassembleShader(pFunction, 0, 0, &pDisassembledShader));

	////--- test PS converter ---
	//pDisassembledShader.Release();
	//FILE* fp = fopen("c:\\1\\ps.txt", "r");
	//char pST[65536];
	//ZeroMemory(pST, 65536);
	//fread(pST, sizeof(char), 65536, fp);
	//fclose(fp);
	//D3DXCreateBuffer(strlen(pST)+1, &pDisassembledShader);
	//strcpy((char*)pDisassembledShader->GetBufferPointer(), pST);
#endif
	SHADER_CACHE_DATA_MAP_ITERATOR iCacheData = m_ShaderCacheData.find( m_CurrentShaderData.CRC32 );
	if( iCacheData == m_ShaderCacheData.end() )
	{
#ifdef FINAL_RELEASE
		bDisassembled = SUCCEEDED(D3DXDisassembleShader(pFunction, 0, 0, &pDisassembledShader));
#endif
		if (bDisassembled)
		{
			inputShaderText = (char*)pDisassembledShader->GetBufferPointer();
			PrepareShaderText(inputShaderText , &pureShader);
			pureShaderText = (char*)pureShader->GetBufferPointer();

			Analyze(shaderVersion, pureShaderText);

			// save PS data to PS cache
			SHADER_CACHE_DATA		cacheData;
			cacheData.CRC32			= m_CurrentShaderData.CRC32;
			cacheData.textures		= m_CurrentShaderData.textures;
			cacheData.matrixSize	= 0;
			cacheData.flags			= (m_CurrentShaderData.vPosUsed	? SHADER_CACHE_DATA::isvPosUsed : 0) |
									  (m_CurrentShaderData.shaderIsHeavy ? SHADER_CACHE_DATA::isShaderHeavy : 0);
			cacheData.reserved		= 0;
			m_ShaderCacheData.insert( cacheData.CRC32, cacheData );
		}
	}
	else
	{
#ifndef FINAL_RELEASE
		inputShaderText = (char*)pDisassembledShader->GetBufferPointer();
#endif
		m_CurrentShaderData.textures			= iCacheData->second.textures;
		m_CurrentShaderData.vPosUsed			= (iCacheData->second.flags & SHADER_CACHE_DATA::isvPosUsed) != 0;
		m_CurrentShaderData.shaderIsHeavy		= (iCacheData->second.flags & SHADER_CACHE_DATA::isShaderHeavy) != 0;
	}
	m_CurrentShaderData.splitThisShaderDrawPass = m_CurrentShaderData.shaderIsHeavy && 
		( USE_MULTI_THREAD_PRESENTER || USE_MULTI_DEVICE_PRESENTER || USE_UM_PRESENTER );	// use splitting for specific shutter modes only!
	if (m_CurrentShaderData.vPosUsed && gInfo.WideRenderTarget)
	{
		if (!bDisassembled)
		{
			bDisassembled = SUCCEEDED(D3DXDisassembleShader(pFunction, 0, 0, &pDisassembledShader));
		}
		if (bDisassembled && pureShaderText == NULL)
		{
			inputShaderText = (char*)pDisassembledShader->GetBufferPointer();
			PrepareShaderText(inputShaderText , &pureShader);
			pureShaderText = (char*)pureShader->GetBufferPointer();
		}

		Modify(shaderVersion, pureShaderText, &pModifiedShader);
	}
#endif
	//#endif

	char* modifiedShaderText = pModifiedShader ? (char*)pModifiedShader->GetBufferPointer() : NULL;

#ifndef FINAL_RELEASE
	LARGE_INTEGER stop;
	QueryPerformanceCounter(&stop);
	analyzingTime.QuadPart= stop.QuadPart - currentTime.QuadPart;
#endif	

#ifdef ZLOG_ALLOW_TRACING
	if(GINFO_DEBUG && gInfo.DumpPS) 
	{
		TCHAR shader_file[ MAX_PATH ];
		_tcscpy(shader_file, gData.DumpDirectory);
		PathAppend(shader_file, _T("ps.client.txt"));
		WriteShaderTextToFile(shader_file, inputShaderText, Size);

		//_tcscpy( shader_file, gData.LogFileName );
		//_tcscpy( &shader_file[ _tcslen( shader_file ) - 4 ], _T("_ps.no.comments.txt") );
		//WriteShaderTextToFile(shader_file, pureShaderText, Size);
		
		_tcscpy(shader_file, gData.DumpDirectory);
		PathAppend(shader_file, _T("ps.modified.txt"));
		WriteShaderTextToFile(shader_file, modifiedShaderText, Size, true);
	}
#endif

	return bAddCRC || modifiedShaderText;
}

void DX9PixelShaderConverter::WriteShaderDataToFile( FILE * fp )
{
	fprintf( fp, "vPosUsed = %d\n", m_CurrentShaderData.vPosUsed );
	fprintf( fp, "shaderIsHeavy = %d\n", m_CurrentShaderData.shaderIsHeavy );
}

bool DX9PixelShaderConverter::Modify( DWORD /*shaderVersion*/, char *shaderText, ID3DXBuffer** outputText )
{
	char	*p, *s, *m;
	char	tmpRegisterName[16] = "";
	char*	vPosName = "vPos";
	char	writeMask[5] = "";

	// reserve additional space for conversion
	D3DXCreateBuffer((DWORD)(16 * (strlen(shaderText)+1)), outputText);
	char *modifiedShaderText  = (char*)(*outputText)->GetBufferPointer();

	char *first_vPos = strstr(shaderText, vPosName); //--- first vPos is declaration ---
	if(first_vPos)
	{
		first_vPos += 5;
		ptrdiff_t n = (ptrdiff_t)(strchr(first_vPos, '\n') - first_vPos);
		strncpy(writeMask, first_vPos, n);
		first_vPos = strstr(first_vPos + 1, vPosName);

		char *after_last_vPos = strrstr(shaderText, vPosName);
		while(*after_last_vPos &&  *after_last_vPos != '\n')
			after_last_vPos++;

		if(first_vPos)
		{
			char *after_last_vPos = strrstr(shaderText, vPosName);
			while(*after_last_vPos &&  *after_last_vPos != '\n')
				after_last_vPos++;

			// find unused temporary register
			DWORD maxTmpRegister = 31;
			bool registerFound = FindFreeRegister(maxTmpRegister, tmpRegisterName, first_vPos, after_last_vPos, shaderText);
			// TODO: need more careful in finding an unused const
			m_CurrentShaderData.add4VectorRegister = D3DPS30_NUMCONSTANTS - 1;

			if(registerFound)
			{
				m = modifiedShaderText;  
				// step to end of previous command 
				for(s = first_vPos; *s != '\n' && s > shaderText; s--) {}
				m += sprintf(m, "%.*s", s-shaderText, shaderText);

				// add tmpReg.xy, vPos.xy, -const.xy
				m += sprintf(m, "\nadd %s.%s, %s.%s, -c%i.%s", 
					tmpRegisterName, writeMask, vPosName, writeMask, 
					m_CurrentShaderData.add4VectorRegister, writeMask);
				
				// copy command to be modified
				m += sprintf(m, "%.*s", first_vPos - s, s);

				// modify pixel shader for wide rendering
				p = first_vPos;
				s = strstr(p, vPosName);
				while(s)
				{                                  
					m += sprintf(m, "%.*s%s", s-p, p, tmpRegisterName);
					p = s+strlen(vPosName);
					while(*p && *p != '\n')
						m += sprintf(m, "%c", *p++);
					s = strstr(p, vPosName);
				}

				sprintf(m, p);
				return true;
			}
			else
				DEBUG_MESSAGE(_T("%s: Error! Can't find free temporary register. Shader #%i\n"), _T(__FUNCTION__), m_CurrentShaderData.shaderGlobalIndex);
		}
		else
			DEBUG_MESSAGE(_T("%s: Error! Can't find vPos register. Shader #%i\n"), _T(__FUNCTION__), m_CurrentShaderData.shaderGlobalIndex);
	}
	else
		DEBUG_MESSAGE(_T("%s: Error! Can't find vPos register. Shader #%i\n"), _T(__FUNCTION__), m_CurrentShaderData.shaderGlobalIndex);
	
	(*outputText)->Release();
	*outputText = NULL;
	return false;
}

bool DX9PixelShaderConverter::AnalyzePSComplexity( char* pureShaderText )
{
	const char* nextTag	= pureShaderText;		// ps 2.0 - 3.0
	DWORD	numTexCommands = 0;
	DWORD	numALUCommands = 0;

	nextTag = skipSpaces(nextTag);				// skip the shader version definition
	if(strncmp(nextTag, "ps_", 3) == 0)
		while(*nextTag != '\0' && *nextTag != '\n')
			nextTag++;

	while(*nextTag)								// skip constant definitions
	{
		nextTag = skipSpaces(nextTag);
		if(strncmp(nextTag, "def", 3) == 0)
			while(*nextTag != '\0' && *nextTag != '\n')	nextTag++;
		else
			break;
	}

	while(*nextTag)								// skip other declarations
	{
		nextTag = skipSpaces(nextTag);
		if(strncmp(nextTag, "dcl", 3) == 0)
			while(*nextTag != '\0' && *nextTag != '\n') nextTag++;
		else
			break;
	}

	while(*nextTag)								// count all the shader commands
	{
		nextTag = skipSpaces(nextTag);
		if(strncmp(nextTag, "tex", 3) == 0)
			++numTexCommands;
		else
			++numALUCommands;
		while(*nextTag != '\0' && *nextTag != '\n')
			nextTag++;
	}

	const DWORD	MAX_PS_COMPLEXITY	= 99;
	const DWORD	TEX_COMPLEXITY		= 10;
	DWORD		complexity	= numTexCommands * TEX_COMPLEXITY + numALUCommands;

	return	(complexity > MAX_PS_COMPLEXITY);
}

bool DX9PixelShaderConverter::Analyze( DWORD shaderVersion, char* pureShaderText )
{
	m_CurrentShaderData.textures = 0;
	m_CurrentShaderData.vPosUsed = false;
	m_CurrentShaderData.shaderIsHeavy = false;
	m_CurrentShaderData.splitThisShaderDrawPass = false;

	const char* nextTag = pureShaderText;
	if (shaderVersion >= 0x200) // ps 2.0 - 3.0
	{
		m_CurrentShaderData.shaderIsHeavy = AnalyzePSComplexity( pureShaderText );

		while(*nextTag)
		{
			nextTag = skipSpaces(nextTag);
			if(strncmp(nextTag, "dcl", 3) == 0)
			{
				nextTag+=3;
				if (nextTag[0] != ' ')
				{
					while(*nextTag != ' ') 
						nextTag++;
					while(*nextTag == ' ') // skip spaces
						nextTag++;
					if (*nextTag == 's')
					{
						DWORD sampler = atoi(&nextTag[1]);
						m_CurrentShaderData.textures |= 1 << sampler;
					}
				}
				else
				{
					while(*nextTag == ' ') // skip spaces 
						nextTag++;
					if (strncmp(nextTag, "vPos", 4) == 0)
					{
						DWORD sampler = atoi(&nextTag[1]);
						m_CurrentShaderData.vPosUsed = true;
					}
				}
			}
			while(*nextTag != '\0' && *nextTag != '\n')
				nextTag++;
		}
	} else if (shaderVersion == 0x104) // ps 1.4
	{
		while(*nextTag)
		{
			nextTag = skipSpaces(nextTag);
			if(strncmp(nextTag, "tex", 3) == 0)
			{
				nextTag+=3;
				while(*nextTag != ' ') 
					nextTag++;
				while(*nextTag == ' ') // skip spaces
					nextTag++;
				while(*nextTag != '\0' && *nextTag != '\n')
				{
					if (*nextTag == 'r')
					{
						DWORD sampler = atoi(&nextTag[1]);
						m_CurrentShaderData.textures |= 1 << sampler;
					}
					nextTag++;
				}
			} 
			while(*nextTag != '\0' && *nextTag != '\n')
				nextTag++;
		}
	}
	else // ps 1.0 - 1.3
	{
		while(*nextTag)
		{
			nextTag = skipSpaces(nextTag);
			if(strncmp(nextTag, "tex", 3) == 0)
			{
				nextTag+=3;
				while(*nextTag != ' ') 
					nextTag++;
				while(*nextTag == ' ') // skip spaces
					nextTag++;
				if(*nextTag != '\0' && *nextTag != '\n')
				{
					if (*nextTag == 't')
					{
						DWORD sampler = atoi(&nextTag[1]);
						m_CurrentShaderData.textures |= 1 << sampler;
					}
				}
			} 
			while(*nextTag != '\0' && *nextTag != '\n')
				nextTag++;
		}
	}
	return true;
}

void ShaderCache::Init(TCHAR* ext)
{
	TCHAR szPath[MAX_PATH];
	iz3d::resources::GetAllUsersDirectory( szPath );
	PathAppend(szPath, _T("DX9ShaderDataCache") );
	CreateDirectory(szPath, NULL);
	int err = _stprintf_s( CacheFileName, _T("%s\\%s.%s"), szPath, gInfo.ProfileName, ext );
}

struct SHADER_CACHE_HEADER
{
	DWORD	Signature;					// The magic word like MAKEFOURCC( 'i','Z','3','D' );
	union {
		DWORD Version;
		struct {
			WORD	Major;
			WORD	Minor;
		} VersionPart;
	};
	WORD	RecordSize;					// Do we need that at all?!
	WORD	Reserved[3];
};

C_ASSERT(sizeof(SHADER_CACHE_HEADER) == 16);

FILE *  ShaderCache::OpenCacheFile(  )
{
	FILE *fp;
	_tfopen_s( &fp, CacheFileName, _T("rb") );
	if (fp == NULL)
		return NULL;
	SHADER_CACHE_HEADER header;
	fread( (void *)&header, sizeof( SHADER_CACHE_HEADER ), 1, fp );
	if( header.Signature != MAKEFOURCC( 'i','Z','3','D' ) ||
		header.Version != PRODUCT_CURRENT_VERSION ||
		header.RecordSize != sizeof( SHADER_CACHE_DATA ) )
	{
		fclose( fp );
		return NULL;
	}
	return fp;
}

bool ShaderCache::Load( )
{
	FILE *fp = OpenCacheFile();
	if (fp == NULL)
		return false;

	ShaderCacheData.clear();
	SHADER_CACHE_DATA	data;
	while( fread((void *)&data, sizeof( SHADER_CACHE_DATA ), 1, fp) == 1 )
	{
		ShaderCacheData.insert( SHADER_CACHE_DATA_PAIR( data.CRC32, data ) );
	}

	fclose( fp );
	return true;
}

FILE * ShaderCache::WriteHeader(  )
{
	FILE * fp;
	SHADER_CACHE_HEADER	header;
	_tfopen_s( &fp, CacheFileName, _T("wb") );	

	if (fp == NULL)
		return NULL;

	header.Signature  = MAKEFOURCC( 'i','Z','3','D' );
	header.Version    = PRODUCT_CURRENT_VERSION;
	header.RecordSize = sizeof( SHADER_CACHE_DATA );
	header.Reserved[0] = header.Reserved[1] = header.Reserved[2] = 0;

	fwrite( (void *)&header, sizeof(SHADER_CACHE_HEADER), 1, fp );
	return fp;
}

bool ShaderCache::Save( )
{
	FILE *fp = WriteHeader();

	if (fp == NULL)
		return false;


	SHADER_CACHE_DATA_MAP_ITERATOR i = ShaderCacheData.begin();

	SHADER_CACHE_DATA	data;
	while( i != ShaderCacheData.end() )
	{
		data = i->second;
		fwrite( (void *)&data, sizeof(SHADER_CACHE_DATA), 1, fp );
		++i;
	}

	fclose( fp );
	return true;
}

bool ShaderCache::Release(  )
{
	if( !ShaderCacheData.empty() )
	{
		ShaderCacheData.clear();
		return true;
	}
	return false;
}