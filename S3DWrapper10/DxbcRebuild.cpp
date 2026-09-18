/* wiz3D - DXBC container rebuild. See DxbcRebuild.h. */

#include "StdAfx.h"
#include "DxbcRebuild.h"
#include "DxbcChecksum.h"
#include "AdapterFunctions.h"   // DDILog
#include <string.h>

namespace wiz3d
{
namespace
{

// Container header: magic, 16-byte digest, version, total size, chunk count,
// then chunkCount DWORD offsets.
static const SIZE_T kHeaderFixedSize = 32;

inline DWORD ReadU32(const BYTE* p) { DWORD v; memcpy(&v, p, 4); return v; }

bool ReadHeader(const void* blob, SIZE_T blobSize, DWORD& outChunkCount, const DWORD*& outOffsets)
{
    if (!blob || blobSize < kHeaderFixedSize) return false;
    const BYTE* b = static_cast<const BYTE*>(blob);
    if (ReadU32(b) != kFourCC_DXBC) return false;

    outChunkCount = ReadU32(b + 28);
    if (outChunkCount == 0 || outChunkCount > 64) return false;
    if (kHeaderFixedSize + SIZE_T(outChunkCount) * 4 > blobSize) return false;

    outOffsets = reinterpret_cast<const DWORD*>(b + kHeaderFixedSize);
    return true;
}

} // namespace

const BYTE* FindDxbcChunk(const void* blob, SIZE_T blobSize, DWORD fourCC, DWORD& outSize)
{
    DWORD count = 0; const DWORD* offsets = nullptr;
    if (!ReadHeader(blob, blobSize, count, offsets)) return nullptr;

    const BYTE* b = static_cast<const BYTE*>(blob);
    for (DWORD i = 0; i < count; ++i)
    {
        const DWORD off = offsets[i];
        if (off + 8 > blobSize) continue;
        if (ReadU32(b + off) != fourCC) continue;
        const DWORD csz = ReadU32(b + off + 4);
        if (SIZE_T(off) + 8 + csz > blobSize) return nullptr;
        outSize = csz;
        return b + off + 8;
    }
    return nullptr;
}

DWORD FindDxbcShaderChunkTag(const void* blob, SIZE_T blobSize)
{
    DWORD sz = 0;
    if (FindDxbcChunk(blob, blobSize, kFourCC_SHEX, sz)) return kFourCC_SHEX;
    if (FindDxbcChunk(blob, blobSize, kFourCC_SHDR, sz)) return kFourCC_SHDR;
    return 0;
}

bool RebuildDxbcWithChunk(const void* blob, SIZE_T blobSize,
                          DWORD fourCC, const void* newPayload, DWORD newPayloadSize,
                          std::vector<BYTE>& outBlob)
{
    DWORD count = 0; const DWORD* offsets = nullptr;
    if (!ReadHeader(blob, blobSize, count, offsets)) return false;
    if (!newPayload && newPayloadSize) return false;

    const BYTE* b = static_cast<const BYTE*>(blob);

    // Chunks are not required to appear in offset order, so gather them first and
    // emit in the original table order to keep the layout recognisable.
    bool found = false;
    SIZE_T total = kHeaderFixedSize + SIZE_T(count) * 4;
    for (DWORD i = 0; i < count; ++i)
    {
        const DWORD off = offsets[i];
        if (off + 8 > blobSize) return false;
        const DWORD csz = ReadU32(b + off + 4);
        if (SIZE_T(off) + 8 + csz > blobSize) return false;
        const bool isTarget = (ReadU32(b + off) == fourCC);
        if (isTarget) found = true;
        total += 8 + (isTarget ? newPayloadSize : csz);
    }
    if (!found) return false;

    outBlob.assign(total, 0);
    BYTE* dst = outBlob.data();

    memcpy(dst, b, kHeaderFixedSize);                  // magic + stale digest + version
    memcpy(dst + 24, &total, 4);                       // total size
    DWORD* dstOffsets = reinterpret_cast<DWORD*>(dst + kHeaderFixedSize);

    SIZE_T cursor = kHeaderFixedSize + SIZE_T(count) * 4;
    for (DWORD i = 0; i < count; ++i)
    {
        const DWORD off = offsets[i];
        const DWORD tag = ReadU32(b + off);
        const DWORD csz = ReadU32(b + off + 4);
        const bool  isTarget = (tag == fourCC);
        const DWORD outSz = isTarget ? newPayloadSize : csz;

        dstOffsets[i] = static_cast<DWORD>(cursor);
        memcpy(dst + cursor, &tag, 4);
        memcpy(dst + cursor + 4, &outSz, 4);
        if (outSz)
            memcpy(dst + cursor + 8, isTarget ? static_cast<const BYTE*>(newPayload) : b + off + 8, outSz);
        cursor += 8 + outSz;
    }

    return WriteDxbcChecksum(outBlob.data(), outBlob.size());
}

bool DxbcSelfTest(const void* knownGoodBlob, SIZE_T blobSize)
{
    if (!knownGoodBlob || blobSize <= kDxbcPayloadOffset) return false;
    const BYTE* b = static_cast<const BYTE*>(knownGoodBlob);

    // 1. Our checksum must reproduce the digest the compiler already stored.
    BYTE digest[16];
    if (!ComputeDxbcChecksum(knownGoodBlob, blobSize, digest))
    {
        DDILog("  DxbcSelfTest: checksum computation failed\n");
        return false;
    }
    if (memcmp(digest, b + kDxbcDigestOffset, 16) != 0)
    {
        DDILog("  DxbcSelfTest: checksum MISMATCH (size=%zu) -- shader modification unsafe\n", blobSize);
        return false;
    }

    // 2. Rebuilding with the shader chunk unchanged must reproduce the blob exactly.
    const DWORD tag = FindDxbcShaderChunkTag(knownGoodBlob, blobSize);
    if (!tag)
    {
        DDILog("  DxbcSelfTest: no SHEX/SHDR chunk found\n");
        return false;
    }
    DWORD payloadSize = 0;
    const BYTE* payload = FindDxbcChunk(knownGoodBlob, blobSize, tag, payloadSize);
    if (!payload) return false;

    std::vector<BYTE> rebuilt;
    if (!RebuildDxbcWithChunk(knownGoodBlob, blobSize, tag, payload, payloadSize, rebuilt))
    {
        DDILog("  DxbcSelfTest: rebuild failed\n");
        return false;
    }
    if (rebuilt.size() != blobSize || memcmp(rebuilt.data(), knownGoodBlob, blobSize) != 0)
    {
        DDILog("  DxbcSelfTest: round-trip differs (orig=%zu rebuilt=%zu)\n", blobSize, rebuilt.size());
        return false;
    }

    DDILog("  DxbcSelfTest: OK (size=%zu, chunk=0x%08lX) -- checksum and rebuild verified\n",
           blobSize, tag);
    return true;
}

} // namespace wiz3d
