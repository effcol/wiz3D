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
#include "ShaderInstruction.h"
#include <algorithm>
#include <queue>
#include <stack>

using namespace std;

char* strrstr( const char *__restrict str, const char *__restrict strSearch )
{
	if (*strSearch == '\0')
		return((char *)str);

	if(*str != '\0')
	{
		const char *pstr = str + strlen(str);

		do {
			--pstr;
			const char *pstrc = pstr;
			const char *pstrSearchc = strSearch;
			while (*pstrc == *pstrSearchc)
			{
				++pstrSearchc;
				if (*pstrSearchc == '\0')
					return (char *)pstr;
				++pstrc;
			}
		} while(pstr != str);
	}
	return NULL;
}

template <size_t _Size>
inline bool isStrEqual(const char *str, const char (&subStr)[_Size], bool ignoreEnd = false)
{
	int r = strncmp(str, subStr, _Size - 1);
	if (r == 0)
	{
		if (str[_Size - 1] == '\0')
			return true;
		else
			return ignoreEnd;
	}
	else
		return false;
}


// for example:  version = 0x200
inline char* GetShaderStartPosition(const char* pShaderText)
{
	char* p;
	p = strrstr(pShaderText, "vs_");
	if (p) return p;
	p = strrstr(pShaderText, "vs.");
	if (p) return p;
	p = strrstr(pShaderText, "ps_");
	if (p) return p;
	p = strrstr(pShaderText, "ps.");
	if (p) return p;
	return (char*)pShaderText + strlen(pShaderText);
}

inline bool GetShaderoPosName(DWORD shaderVersion, const char* inputShader, char* oPosName)
{
	if(shaderVersion >= 0x300)
	{
		const char* dcl_pos = strrstr(inputShader, "dcl_position");
		if (dcl_pos)
		{
			dcl_pos += (int)strlen("dcl_position");

			while (*dcl_pos && *dcl_pos >= '0' && *dcl_pos <= '9')	// skip usage index (e.g. "dcl_position0")
				dcl_pos++;

			while (*dcl_pos && (*dcl_pos == ' ' || *dcl_pos == '\t' || *dcl_pos == '\n'))
				dcl_pos++;

			int len = 0;
			while (dcl_pos[len] &&
			       dcl_pos[len] != ' ' &&
			       dcl_pos[len] != '\t' &&
			       dcl_pos[len] != '\n')
				len++;

			if (len > 0)
			{
				memcpy(oPosName, dcl_pos, len);
				oPosName[len] = '\0';
			}
			else
			{
				oPosName[0] = '\0';
				return false;
			}
		}
		else
		{
			oPosName[0] = '\0';
			return false;
		}
	}
	else
		strcpy(oPosName, "oPos");
	
	return true;
}

inline bool isSpace(const char text)
{
	if(text && (text == ' ' || text == '\t' || text == '\n'))
		return true;
	else
		return false;
}

inline bool isText(const char text)
{
	if(text && !isSpace(text))
		return true;
	else
		return false;
}

const char *skipSpaces(const char *text)
{
	// skip all spaces while text not found
	while(*text && isSpace(*text))
		text++;
	return text;
}

//------------------------------------ class ShaderInstruction ------------------------------------
ShaderInstruction::ShaderInstruction()
{
	ZeroMemory(opField, sizeof(opField));
	nextTag = NULL;
	pZero = NULL;
	pOne = NULL;
}

//--- work for strings with one register only ---
inline char* ShaderInstruction::getSwizzle(char* reg)
{
	char* b = strrchr(reg, ']');
	char* p = strchr(b ? b+1 : reg, '.');
	return p;
}

bool ShaderInstruction::readCodeTag()
{
	char* p0 = command;
	if(*nextTag)
	{
		const char* s = skipSpaces(nextTag);
		if(s)
		{
			// read command
			while(isText(*s))		
				*p0++ = *s++;	
			*p0 = 0;
			// move to next tag
			nextTag = skipSpaces(s);
		}
		// instruction ended if text ended
		if(!*nextTag)
			return false;

		// instruction ended, if next line symbol found between words
		// count of arguments not taken into account in this realization
		while(s < nextTag)
			if(*s++ == '\n')
				return false;

		return true;
	}
	p0[0] = '\0';
	return false;
}

bool ShaderInstruction::readOperand(int codeIndex)
{
	char* p0 = opField[0][codeIndex];
	char* p1 = opField[1][codeIndex];
	const char* s = skipSpaces(nextTag);
	if(s)
	{
		//--- because of free order of modifiers ---
		for(int i=0; i< 2; i++)
		{
			// read '-' modifier
			if(*s == '-')
			{
				*p0++ = *s++;
				s = skipSpaces(s);
			}
			// read abs modifier
			if(!strncmp(s, "abs", 3))
			{
				strcpy(p0, "abs ");
				p0 += 4;
				s = skipSpaces(s+3);
			}
		}
		// read operand
		DWORD backetCounter = 0;
		while(isText(*s) || backetCounter)
		{
			if(*s == '[')	
				backetCounter++;
			if(*s == ']')	
				backetCounter--;
			if(*s == ',')	
				break;
			if(isText(*s))
				*p1++ = *p0++ = *s;
			s++;
		}
		*p1 = *p0 = 0;
		// move to next tag
		nextTag = skipSpaces(s);
		if(*nextTag == ',')
			nextTag = skipSpaces(nextTag+1);
	}
	// instruction ended if text ended
	if(!*nextTag)
		return false;

	// instruction ended, if next line symbol found between words
	// count of arguments not taken into account in this realization
	while(s < nextTag)
		if(*s++ == '\n')
			return false;

	return true;
}

const char* ShaderInstruction::readNextInstruction()
{
	const char* p = skipSpaces(nextTag);
	opCount = 0;
	if(readCodeTag())
	{
		for(int i=0; i< MAX_INSTRUCTION_PARAM; i++)
		{
			opCount++;
			if(!readOperand(i))
				break;
		}
	}
	return p;
}

void ShaderInstruction::GetVariableList(const ShaderRegister* expr, const ShaderRegister* &outList)
{
	//--- create list of variables ---
	ShaderRegister* reg = CreateRegister();
	BuildFullListWitoutDublicates(expr, reg);
	// TODO: free expr
	outList = reg;
}

void ShaderInstruction::GetVariableList(const ShaderRegister* expression1, const ShaderRegister* expression2, const ShaderRegister* &outList)
{
	//--- create list of variables ---
	ShaderRegister* reg = CreateRegister();
	BuildFullListWitoutDublicates(expression1, reg);
	BuildFullListWitoutDublicates(expression2, reg);
	// TODO: free expression1 && expression2
	outList = reg;
}

void ShaderInstruction::BuildFullListWitoutDublicates( const ShaderRegister* expr, ShaderRegister* &reg )
{
	// add all registers from outList to expr
	//--- remove duplicates ---

	treePos newPos;
	newPos.it = expr->elem.begin();
	newPos.p = expr;
	stack<treePos> allRegs;
	allRegs.push(newPos);
	do 
	{
		treePos* pos = &allRegs.top();
		if (pos->it == pos->p->elem.begin())
		{
			if (pos->p->op[0] == 'c' || pos->p->op[0] == 'r')
			{   
				//registersIterator it;
				//for ( it = reg->elem.begin( ) ; it != reg->elem.end( ) ; it++ )
				//{
				//    if (strcmp(pos.p->op, it->reg->op) == 0)
				//        break;
				//}

				//if(it == reg->elem.end())
				{
					//--- create list of variables ---
					RegisterWithOp r;
					r.comm = ',';
					r.reg = pos->p;
					reg->elem.push_back(r);
					reg->vRegCount += r.reg->vRegCount;
					reg->cRegCount += r.reg->cRegCount;
					reg->cRegCountWithInd += r.reg->cRegCountWithInd;
				}
			} 
		}

		if (pos->it != pos->p->elem.end())
		{
			allRegs.push(treePos());
			treePos* newPos = &allRegs.top();
			newPos->p = pos->it->reg;
			newPos->it = newPos->p->elem.begin();
			++pos->it;
		} 
		else
			allRegs.pop();
	} while(!allRegs.empty());
}

void ShaderInstruction::SetRegister(ProcessorRegister* pDest, ProcessorRegister* pSrc, int maxCounter, bool bReplace)
{
	if (bReplace)
		*pDest = *pSrc;
	else // fix problems with if
	{
		for (int i = 0; i < maxCounter; i++)
			if (pDest->comp[i] != pSrc->comp[i])
			{
				if (pDest->comp[i] != pOne)
				{
					pDest->comp[i] = CreateRegistersVectorAndExpand(CreateRegistersVector(pDest->comp[i], 0), ',', 
						CreateRegistersVector(pSrc->comp[i], 0), 0);
				}
				else
				{
					pDest->comp[i] = pSrc->comp[i];
				}
			}
	}
}

#define START_LOOP(i, maxCounter)												\
{																				\
	int mc = maxCounter;														\
	for(RegisterComponent i=ComponentX; i < maxCounter; i++)					\
		if(isComponentWritable(i, getUnmodifiedOperand(DEST)))

#define END_LOOP()																\
	SetRegister(pDest, &tempOutputRegister, mc, bReplace);						\
	continue;																	\
}

void ShaderInstruction::execute(DWORD shaderVersion, const char* shaderText, char* outputRegisterName, ProcessorRegister& outputRegister, DWORD &textures)
{
	InitRegisters();
	const char* shaderEnd = shaderText + strlen(shaderText);
	nextTag = shaderText;
	int lineIndex = 0;
	int outputRegisterLength = (int)strlen(outputRegisterName);
	InitProcessorRegistor(outputRegister);
	while(*nextTag)
	{
		if(readNextInstruction() >= shaderEnd)
			break;
		const char * unmodifiedCommand = getUnmodifiedCommand();
		// skip declaration
		if(isStrEqual(unmodifiedCommand, "vs", true))
			continue;
		else if(isStrEqual(unmodifiedCommand, "def"))
			continue;
		else if(isStrEqual(unmodifiedCommand, "dcl_", true))
		{
			if (getUnmodifiedOperand(DEST)[0] == 's')
			{
				DWORD sampler = atoi(&getUnmodifiedOperand(DEST)[1]);
				textures |= 1 << sampler;
			}
			continue;
		}
		lineIndex++;
		if(opCount == 0)
			continue;

		//--- check for destination register ---
		ProcessorRegister* pDest = NULL;
		bool bReplace = true;
		if(*getUnmodifiedOperand(DEST) == 'r')
		{
			int ind = atoi(getUnmodifiedOperand(DEST)+1);
			Registers_t::iterator it = TmpRegisters.find(ind);
			if (it == TmpRegisters.end())
			{
				ProcessorRegister pr;
				InitProcessorRegistor(pr);
				it = TmpRegisters.insert(it,
					Registers_t::value_type(ind, pr));
			}
			pDest = &((*it).second);
		}
		else if(!strncmp(getUnmodifiedOperand(DEST), outputRegisterName, outputRegisterLength))
		{
			bReplace = false;
			pDest = &outputRegister;
		}
		else
			continue;

		ProcessorRegister tempOutputRegister = *pDest;
		ProcessorRegister* pOutput = &tempOutputRegister;

		const ShaderRegister* op[8];		
		switch (unmodifiedCommand[0])
		{
		case 'a':
			if(isStrEqual(unmodifiedCommand, "abs"))
			START_LOOP(i, 4) {
				getComponentValue(i, SRC0, pOutput->comp[i]);
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "add"))
			START_LOOP(i, 4) {
				getComponentValue(i, SRC0, op[0]);
				getComponentValue(i, SRC1, op[1]);
				pOutput->comp[i] = CreateRegistersVectorAndExpand(op[0], '+', op[1], 0);
			} END_LOOP()
			break;
		case 'c':		
			if(isStrEqual(unmodifiedCommand, "crs"))
			START_LOOP(i, 3) {
				getComponentValue((RegisterComponent)((i+1)%3), SRC0, op[0]);
				getComponentValue((RegisterComponent)((i+2)%3), SRC1, op[1]);
				getComponentValue((RegisterComponent)((i+2)%3), SRC0, op[2]);
				getComponentValue((RegisterComponent)((i+1)%3), SRC1, op[3]);
				pOutput->comp[i] = CreateRegistersVector(op[0], '*', op[1], 
					'-', op[2], '*', op[3], 0);
			} END_LOOP()
			break;
		case 'd':
			if(isStrEqual(unmodifiedCommand, "dp3"))
			START_LOOP(i, 4) {
				getComponentValue(ComponentX, SRC0, op[0]);
				getComponentValue(ComponentX, SRC1, op[1]);
				getComponentValue(ComponentY, SRC0, op[2]);
				getComponentValue(ComponentY, SRC1, op[3]);
				getComponentValue(ComponentZ, SRC0, op[4]);
				getComponentValue(ComponentZ, SRC1, op[5]);
				pOutput->comp[i] = CreateRegistersVector(op[0], '*', op[1], 
					'+', op[2], '*', op[3], 
					'+', op[4], '*', op[5], 0);
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "dp4"))
			START_LOOP(i, 4) {
				getComponentValue(ComponentX, SRC0, op[0]);
				getComponentValue(ComponentX, SRC1, op[1]);
				getComponentValue(ComponentY, SRC0, op[2]);
				getComponentValue(ComponentY, SRC1, op[3]);
				getComponentValue(ComponentZ, SRC0, op[4]);
				getComponentValue(ComponentZ, SRC1, op[5]);
				getComponentValue(ComponentW, SRC0, op[6]);
				getComponentValue(ComponentW, SRC1, op[7]);
				pOutput->comp[i] = CreateRegistersVector(op[0], '*', op[1], 
					'+', op[2], '*', op[3], 
					'+', op[4], '*', op[5],
					'+', op[6], '*', op[7], 0);
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "dst"))
			{
				if(isComponentWritable(ComponentX, getUnmodifiedOperand(DEST)))	
					pOutput->comp[ComponentX] = pOne;
				if(isComponentWritable(ComponentY, getUnmodifiedOperand(DEST)))	
				{
					getComponentValue(ComponentY, SRC0, op[0]);
					getComponentValue(ComponentY, SRC1, op[1]);
					GetVariableList(op[0], op[1], pOutput->comp[ComponentY]);
					//TMPBUFFER_PRINT2("(%s)*(%s)");
					//tempOutputRegister[ComponentY] = tmpBuffer.c_str();
				}
				if(isComponentWritable(ComponentZ, getUnmodifiedOperand(DEST)))	
					getComponentValue(ComponentZ, SRC0, pOutput->comp[ComponentZ]); 
				if(isComponentWritable(ComponentW, getUnmodifiedOperand(DEST)))	
					getComponentValue(ComponentW, SRC1, pOutput->comp[ComponentW]);
				*pDest = tempOutputRegister;
				continue;
			}
			break;
		case 'e':		
			if(isStrEqual(unmodifiedCommand, "exp"))
			START_LOOP(i, 4) {
				getComponentValue(ComponentX, SRC0, op[0]);
				GetVariableList(op[0], pOutput->comp[i]);
				//TMPBUFFER_PRINT1("pow(2,%s)");			
				//tempOutputRegister[i] = tmpBuffer.c_str());
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "expp"))
			{
				if(shaderVersion < 0x200)	
				{
					getComponentValue(ComponentX, SRC0, op[0]);
					if(isComponentWritable(ComponentX, getUnmodifiedOperand(DEST)))	
					{
						GetVariableList(op[0], pOutput->comp[ComponentX]);
						//TMPBUFFER_PRINT1("pow(2,floor(%s))");
						//tempOutputRegister[ComponentX] = tmpBuffer.c_str();
					}
					if(isComponentWritable(ComponentY, getUnmodifiedOperand(DEST)))	
					{
						GetVariableList(op[0], pOutput->comp[ComponentY]);
						//TMPBUFFER_PRINT1("frc(%s)");
						//tempOutputRegister[ComponentY] = tmpBuffer.c_str();
					}
					if(isComponentWritable(ComponentZ, getUnmodifiedOperand(DEST)))	
					{
						GetVariableList(op[0], pOutput->comp[ComponentZ]);
						//TMPBUFFER_PRINT1("pow(2,%s)");
						//tempOutputRegister[ComponentZ] = tmpBuffer.c_str();
					}
					if(isComponentWritable(ComponentW, getUnmodifiedOperand(DEST)))	
						pOutput->comp[ComponentW] = CreateRegister("1");
					*pDest = tempOutputRegister;
				}
				else
				{
					START_LOOP(i, 4) {
						getComponentValue(ComponentX, SRC0, op[0]);
						GetVariableList(op[0], pOutput->comp[i]);
						//TMPBUFFER_PRINT1("pow(2,%s)");
						//tempOutputRegister[i] = tmpBuffer.c_str());
					} END_LOOP()
				}
				continue;
			}
			break;
		case 'f':
			if(isStrEqual(unmodifiedCommand, "frc"))
			START_LOOP(i, 4) {
				getComponentValue(i, SRC0, op[0]);
				GetVariableList(op[0], pOutput->comp[i]);
				//TMPBUFFER_PRINT1("frc(%s)");
				//tempOutputRegister[i] = tmpBuffer.c_str());
			} END_LOOP()
			break;
		case 'l':
			if(isStrEqual(unmodifiedCommand, "lit"))
			START_LOOP(i, 4) {
				pOutput->comp[i] = (i == ComponentY || i == ComponentZ) ? pZero : pOne;
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "log"))
			START_LOOP(i, 4) {
				getComponentValue(ComponentX, SRC0, op[0]);
				GetVariableList(op[0], pOutput->comp[i]);
				//TMPBUFFER_PRINT1("abs(log2(%s))");
				//tempOutputRegister[i] = tmpBuffer.c_str());
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "logp"))
			START_LOOP(i, 4) {
				getComponentValue(ComponentX, SRC0, op[0]);
				GetVariableList(op[0], pOutput->comp[i]);
				//TMPBUFFER_PRINT1("abs(logp2(%s))");
				//tempOutputRegister[i] = tmpBuffer.c_str());
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "lrp"))
			START_LOOP(i, 4) {
				getComponentValue(i, SRC0, op[0]);
				getComponentValue(i, SRC1, op[1]);
				getComponentValue(i, SRC2, op[2]);
				// simplified + hack for RE5
				pOutput->comp[i] = CreateRegistersVectorAndExpand(CreateRegistersVector(op[2], 0), ',', 
					CreateRegistersVector(op[1], 0), ',', CreateRegistersVector(op[0], 0), 0);
			} END_LOOP()	
			break;
		case 'm':
			if(isStrEqual(unmodifiedCommand, "m3x2"))
			START_LOOP(i, 2) {
				getComponentValue(ComponentX, SRC0, op[0]);
				getComponentValue(ComponentX, SRC1, op[1]);
				getComponentValue(ComponentY, SRC0, op[2]);
				getComponentValue(ComponentX, SRC1, op[3]);
				getComponentValue(ComponentZ, SRC0, op[4]);
				getComponentValue(ComponentZ, SRC1, op[5]);
				pOutput->comp[i] = CreateRegistersVector(op[0], '*', op[1], 
					'+', op[2], '*', op[3], 
					'+', op[4], '*', op[5], 0);
				IncrementConstantRegisterIndex();
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "m3x3"))
			START_LOOP(i, 3) {
				getComponentValue(ComponentX, SRC0, op[0]);
				getComponentValue(ComponentX, SRC1, op[1]);
				getComponentValue(ComponentY, SRC0, op[2]);
				getComponentValue(ComponentY, SRC1, op[3]);
				getComponentValue(ComponentZ, SRC0, op[4]);
				getComponentValue(ComponentZ, SRC1, op[5]);
				pOutput->comp[i] = CreateRegistersVector(op[0], '*', op[1], 
					'+', op[2], '*', op[3], 
					'+', op[4], '*', op[5], 0);
				IncrementConstantRegisterIndex();
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "m3x4"))
			START_LOOP(i, 4) {
				getComponentValue(ComponentX, SRC0, op[0]);
				getComponentValue(ComponentX, SRC1, op[1]);
				getComponentValue(ComponentY, SRC0, op[2]);
				getComponentValue(ComponentY, SRC1, op[3]);
				getComponentValue(ComponentZ, SRC0, op[4]);
				getComponentValue(ComponentZ, SRC1, op[5]);
				pOutput->comp[i] = CreateRegistersVector(op[0], '*', op[1], 
					'+', op[2], '*', op[3], 
					'+', op[4], '*', op[5], 0);
				IncrementConstantRegisterIndex();
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "m4x3"))
			START_LOOP(i, 3) {
				getComponentValue(ComponentX, SRC0, op[0]);
				getComponentValue(ComponentX, SRC1, op[1]);
				getComponentValue(ComponentY, SRC0, op[2]);
				getComponentValue(ComponentY, SRC1, op[3]);
				getComponentValue(ComponentZ, SRC0, op[4]);
				getComponentValue(ComponentZ, SRC1, op[5]);
				getComponentValue(ComponentW, SRC0, op[6]);
				getComponentValue(ComponentW, SRC1, op[7]);
				pOutput->comp[i] = CreateRegistersVector(op[0], '*', op[1], 
					'+', op[2], '*', op[3], 
					'+', op[4], '*', op[5],
					'+', op[6], '*', op[7], 0);
				IncrementConstantRegisterIndex();
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "m4x4"))
			START_LOOP(i, 4) {
				getComponentValue(ComponentX, SRC0, op[0]);
				getComponentValue(ComponentX, SRC1, op[1]);
				getComponentValue(ComponentY, SRC0, op[2]);
				getComponentValue(ComponentY, SRC1, op[3]);
				getComponentValue(ComponentZ, SRC0, op[4]);
				getComponentValue(ComponentZ, SRC1, op[5]);
				getComponentValue(ComponentW, SRC0, op[6]);
				getComponentValue(ComponentW, SRC1, op[7]);
				pOutput->comp[i] = CreateRegistersVector(op[0], '*', op[1], 
					'+', op[2], '*', op[3], 
					'+', op[4], '*', op[5],
					'+', op[6], '*', op[7], 0);
				IncrementConstantRegisterIndex();
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "mad"))
			START_LOOP(i, 4) {
				getComponentValue(i, SRC0, op[0]);
				getComponentValue(i, SRC1, op[1]);
				getComponentValue(i, SRC2, op[2]);
				pOutput->comp[i] = CreateRegistersVectorAndExpand(
					CreateRegistersVector(op[0], 0), '*', CreateRegistersVector(op[1], 0), 
					'+', op[2], 0);
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "max"))
			START_LOOP(i, 4) {
				getComponentValue(i, SRC0, op[0]);
				getComponentValue(i, SRC1, op[1]);
				GetVariableList(op[0], op[1], pOutput->comp[i]);
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "min"))
			START_LOOP(i, 4) {
				getComponentValue(i, SRC0, op[0]);
				getComponentValue(i, SRC1, op[1]);
				GetVariableList(op[0], op[1], pOutput->comp[i]);
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "mov"))
			START_LOOP(i, 4) {
				getComponentValue(i, SRC0, pOutput->comp[i]);
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "mul"))
			START_LOOP(i, 4) {
				getComponentValue(i, SRC0, op[0]);
				getComponentValue(i, SRC1, op[1]);
				pOutput->comp[i] = CreateRegistersVector(op[0], '*', op[1], 0);
			} END_LOOP()
			break;
		case 'n':
			if(isStrEqual(unmodifiedCommand, "nrm"))
			START_LOOP(i, 4) {
				getComponentValue(i, SRC0, op[0]);
		pOutput->comp[i] = op[0];
		//GetVariableList(op[0], pOutput->comp[i]);
				//TMPBUFFER_PRINT1("nrm(%s)");
				//tempOutputRegister[i] = tmpBuffer.c_str());
			} END_LOOP()
			break;
		case 'p':
			if(isStrEqual(unmodifiedCommand, "pow"))
			START_LOOP(i, 4) {
				getComponentValue(ComponentX, SRC0, op[0]);
				getComponentValue(ComponentX, SRC1, op[1]);
				GetVariableList(op[0], op[1], pOutput->comp[i]);
				//TMPBUFFER_PRINT2("pow(abs(%s), %s)");
				//tempOutputRegister[i] = tmpBuffer.c_str());
			} END_LOOP()
			break;
		case 'r':
			if(isStrEqual(unmodifiedCommand, "rcp"))
			START_LOOP(i, 4) {
				getComponentValue(ComponentX, SRC0, op[0]);
				GetVariableList(op[0], pOutput->comp[i]);
				//TMPBUFFER_PRINT1("1/(%s)");
				//tempOutputRegister[i] = tmpBuffer.c_str());
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "rsq"))
			START_LOOP(i, 4) {
				// requires a long time, but does not do any good
				//getComponentValue(ComponentX, SRC0, op[0]);
				//GetVariableList(op[0], pOutput->comp[i]);
				pOutput->comp[i] = CreateRegister("1");
				//TMPBUFFER_PRINT1("1/sqrt(%s)");
				//tempOutputRegister[i] = tmpBuffer.c_str());
			} END_LOOP()
			break;	
		case 's':
			if(isStrEqual(unmodifiedCommand, "sge"))
			START_LOOP(i, 4) {
				pOutput->comp[i] = CreateRegister("X"); // 0 or 1
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "sgn"))
			START_LOOP(i, 4) {
				pOutput->comp[i] = CreateRegister("0");
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "sincos"))
			START_LOOP(i, 2) {
				getComponentValue(i, SRC0, op[0]);
				GetVariableList(op[0], pOutput->comp[i]);
				/*
				if(i == ComponentX)
					TMPBUFFER_PRINT1("cos(%s)");
				if(i == ComponentY)
					TMPBUFFER_PRINT1("sin(%s)");
				tempOutputRegister[i] = tmpBuffer.c_str());
				*/
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "slt"))
			START_LOOP(i, 4) {
				pOutput->comp[i] = CreateRegister("X"); // 0 or 1
			} END_LOOP()
			else if(isStrEqual(unmodifiedCommand, "sub"))
			START_LOOP(i, 4) {
				getComponentValue(i, SRC0, op[0]);
				getComponentValue(i, SRC1, op[1]);
				pOutput->comp[i] = CreateRegistersVectorAndExpand(op[0], '-', op[1], 0);
			} END_LOOP()
			break;	
		case 't':
			if(isStrEqual(unmodifiedCommand, "texldl"))
			START_LOOP(i, 4) {
				pOutput->comp[i] = pZero;
				//tempOutputRegister[i] = getComponentValue(i, SRC1, op[0]));
			} END_LOOP()
			break;	
		}
	}
	TmpRegisters.clear();
}

inline bool ShaderInstruction::isComponentWritable(RegisterComponent componentIndex, const char* registerName)
{
	const char* p = strchr(registerName, '.');
	//--- all components available ---
	if(!p)
		return true;

	const char componentNames[4]      = { 'x', 'y', 'z', 'w' };
	const char componentNamesAlias[4] = { 'r', 'r', 'b', 'a' };
	//--- check for specified component ---
	if(strchr(p+1, componentNames[componentIndex]) || strchr(p+1, componentNamesAlias[componentIndex]))
		return true;
	else
		return false;
}

void ShaderInstruction::getComponentValue(RegisterComponent componentIndex, CommandParameters commandIndex, const ShaderRegister* &outValue)
{
	const char componentNames[4] = { 'x', 'y', 'z', 'w' };

	int	 swizzleIndex = componentIndex;
	char swizzleChar = componentNames[componentIndex];
	char* unmodifiedOperand = getUnmodifiedOperand(commandIndex);
	char* p = getSwizzle(unmodifiedOperand);
	//--- translate swizzle ---
	if(p)
	{
		int  swizzleLenght = (int)strlen(p+1);
		swizzleChar = componentIndex < swizzleLenght ? *(p+1+componentIndex) : *(p+swizzleLenght);
		for(int i=ComponentX; i<= ComponentW; i++)
			if(componentNames[i] == swizzleChar)
			{
				swizzleIndex = i;
				break;
			}
	}

	if(unmodifiedOperand[0] == 'r')
	{
		DWORD regNum = atoi(unmodifiedOperand+1);
		outValue = TmpRegisters[regNum].comp[swizzleIndex];
	}
	else
	{
		char operand[MAX_OPERAND_LENGTH];
		strcpy(operand, unmodifiedOperand);
		char* s = getSwizzle(operand);
		sprintf(s ? s : operand+strlen(operand), ".%c", swizzleChar);
		outValue = CreateRegister(operand);
	}	
}

void ShaderInstruction::IncrementConstantRegisterIndex()
{
	for(CommandParameters i=SRC0; i< MAX_INSTRUCTION_PARAM; i++)
	{
		char* op = getUnmodifiedOperand(i);
		if('c' == op[0])
		{
			char operand[MAX_OPERAND_LENGTH];
			strcpy(operand, op);
			// increase index
			int ci = atoi(op+1);
			sprintf(op, "c%i", ci+1);
			// copy remaining part of register
			char* p = operand+1;
			while(isdigit((int)*p))
				p++;
			if(*p)
				strcat(op, p);
			break;
		}
	}
}

void ShaderInstruction::InitProcessorRegistor(ProcessorRegister &pr)
{
	pr.comp[0] = pOne;
	pr.comp[1] = pOne;
	pr.comp[2] = pOne;
	pr.comp[3] = pOne;
}

ShaderRegister* ShaderInstruction::CreateRegister() 
{
	ShaderRegister c;
	c.cRegCountWithInd = 0;
	c.cRegCount = 0;
	c.vRegCount = 0;
	c.op[0] = '1';
	c.op[1] = '\0';
	registerManager.push_back(c);
	return &registerManager.back();
}

const ShaderRegister* ShaderInstruction::CreateRegister(char* srcOp ) 
{
	ShaderRegister c;
	c.cRegCountWithInd = 0;
	c.cRegCount = 0;
	c.vRegCount = 0; 
	strcpy(c.op, srcOp);
	c.SetCount();
	registerManager.push_back(c);
	return &registerManager.back();
}

const ShaderRegister* ShaderInstruction::CreateRegistersVector(const ShaderRegister* first, ... ) 
{
	ShaderRegister c;
	c.cRegCountWithInd = 0;
	c.cRegCount = 0;
	c.vRegCount = 0; 
	c.op[0] = '\0';

	va_list marker;
	va_start( marker, first );
	const ShaderRegister* cur = first;
	char curComm = '\0';
	int i = 0;
	while(true)
	{
		RegisterWithOp r;
		r.comm = curComm;
		r.reg = cur;
		c.cRegCountWithInd += cur->cRegCountWithInd;
		c.cRegCount += cur->cRegCount;
		c.vRegCount += cur->vRegCount;
		c.elem.push_back(r);
		i++;
		curComm = va_arg( marker, char);
		if (curComm == 0)
			break;
		i++;
		cur = va_arg( marker, const ShaderRegister* );
	}
	registerManager.push_back(c);
	return &registerManager.back();
}

const ShaderRegister* ShaderInstruction::CreateRegistersVectorAndExpand(const ShaderRegister* first, ... ) 
{
	ShaderRegister c;
	c.cRegCountWithInd = 0;
	c.cRegCount = 0;
	c.vRegCount = 0; 
	c.op[0] = '\0';

	va_list marker;
	va_start( marker, first );
	const ShaderRegister* cur = first;
	char curComm = '\0';
	int i = 0;
	while(true)
	{
		if (!cur->elem.empty())
		{
			for (registersIterator it = cur->elem.begin(); it != cur->elem.end(); ++it)
			{
				c.elem.push_back(*it);
				RegisterWithOp& r = c.elem.back();
				if (it == cur->elem.begin())
					r.comm = curComm;
				c.cRegCountWithInd += r.reg->cRegCountWithInd;
				c.cRegCount += r.reg->cRegCount;
				c.vRegCount += r.reg->vRegCount;
				i++;
			}
			// TODO: free cur after merge
		}
		else
		{
			RegisterWithOp r;
			r.comm = curComm;
			r.reg = cur;
			c.cRegCountWithInd += cur->cRegCountWithInd;
			c.cRegCount += cur->cRegCount;
			c.vRegCount += cur->vRegCount;
			c.elem.push_back(r);
			i++;
		}
		curComm = va_arg( marker, char);
		if (curComm == 0)
			break;
		cur = va_arg( marker, const ShaderRegister* );
	}
	registerManager.push_back(c);
	return &registerManager.back();
}

void ShaderInstruction::InitRegisters()
{
	registerManager.clear();
	TmpRegisters.clear();

	pZero = CreateRegister("0");
	pOne = CreateRegister("1");
}
