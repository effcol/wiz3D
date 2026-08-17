/* wiz3D - automatic stereo shader modification. See ShaderModify11.h. */

#include "StdAfx.h"
#include "ShaderModify11.h"
#include "DxbcRebuild.h"
#include "AdapterFunctions.h"   // DDILog
#include "..\ShaderAnalysis\Disasm.h"

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

} // namespace wiz3d
