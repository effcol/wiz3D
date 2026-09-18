/* wiz3D - DXBC analyzer adapter implementation */

#include "StdAfx.h"
#include "ShaderAnalyzer11.h"
// Disasm.h pulls in d3d11tokenizedprogramformat.hpp via the analyzer's
// internal includes, which is the SM5 superset of the d3d10 version. Don't
// directly include the d3d10 variant — both define the same D3D10_SB_NAME
// enum and the linker barfs on the duplicate.
#include "..\ShaderAnalysis\Disasm.h"  // shader_analyzer::ParseShader / GetProjectionMatrices
#include <d3d10umddi.h>                // D3D10DDIARG_SIGNATURE_ENTRY / STAGE_IO_SIGNATURES

namespace
{

constexpr DWORD kDXBC = 'CBXD';
constexpr DWORD kSHDR = 'RDHS';
constexpr DWORD kSHEX = 'XEHS';
constexpr DWORD kOSGN = 'NGSO';
constexpr DWORD kOSG5 = '5GSO';
constexpr DWORD kOSG1 = '1GSO';

DWORD ComputeCRC32(const BYTE* p, SIZE_T n)
{
    DWORD crc = 0xFFFFFFFFu;
    for (SIZE_T i = 0; i < n; ++i)
    {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & -(LONG)(crc & 1u));
    }
    return ~crc;
}

// DXBC blob layout:
//   DWORD  fourCC ('DXBC')
//   BYTE   hash[16]
//   DWORD  one (always 1)
//   DWORD  totalSize
//   DWORD  chunkCount
//   DWORD  chunkOffsets[chunkCount]    -- offsets from start of blob
// Each chunk:
//   DWORD  chunkTag
//   DWORD  chunkSize (bytes following these 8)
//   BYTE   chunkData[chunkSize]
//
// Returns pointer to chunk's data section (right after tag+size), and writes
// chunkSize. Returns nullptr if not found.
const BYTE* FindChunk(const BYTE* blob, SIZE_T blobLen, DWORD wantTag,
                      DWORD& outChunkSize)
{
    outChunkSize = 0;
    if (!blob || blobLen < 32) return nullptr;

    const DWORD* head = reinterpret_cast<const DWORD*>(blob);
    if (head[0] != kDXBC) return nullptr;

    // chunkCount at offset 28 bytes in
    if (blobLen < 32) return nullptr;
    DWORD chunkCount = head[7];
    if (chunkCount == 0 || chunkCount > 64) return nullptr; // sanity cap

    const DWORD* chunkOffsets = head + 8;
    SIZE_T offsetTableEnd = 32 + chunkCount * sizeof(DWORD);
    if (offsetTableEnd > blobLen) return nullptr;

    for (DWORD i = 0; i < chunkCount; ++i)
    {
        DWORD off = chunkOffsets[i];
        if (off + 8 > blobLen) continue;
        const DWORD* chunkHead = reinterpret_cast<const DWORD*>(blob + off);
        if (chunkHead[0] != wantTag) continue;
        DWORD csz = chunkHead[1];
        if (off + 8 + csz > blobLen) return nullptr;
        outChunkSize = csz;
        return blob + off + 8;
    }
    return nullptr;
}

// OSGN/OSG5/OSG1 element header (in chunk data, after the 8-byte chunk header):
//   DWORD elementCount
//   DWORD reserved (must be 8 — offset to first element)
// Each element (OSGN: 24 bytes, OSG5/OSG1: 28/32 — extra trailing fields,
// but the first 24 bytes are layout-stable for our needs):
//   DWORD nameOffset
//   DWORD semanticIndex
//   DWORD systemValueType   (D3D_NAME enum, value matches D3D10_SB_NAME_*)
//   DWORD componentType
//   DWORD registerIndex
//   BYTE  mask
//   BYTE  readWriteMask
//   WORD  stream/padding   (OSG5 uses this; OSGN ignores)
//   ... (OSG5/OSG1 has more fields appended)
//
// We build a parallel array of D3D10DDIARG_SIGNATURE_ENTRY so that the
// existing shader_analyzer::FindOutputSignature can walk it. Output is owned
// by the caller's vector — must outlive any pSignatures usage.
bool ParseOutputSignature(const BYTE* chunkData, DWORD chunkSize, DWORD chunkTag,
                          std::vector<D3D10DDIARG_SIGNATURE_ENTRY>& outEntries)
{
    if (!chunkData || chunkSize < 8) return false;
    const DWORD* hdr = reinterpret_cast<const DWORD*>(chunkData);
    DWORD numEntries = hdr[0];
    if (numEntries == 0 || numEntries > 256) return false;

    // OSGN element stride is 24, OSG5 is 28, OSG1 is 32. The first 24 bytes
    // line up identically across all three, which is all we need.
    DWORD stride = 24;
    if (chunkTag == kOSG5) stride = 28;
    else if (chunkTag == kOSG1) stride = 32;

    SIZE_T need = SIZE_T(8) + SIZE_T(numEntries) * stride;
    if (chunkSize < need) return false;

    outEntries.clear();
    outEntries.reserve(numEntries);
    const BYTE* elBase = chunkData + 8;
    for (DWORD i = 0; i < numEntries; ++i)
    {
        const DWORD* el = reinterpret_cast<const DWORD*>(elBase + i * stride);
        D3D10DDIARG_SIGNATURE_ENTRY entry = {};
        entry.SystemValue = static_cast<D3D10_SB_NAME>(el[2]);
        entry.Register    = el[4];
        // Mask is in the low byte of el[5] but pack as (mask >> 4) per the
        // DDI convention quirks isn't necessary — shader_analyzer only checks
        // SystemValue + Register. Set to 0xF (all components written) so any
        // future consumer behaves sanely.
        entry.Mask        = 0xF;
        outEntries.push_back(entry);
    }
    return true;
}

} // anonymous namespace

namespace wiz3d
{

bool AnalyzeShader11(const void* pBytecode, SIZE_T byteLength,
                     ShaderAnalysis11Result& out)
{
    out = {};
    if (!pBytecode || byteLength < 32) return false;

    const BYTE* blob = static_cast<const BYTE*>(pBytecode);
    out.crc32 = ComputeCRC32(blob, byteLength);

    // SHEX (shader model 5) is preferred over SHDR (SM4) — DXBC blobs only
    // ever carry one of the two.
    DWORD shexSize = 0;
    const BYTE* shex = FindChunk(blob, byteLength, kSHEX, shexSize);
    if (!shex)
    {
        shex = FindChunk(blob, byteLength, kSHDR, shexSize);
    }
    if (!shex) return false;

    // Output signature: try OSG1 (SM 5.1), OSG5 (SM 5), then OSGN (SM 4).
    DWORD osgSize = 0;
    DWORD osgTag  = 0;
    const BYTE* osg = nullptr;
    osg = FindChunk(blob, byteLength, kOSG1, osgSize); osgTag = kOSG1;
    if (!osg) { osg = FindChunk(blob, byteLength, kOSG5, osgSize); osgTag = kOSG5; }
    if (!osg) { osg = FindChunk(blob, byteLength, kOSGN, osgSize); osgTag = kOSGN; }
    if (!osg) return false;

    std::vector<D3D10DDIARG_SIGNATURE_ENTRY> outputEntries;
    if (!ParseOutputSignature(osg, osgSize, osgTag, outputEntries))
        return false;

    D3D10DDIARG_STAGE_IO_SIGNATURES sigs = {};
    sigs.pOutputSignature           = outputEntries.empty() ? nullptr : outputEntries.data();
    sigs.NumOutputSignatureEntries  = static_cast<UINT>(outputEntries.size());

    const D3D10DDIARG_SIGNATURE_ENTRY* pOutPos = nullptr;
    if (!shader_analyzer::FindOutputSignature(&sigs, pOutPos) || !pOutPos)
        return false;

    shader_analyzer::TShaderList shList;
    shader_analyzer::ParseShader(reinterpret_cast<const unsigned*>(shex), shList);
    shader_analyzer::GetProjectionMatrices(pOutPos, out.projection, shList);

    out.posRegister = pOutPos->Register;
    out.parsed = true;
    return true;
}

} // namespace wiz3d
