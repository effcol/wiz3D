/* 
* Project : iZ3D Stereo Driver
* Copyright (C) iZ3D Inc. 2002 - 2010
*/

#pragma once

#include "PostprocessedOutput_dx9.h"

namespace DX9Output
{

class AnaglyphOutput :
	public PostprocessedOutput
{
public:
	AnaglyphOutput(DWORD mode, DWORD spanMode);
	~AnaglyphOutput(void);

	virtual HRESULT InitializeResources();
	virtual void	ReadConfigData(const char* configXml);
private:
	float m_CustomLeftMatrix[3][3];
	float m_CustomRightMatrix[3][3];
	void ReadMatrix( float matrix[3][3], TiXmlElement* pitem );
};

}
