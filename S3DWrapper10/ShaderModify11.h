/* wiz3D - automatic stereo shader modification for the COM-wrap path
 *
 * Mirrors what the legacy DDI path does in ShaderWrapper::ProcessShader: run the
 * shared ShaderAnalysis modifier over a vertex shader's instruction stream so it
 * shifts its own output position, instead of us trying to find and patch a
 * projection matrix in a constant buffer. Shaders whose matrix the analyzer cannot
 * locate — 63% of GTA V's CB writes — are unreachable by the CB path but fine here,
 * because this never needs to identify the matrix.
 *
 * The DDI path gets raw shader tokens; COM-wrap gets a DXBC container with a
 * validated digest, so the modified token stream is re-wrapped via DxbcRebuild.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include "..\ShaderAnalysis\modify.h"

namespace wiz3d
{

// Produces a stereo-modified copy of a DXBC vertex shader. posRegister is the
// SV_Position output register from ShaderAnalysis11Result. On success outBlob holds
// a complete, correctly-checksummed container and outData records the constant
// buffer slot and registers the modifier reserved.
// Returns false when the shader cannot be modified (no free CB slot or temp
// register, unparseable stream); the caller should fall back to CB patching.
bool TryModifyShaderForStereo(const void* bytecode, SIZE_T byteLength,
                              DWORD posRegister, bool addZNearCheck,
                              std::vector<BYTE>& outBlob, ModifiedShaderData& outData);

// Writes before/after disassembly to wiz3D_vs_<idx>_<crc>.txt beside the exe.
void DumpShaderPair(const void* orig, SIZE_T origLen, const void* mod, SIZE_T modLen,
                    unsigned idx, DWORD crc);

} // namespace wiz3d
