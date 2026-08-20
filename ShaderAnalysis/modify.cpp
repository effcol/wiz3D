#include "StdAfx.h"
#include <d3d10.h>
#include "..\CommonUtils\ShaderProcessor.h"
#include "modify.h"

using namespace shader_analyzer;
using namespace iz3d::shader;

bool FindFreeRegisters(const TShaderList& originalShader, unsigned* reg, unsigned regCount, bool& needTmpDCLModification, unsigned& dclTempOpCodeIndex)
{
	dclTempOpCodeIndex = 0xFFFFFFFF;
	needTmpDCLModification = true;
	//--- now use fastest and simplest methods - add another temporary register ---
	for(unsigned i = 0; i< originalShader.size(); i++)
	{
		if(originalShader[i].getOperation()->getType() == D3D10_SB_OPCODE_DCL_TEMPS)
		{
			std::vector<UINT> Data;
			originalShader[i].GetBlob( Data );
			unsigned tempIndex = Data[1];
			if(tempIndex > D3D10_COMMONSHADER_TEMP_REGISTER_COUNT - regCount)
				return false;
			dclTempOpCodeIndex = i;
			for(unsigned j=0; j < regCount; j++)
				reg[j] = tempIndex++;
			return true;
		}
	}
	//--- shader don't use temporary registers ---
	for(unsigned j=0; j < regCount; j++)
		reg[j] = j;
	return true;
}

void push_backCommand(TShaderList& shader, char* command)
{
	ShaderProcessor shaderProcessor;
	ShaderProcessor::TBinData instructionData;
	shaderProcessor.CompileInstruction(command, instructionData);
	shader.push_back(ShaderInstruction((unsigned*)&instructionData[0], instructionData.size() * sizeof(DWORD)));
}

//--- return false if modification failed --- 
bool ModifyShader( const TShaderList& originalShader, unsigned outputRegisterIndex, bool AddZNearCheck, TShaderList& modifiedShader, ModifiedShaderData& MData)
{
	//--- find unused constant buffer slot ---
	int j;
	unsigned i = 0;
	SHORT usedCBMask = 0;
	unsigned freeCBIndex;
	for(i = 0; i< originalShader.size(); i++)
	{
		if(originalShader[i].getOperation()->getType() == D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER)
		{
			shader_analyzer::ShaderOperand op( const_cast<UINT*>( &originalShader[i].GetData()[1] ) );
			usedCBMask |= 1 << op.getIndex(0);
		}
	}
	for(j = D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; j >= 0; j--)
		if((usedCBMask & (1 << j)) == 0)
		{
			freeCBIndex = j;
			break;
		}
	if(j < 0)
	{
		//--- no free CB slot available ---
		return false;
	}

	//--- find unused temporary register ---
	unsigned dclTempOpCodeIndex;
	bool	 needTmpDCLModification;
	unsigned tmpReg[2];
	bool bFound = FindFreeRegisters(originalShader, tmpReg, _countof(tmpReg), needTmpDCLModification, dclTempOpCodeIndex);
	if(!bFound)
		return false;

	//--- set input registers ---
	MData.CBIndex = freeCBIndex;
	MData.dp4VectorRegister = 0;
	MData.ZNearRegister = 1;

	// dcl_constantbuffer cb freeCBIndex[2]
	ShaderInstruction cbDeclInst;
	{
		unsigned dataCB[] = {	ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER) |
								ENCODE_D3D10_SB_D3D10_SB_CONSTANT_BUFFER_ACCESS_PATTERN(D3D10_SB_CONSTANT_BUFFER_IMMEDIATE_INDEXED) |
								ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(4),
							
								ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) |
								ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) |
								D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE |
								ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER) |
								ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_2D) |
								ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(2, D3D10_SB_OPERAND_INDEX_IMMEDIATE32),

								freeCBIndex, 2 };

		std::istrstream shaderStream((char*)dataCB, sizeof(dataCB));
		shaderStream >> cbDeclInst;
	}

	//--- dcl_temp command
	ShaderInstruction tmpDeclInst;
	{
		unsigned dataTmp[] = {	ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_TEMPS) | 
								ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(2), 
								max(tmpReg[0], tmpReg[1]) + 1 };
		std::istrstream shaderStream((char*)dataTmp, sizeof(dataTmp));
		shaderStream >> tmpDeclInst;
	}

	//--- make modified commands ---
	//--- in current version of compiler ALL components MUST be specified  --- 
	TShaderList modificationCommands;
	{
		char s[256];
		if (!AddZNearCheck)
		{
			switch(gInfo.VertexShaderModificationMethod)
			{
			case 1: // WoW Hybrid 2D Objects
				// mad tmpReg1.x, tmpReg0.w, const_dp4.z, tmpReg0.x
				sprintf(s, "mad r%i.x, r%i.w, cb%i[%i].z, r%i.x", tmpReg[1], tmpReg[0], freeCBIndex, MData.dp4VectorRegister, tmpReg[0]);
				push_backCommand(modificationCommands, s);
				// add tmpReg0.x, tmpReg1.x, const_dp4.w
				sprintf(s, "add r%i.x, r%i.x, cb%i[%i].w", tmpReg[0], tmpReg[1], freeCBIndex, MData.dp4VectorRegister);
				push_backCommand(modificationCommands, s);
				break;
			case 2: // NVidia-like method
				// add tmpReg1.x, tmpReg0.w, -const_dp4.y
				sprintf(s, "add r%i.x, r%i.w, -cb%i[%i].y", tmpReg[1], tmpReg[0], freeCBIndex, MData.dp4VectorRegister);
				push_backCommand(modificationCommands, s);
				// mad tmpReg0.x, tmpReg1.x, const_dp4.x, tmpReg0.x
				sprintf(s, "mad r%i.x, r%i.x, cb%i[%i].x, r%i.x", tmpReg[0], tmpReg[1], freeCBIndex, MData.dp4VectorRegister, tmpReg[0]);
				push_backCommand(modificationCommands, s);
				break;
			default:
				// dp4 tmpReg0.x, tmpReg0, const_dp4
				sprintf(s, "dp4 r%i.x, r%i.xyzw, cb%i[%i].xyzw", tmpReg[0], tmpReg[0], freeCBIndex, MData.dp4VectorRegister);
				push_backCommand(modificationCommands, s);
				break;
			}
		}
		else
		{
			switch(gInfo.VertexShaderModificationMethod)
			{
			case 1:
				// mad tmpReg1.x, tmpReg0.w, const_dp4.z, tmpReg0.x
				sprintf(s, "mad r%i.x, r%i.w, cb%i[%i].z, r%i.x", tmpReg[1], tmpReg[0], freeCBIndex, MData.dp4VectorRegister, tmpReg[0]);
				push_backCommand(modificationCommands, s);
				// add tmpReg1.x, tmpReg1.x, const_dp4.w
				sprintf(s, "add r%i.x, r%i.x, cb%i[%i].w", tmpReg[1], tmpReg[1], freeCBIndex, MData.dp4VectorRegister);
				push_backCommand(modificationCommands, s);
				// add tmpReg1.x, -tmpReg0.x, tmpReg1.x
				sprintf(s, "add r%i.x, -r%i.x, r%i.x", tmpReg[1], tmpReg[0], tmpReg[1]);
				push_backCommand(modificationCommands, s);
				// lt tmpReg1.y, const_ZNear.w, tmpReg0.w
				sprintf(s, "lt r%i.y, cb%i[%i].w, r%i.w", tmpReg[1], freeCBIndex, MData.ZNearRegister, tmpReg[0]);
				push_backCommand(modificationCommands, s);
				// mad tmpReg0.x, tmpReg1.y, tmpReg1.x, tmpReg0.x
				sprintf(s, "mad r%i.x, r%i.y, r%i.x, r%i.x", tmpReg[0], tmpReg[1], tmpReg[1], tmpReg[0]);
				push_backCommand(modificationCommands, s);
				break;
			case 2: // NVidia-like Ex method
				// lt tmpReg1.y, const_dp4.w, tmpReg0.w
				sprintf(s, "lt r%i.y, cb%i[%i].w, r%i.w", tmpReg[1], freeCBIndex, MData.dp4VectorRegister, tmpReg[0]);
				push_backCommand(modificationCommands, s);
				// mul tmpReg1.y, tmpReg1.y, const_dp4.x
				sprintf(s, "mul r%i.y, r%i.y, cb%i[%i].x", tmpReg[1], tmpReg[1], freeCBIndex, MData.dp4VectorRegister);
				push_backCommand(modificationCommands, s);
				// add tmpReg1.x, tmpReg0.w, -const_dp4.y
				sprintf(s, "add r%i.x, r%i.w, -cb%i[%i].y", tmpReg[1], tmpReg[0], freeCBIndex, MData.dp4VectorRegister);
				push_backCommand(modificationCommands, s);
				// mad tmpReg0.x, tmpReg1.x, tmpReg1.y, tmpReg0.x
				sprintf(s, "mad r%i.x, r%i.x, r%i.y, r%i.x", tmpReg[0], tmpReg[1], tmpReg[1], tmpReg[0]);
				push_backCommand(modificationCommands, s);
			default:
				// dp4 tmpReg1.x, tmpReg0, const_dp4
				sprintf(s, "dp4 r%i.x, r%i, cb%i[%i]", tmpReg[1], tmpReg[0], freeCBIndex, MData.dp4VectorRegister);
				push_backCommand(modificationCommands, s);
				// add tmpReg1.x, -tmpReg0.x, tmpReg1.x
				sprintf(s, "add r%i.x, -r%i.x, r%i.x", tmpReg[1], tmpReg[0], tmpReg[1]);
				push_backCommand(modificationCommands, s);
				// lt tmpReg1.y, const_ZNear.w, tmpReg0.w
				sprintf(s, "lt r%i.y, cb%i[%i].w, r%i.w", tmpReg[1], freeCBIndex, MData.ZNearRegister, tmpReg[0]);
				push_backCommand(modificationCommands, s);
				// mad tmpReg0.x, tmpReg1.y, tmpReg1.x, tmpReg0.x
				sprintf(s, "mad r%i.x, r%i.y, r%i.x, r%i.x", tmpReg[0], tmpReg[1], tmpReg[1], tmpReg[0]);
				push_backCommand(modificationCommands, s);
				break;
			}
		}
		
		sprintf(s, "mov o%i.xyzw, r%i.xyzw", outputRegisterIndex, tmpReg[0]);
		push_backCommand(modificationCommands, s);
	}
	
	//--- copy dcl_ commands ---
	bool modifiedTmpDCL = false;
	bool addedCbDecl = false;
	for(i=0; originalShader[i].getOperation()->isCustomData() || originalShader[i].getOperation()->isDeclaration(); i++)
	{
		const ShaderInstruction& si = originalShader[i];
		DBG_UNREFERENCED_LOCAL_VARIABLE(si);
		if (needTmpDCLModification && i == dclTempOpCodeIndex)
		{
			//--- replace decl_tmp command, because we use additional registers
			//--- trying to preserve original shader command flow
			assert(!modifiedTmpDCL);
			modifiedShader.push_back(tmpDeclInst);
			modifiedTmpDCL = true;
			continue;
		}
		else if (!addedCbDecl && originalShader[i].getOperation()->getType() == D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER) 
		{
			//--- add command buffer declaration for storing stereo parameters
			//--- trying to preserve original shader command flow
			addedCbDecl = true;
			modifiedShader.push_back(cbDeclInst);
		}
		modifiedShader.push_back(originalShader[i]);
	}

	//--- insert decl_tmp command if it is not found in original shader
	if (!modifiedTmpDCL && needTmpDCLModification) {
		modifiedShader.push_back(tmpDeclInst);
	}

	//--- same for the cb decl: without this a shader declaring no constant buffer
	//--- would reference cb[freeCBIndex] undeclared and fault in the driver
	if (!addedCbDecl) {
		modifiedShader.push_back(cbDeclInst);
	}

	//--- substitute output register ---
	//--- copy all command before "ret" one ---
	int outputMask = 0;
	int outputMaskMerged = 0;
	ShaderOperand* outputOperand = 0;
	ShaderOperand* immediateOperand = 0;
	for(; i < originalShader.size(); i++)
	{
		outputOperand = 0;
		immediateOperand = 0;

		ShaderInstruction si(originalShader[i]);
		for(unsigned j = 0; j < si.getOperandsCount(); j++)
		{
			ShaderOperand* so = const_cast<ShaderOperand*>( si.getOperand(j) );
			if (so->getOperandType() == D3D10_SB_OPERAND_TYPE_OUTPUT &&
				so->getIndex(0) == outputRegisterIndex)
			{
				outputOperand = so;
				outputMask = so->getComponentState();
				outputMaskMerged |= outputMask;
			}
			else if (so->getOperandType() == D3D10_SB_OPERAND_TYPE_IMMEDIATE32) {
				immediateOperand = so;
			}
		}

		//--- change command if needed;
		if (outputOperand) 
		{
			// Full-write only: bailing on the tail of a split write ("mov o0.xyz, v0"
			// then "mov o0.w, l(1)") left o0.xyz redirected but never written back.
			if (immediateOperand && si.getOperation()->getType() == D3D10_SB_OPCODE_MOV &&
				outputMask == ShaderOperand::ALL_COMPONENTS)
			{
				// Disable modification if immediate operand detected, this is not a worldViewProj transform and
				// modification of such command can lead to crash(Detected in BFBC 2)
				// May be driver interpret this command as branching hint. E.g. "mov o0, l(0.0, 0.0, 0.0, 0.0)" means discard.
				outputMaskMerged = 0;
				immediateOperand = 0;
			}
			else
			{
				//--- write to tmp register instead of output to allow further modification
				outputOperand->setOperandType(D3D10_SB_OPERAND_TYPE_TEMP);
				outputOperand->setImmediate32IndexIfItIsTheOne(tmpReg[0]);

				if (outputMaskMerged == ShaderOperand::ALL_COMPONENTS)
				{
					modifiedShader.push_back(si);
					std::copy(modificationCommands.begin(), modificationCommands.end(), std::back_inserter(modifiedShader));

					//--- reset mask to allow further modification insertion. E.g. in other branch of 'if'.
					outputMaskMerged = 0;
					continue;
				}
			}
		}
		
		modifiedShader.push_back(si);
	}
	assert(outputMaskMerged == 0);

	return true;
}
