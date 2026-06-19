/* 
* Project : iZ3D Stereo Driver
* Copyright (C) iZ3D Inc. 2002 - 2010
*/

#pragma once

#include "OutputMethod_dx9.h"
#include <vector>

namespace DX9Output
{

struct ResolutionGap
{
	int Width;
	int Height;
	int Gap;
};

class SideBySideOutput :
	public OutputMethod
{
public:
	SideBySideOutput(DWORD mode, DWORD spanMode);
	virtual ~SideBySideOutput(void);

	virtual UINT	GetOutputChainsNumber();
	virtual HRESULT	Output(CBaseSwapChain* pSwapChain);
	virtual HRESULT InitializeSCData(CBaseSwapChain* pSwapChain);
	virtual void	ReadConfigData(const char* configXml);
private:
	bool	m_bCrosseyed;
	std::vector<ResolutionGap>	m_Gap;
	int	m_DefaultGap;
};

}
