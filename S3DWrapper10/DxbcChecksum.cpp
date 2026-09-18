/* wiz3D - DXBC container checksum. See DxbcChecksum.h. */

#include "StdAfx.h"
#include "DxbcChecksum.h"
#include <string.h>

namespace wiz3d
{
namespace
{

struct Md5State { DWORD a, b, c, d; };

const DWORD kK[64] = {
0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };

const int kR[64] = {
7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };

inline DWORD Rol(DWORD x, int c) { return (x << c) | (x >> (32 - c)); }

// Standard RFC 1321 block function — verified against the published test vectors.
void Md5Block(Md5State& s, const BYTE* p)
{
    DWORD m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | ((DWORD)p[i * 4 + 3] << 24);

    DWORD a = s.a, b = s.b, c = s.c, d = s.d;
    for (int i = 0; i < 64; ++i)
    {
        DWORD f; int g;
        if      (i < 16) { f = (b & c) | (~b & d);  g = i; }
        else if (i < 32) { f = (d & b) | (~d & c);  g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;           g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);        g = (7 * i) & 15; }
        DWORD t = d; d = c; c = b;
        b = b + Rol(a + f + kK[i] + m[g], kR[i]);
        a = t;
    }
    s.a += a; s.b += b; s.c += c; s.d += d;
}

} // namespace

bool ComputeDxbcChecksum(const void* blob, SIZE_T blobSize, BYTE outDigest[16])
{
    if (!blob || !outDigest || blobSize <= kDxbcPayloadOffset) return false;

    const BYTE* payload = static_cast<const BYTE*>(blob) + kDxbcPayloadOffset;
    const DWORD size    = static_cast<DWORD>(blobSize - kDxbcPayloadOffset);

    Md5State s = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    const DWORD leftOver = size & 0x3F;
    const DWORD whole    = size - leftOver;
    for (DWORD i = 0; i < whole; i += 64) Md5Block(s, payload + i);

    const BYTE* tail    = payload + whole;
    const DWORD numBits = size << 3;
    // Non-standard tail marker: (byteCount * 2) | 1, expressed as vkd3d does.
    const DWORD sentinel = (numBits >> 2) | 1;
    BYTE blk[64];

    if (leftOver < 56)
    {
        // Single final block; note the leftover data starts at offset 4, after
        // the bit count, which is what makes this differ from textbook MD5.
        memset(blk, 0, sizeof(blk));
        memcpy(blk, &numBits, 4);
        memcpy(blk + 4, tail, leftOver);
        blk[4 + leftOver] = 0x80;
        memcpy(blk + 60, &sentinel, 4);
        Md5Block(s, blk);
    }
    else
    {
        memset(blk, 0, sizeof(blk));
        memcpy(blk, tail, leftOver);
        if (leftOver < 64) blk[leftOver] = 0x80;
        Md5Block(s, blk);

        memset(blk, 0, sizeof(blk));
        memcpy(blk, &numBits, 4);
        memcpy(blk + 60, &sentinel, 4);
        Md5Block(s, blk);
    }

    const DWORD out[4] = { s.a, s.b, s.c, s.d };
    memcpy(outDigest, out, 16);
    return true;
}

bool WriteDxbcChecksum(void* blob, SIZE_T blobSize)
{
    BYTE digest[16];
    if (!ComputeDxbcChecksum(blob, blobSize, digest)) return false;
    memcpy(static_cast<BYTE*>(blob) + kDxbcDigestOffset, digest, 16);
    return true;
}

} // namespace wiz3d
