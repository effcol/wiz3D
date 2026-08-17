/* wiz3D - DXBC container checksum
 *
 * The D3D11 runtime validates the 16-byte digest in a DXBC container header and
 * rejects CreateXxxShader with E_INVALIDARG if it does not match the body, so any
 * shader we rebuild must carry a correct one. The algorithm is a modified MD5:
 * standard core, non-standard final block. Cross-checked against D3DCompile output
 * for vs_4_0/vs_5_0/ps_5_0 over every leftover-length class (see DxbcRebuild.cpp
 * SelfTest). Reference: vkd3d-shader/checksum.c (vkd3d_compute_dxbc_checksum).
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace wiz3d
{

// Bytes [0,20) of a container are the 'DXBC' magic plus the digest itself; the
// hash covers everything after them.
static const SIZE_T kDxbcDigestOffset  = 4;
static const SIZE_T kDxbcPayloadOffset = 20;

// Computes the digest for a whole container. outDigest receives 16 bytes. The
// existing digest bytes in the blob are not read, so it is safe to call on a blob
// whose digest is stale or zeroed. Returns false if the blob is too small.
bool ComputeDxbcChecksum(const void* blob, SIZE_T blobSize, BYTE outDigest[16]);

// Computes and writes the digest in place at bytes [4,20).
bool WriteDxbcChecksum(void* blob, SIZE_T blobSize);

} // namespace wiz3d
