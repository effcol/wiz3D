/* wiz3D - DXBC-driven shader analyzer adapter (Option B Stage 4e)
 *
 * Bridges the COM-wrap path (raw DXBC blobs from Create{Vertex,Pixel,...}Shader)
 * to the existing iZ3D shader_analyzer in ShaderAnalysis/, which was written
 * against the legacy DDI surface (D3D10DDIARG_STAGE_IO_SIGNATURES + a SHEX/SHDR
 * chunk pointer). Walks DXBC chunks, extracts the SHDR/SHEX chunk and the
 * OSGN/OSG5 output signature, builds a synthetic STAGE_IO_SIGNATURES from
 * the OSGN element table, then calls shader_analyzer::GetProjectionMatrices.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include "..\ShaderAnalysis\AnalyzeData.h"

struct D3D10DDIARG_SIGNATURE_ENTRY;

namespace wiz3d
{

// Result of analyzing a DXBC blob. ProjectionShaderData mirrors the legacy
// analyzer's output — see ShaderAnalysis/AnalyzeData.h.
struct ShaderAnalysis11Result
{
    bool                  parsed;     // SHDR/SHEX + OSGN chunks both present and parsed
    DWORD                 crc32;      // bytecode CRC32 — diagnostic, helps cross-match against legacy DDI logs
    ProjectionShaderData  projection; // matrix register info per CB
    // Register holding SV_Position. ModifyShader needs it to know which output to
    // shift; valid only when parsed is true.
    DWORD                 posRegister;
};

// Analyze a DXBC bytecode blob. Returns true if a SHDR/SHEX + OSGN pair was
// found and at least walked; the caller still needs to inspect
// projection.matrixData.cb.empty() to know whether any projection matrices
// were detected. Safe to call with nullptr/0 — returns parsed=false.
bool AnalyzeShader11(const void* pBytecode, SIZE_T byteLength,
                     ShaderAnalysis11Result& outResult);

} // namespace wiz3d
