/* wiz3D - DXBC container rebuild
 *
 * Emits a new container with one chunk's payload replaced, fixing the chunk offset
 * table, the total-size field and the digest. Used to substitute a modified SHEX /
 * SHDR (shader code) chunk produced by ShaderAnalysis' ModifyShader + ShaderList2Blob,
 * which is how the legacy DDI path already does stereo shader modification.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>

namespace wiz3d
{

// FourCC tags, low byte first: 'DXBC', 'SHEX' (SM5), 'SHDR' (SM4).
static const DWORD kFourCC_DXBC = 0x43425844;
static const DWORD kFourCC_SHEX = 0x58454853;
static const DWORD kFourCC_SHDR = 0x52444853;

// Locates a chunk's payload. Returns nullptr when absent; outSize is the payload
// size excluding the 8-byte chunk header.
const BYTE* FindDxbcChunk(const void* blob, SIZE_T blobSize, DWORD fourCC, DWORD& outSize);

// Returns the tag of whichever shader-code chunk this blob carries, or 0.
DWORD FindDxbcShaderChunkTag(const void* blob, SIZE_T blobSize);

// Rebuilds `blob` with the payload of `fourCC` replaced by newPayload. Every other
// chunk is copied verbatim. Returns false if the chunk is absent or the blob is
// malformed; on success outBlob holds a complete, correctly-checksummed container.
bool RebuildDxbcWithChunk(const void* blob, SIZE_T blobSize,
                          DWORD fourCC, const void* newPayload, DWORD newPayloadSize,
                          std::vector<BYTE>& outBlob);

// Verifies the checksum implementation and the rebuild path against a blob the
// caller knows is valid. Logs via DDILog. Returns true when both round-trip.
bool DxbcSelfTest(const void* knownGoodBlob, SIZE_T blobSize);

} // namespace wiz3d
