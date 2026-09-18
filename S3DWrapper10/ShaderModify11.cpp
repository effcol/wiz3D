/* wiz3D - automatic stereo shader modification. See ShaderModify11.h. */

#include "StdAfx.h"
#include "ShaderModify11.h"
#include "DxbcRebuild.h"
#include "AdapterFunctions.h"   // DDILog
#include "..\ShaderAnalysis\Disasm.h"
#include <d3dcompiler.h>
#include <stdio.h>
#pragma comment(lib, "d3dcompiler.lib")

namespace wiz3d
{

bool TryModifyShaderForStereo(const void* bytecode, SIZE_T byteLength,
                              DWORD posRegister, bool addZNearCheck,
                              std::vector<BYTE>& outBlob, ModifiedShaderData& outData)
{
    if (!bytecode || byteLength < 32) return false;

    const DWORD tag = FindDxbcShaderChunkTag(bytecode, byteLength);
    if (!tag) return false;

    DWORD payloadSize = 0;
    const BYTE* payload = FindDxbcChunk(bytecode, byteLength, tag, payloadSize);
    if (!payload || payloadSize < 8) return false;

    // Chunk payload is [versionToken][lengthInDwords][instructions...], the same
    // shape the DDI hands to ShaderWrapper as pCode.
    const DWORD* code = reinterpret_cast<const DWORD*>(payload);
    const DWORD  versionToken = code[0];

    shader_analyzer::TShaderList shList;
    shader_analyzer::ParseShader(reinterpret_cast<const unsigned*>(payload), shList);
    if (shList.empty()) return false;

    // Skip pre-transformed positions (screen-space quads); shifting those breaks
    // every fullscreen pass. DX9 equivalent: AnalysisSharedIsMono.
    bool positionIsComputed = false;
    for (size_t i = 0; i < shList.size() && !positionIsComputed; ++i)
    {
        const shader_analyzer::ShaderInstruction& si = shList[i];
        // dcl_output_siv names o0 without writing it; only real instructions count.
        if (si.getOperation()->isDeclaration() || si.getOperation()->isCustomData()) continue;
        bool writesPos = false;
        for (unsigned j = 0; j < si.getOperandsCount(); ++j)
        {
            const shader_analyzer::ShaderOperand* so = si.getOperand(j);
            if (so->getOperandType() == D3D10_SB_OPERAND_TYPE_OUTPUT &&
                so->getIndex(0) == posRegister)
            { writesPos = true; break; }
        }
        if (!writesPos) continue;

        // A mov from input/literal carries no transform; anything else does.
        if (si.getOperation()->getType() != D3D10_SB_OPCODE_MOV) { positionIsComputed = true; break; }
        for (unsigned j = 1; j < si.getOperandsCount(); ++j)
        {
            const D3D10_SB_OPERAND_TYPE t = si.getOperand(j)->getOperandType();
            if (t != D3D10_SB_OPERAND_TYPE_INPUT && t != D3D10_SB_OPERAND_TYPE_IMMEDIATE32)
            { positionIsComputed = true; break; }
        }
    }
    if (!positionIsComputed) return false;

    // ModifyShader's output-mask tracking assumes straight-line flow; branches
    // put the shift in the wrong block.
    for (size_t i = 0; i < shList.size(); ++i)
    {
        switch (shList[i].getOperation()->getType())
        {
        case D3D10_SB_OPCODE_IF:    case D3D10_SB_OPCODE_ELSE:
        case D3D10_SB_OPCODE_LOOP:  case D3D10_SB_OPCODE_BREAK:
        case D3D10_SB_OPCODE_BREAKC: case D3D10_SB_OPCODE_SWITCH:
        case D3D10_SB_OPCODE_CALL:  case D3D10_SB_OPCODE_CALLC:
        case D3D10_SB_OPCODE_RETC:  case D3D10_SB_OPCODE_DISCARD:
            return false;
        case D3D10_SB_OPCODE_RET:
            // A ret anywhere but the final instruction is an early-out branch.
            if (i + 1 != shList.size()) return false;
            break;
        default: break;
        }
    }

    // A literal position w means no perspective divide: a pre-transformed 2D
    // quad, which shifting only slides sideways. DX9 calls this AnalysisSharedIsMono.
    for (size_t i = 0; i < shList.size(); ++i)
    {
        const shader_analyzer::ShaderInstruction& si = shList[i];
        if (si.getOperation()->getType() != D3D10_SB_OPCODE_MOV) continue;
        bool writesPosW = false, fromLiteral = false;
        for (unsigned j = 0; j < si.getOperandsCount(); ++j)
        {
            const shader_analyzer::ShaderOperand* so = si.getOperand(j);
            if (so->getOperandType() == D3D10_SB_OPERAND_TYPE_OUTPUT &&
                so->getIndex(0) == posRegister &&
                (so->getComponentState() & shader_analyzer::ShaderOperand::W_STATE))
                writesPosW = true;
            else if (so->getOperandType() == D3D10_SB_OPERAND_TYPE_IMMEDIATE32)
                fromLiteral = true;
        }
        if (writesPosW && fromLiteral) return false;
    }

    shader_analyzer::TShaderList modified;
    if (!ModifyShader(shList, posRegister, addZNearCheck, modified, outData))
        return false;

    // + 32 dwords of headroom, matching ShaderWrapper.cpp's call.
    std::vector<UINT> tokens;
    if (!shader_analyzer::ShaderList2Blob(modified, versionToken, tokens, code[1] + 32))
        return false;
    if (tokens.empty()) return false;

    const DWORD newPayloadSize = static_cast<DWORD>(tokens.size() * sizeof(UINT));
    if (!RebuildDxbcWithChunk(bytecode, byteLength, tag,
                              tokens.data(), newPayloadSize, outBlob))
        return false;

    outData.ModifiedShaderAvailable = true;
    return true;
}

void DumpShaderBytecode(const char* kind, const void* code, SIZE_T len, unsigned idx)
{
    if (!gInfo.DumpModifiedShaders || idx > 400 || !code) return;

    ID3DBlob* a = nullptr;
    if (FAILED(D3DDisassemble(code, len, 0, nullptr, &a)) || !a) return;

    char path[MAX_PATH];
    sprintf_s(path, "wiz3D_%s_%03u.txt", kind, idx);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") == 0 && f)
    {
        fprintf(f, "%s\n", (const char*)a->GetBufferPointer());
        fclose(f);
    }
    a->Release();
}

// Before/after disassembly, named by modification index and CRC so the files
// line up with the TryBuildModifiedVS[N] log lines.
void DumpShaderPair(const void* orig, SIZE_T origLen, const void* mod, SIZE_T modLen,
                    unsigned idx, DWORD crc)
{
    if (!gInfo.DumpModifiedShaders) return;
    if (idx > 400) return;

    ID3DBlob* a = nullptr; ID3DBlob* b = nullptr;
    if (FAILED(D3DDisassemble(orig, origLen, 0, nullptr, &a)) || !a) return;
    if (FAILED(D3DDisassemble(mod, modLen, 0, nullptr, &b)) || !b) { a->Release(); return; }

    char path[MAX_PATH];
    sprintf_s(path, "wiz3D_vs_%03u_%08lX.txt", idx, crc);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") == 0 && f)
    {
        fprintf(f, "==== ORIGINAL (%zu bytes) ====\n%s\n", origLen, (const char*)a->GetBufferPointer());
        fprintf(f, "\n==== MODIFIED (%zu bytes) ====\n%s\n", modLen, (const char*)b->GetBufferPointer());
        fclose(f);
        DDILog("  DumpShaderPair: wrote %s\n", path);
    }
    a->Release(); b->Release();
}

} // namespace wiz3d
