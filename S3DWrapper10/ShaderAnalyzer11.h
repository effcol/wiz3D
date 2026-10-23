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
#include <memory>
#include <string>
#include <utility>
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
    // Located by the names the game's <Profile> gives (g_ProfileData), from RDEF; -1 = absent or not read.
    // StereoTexture's t-slot, and the cb slot + byte offset of its EyeOffset view inverse (row 0 = the camera's right axis).
    int                   stereoParmsSlot = -1;
    int                   viewInvCB       = -1;
    UINT                  viewInvOffset   = 0;
    // MirroredTarget's t-slot: a planar reflection, sampled at u = 0.5 - 0.5*x/w (MP3 mirrors and water).
    int                   reflectTexSlot  = -1;
    // AdditionalMatrixName matrices the shader reads: cb slot and first register each.
    std::vector<std::pair<int, UINT>> namedMatrices;
    // Pixel and compute shaders: every float4x4 cbuffer variable read (cb slot, byte offset), for the right eye's
    // camera matrices at the draw or dispatch.
    std::vector<std::pair<int, UINT>> readMatrices;
    // Outputs other than SV_Position built by four dp4 with consecutive registers of one cbuffer: (cb slot, byte offset)
    // of each float4x4, what a vertex shader without RDEF is known to read.
    std::vector<std::pair<int, UINT>> dp4OutputMatrices;
    // Pixel and compute shaders without RDEF: 3x4 affines read as mul/mad columns cbN[k..k + 2] of one vector's x, y, z
    // (cb slot, byte offset of the first register), matched at the draw against the camera's view (ViewMatrices).
    std::vector<std::pair<int, UINT>> viewMatrices;
    // ViewportMatrices, without RDEF: rows k, k + 1 and k + 3 of a float4x4 read as dp4 of one vector, a pixel position
    // (x * w, y * w, w) to divide (cb slot, byte offset of row k); also in readMatrices.
    std::vector<std::pair<int, UINT>> pixelMatrices;
    // readMatrices came from the code (CodeMatrices): each is read as dp4 rows, its registers the output's rows.
    bool matricesFromCode = false;
    // HudDepth: the w the game gives a HUD element under 3D Vision (MP3's ApparentDepth).
    int                   apparentDepthCB     = -1;
    UINT                  apparentDepthOffset = 0;
    // HudRect, same cbuffer: ndc = corner * rect.xy + rect.zw (MP3's TransPos).
    int                   transPosCB          = -1;
    UINT                  transPosOffset      = 0;
    // CameraTanX: .x = tan of the half FOV, 1/p00 of the camera (MP3's gDeferredProjParams).
    int                   projParamsCB        = -1;
    UINT                  projParamsOffset    = 0;
    // Cbuffers a profile feature edits for the right eye (ProfileFeature::EditRightConstants), by name, and their slots.
    std::vector<std::pair<std::string, int>> rightCBs;
    void NoteRightCB(const std::string& name, int slot)
    {
        for (const auto& n : rightCBs) if (n.first == name) return;
        rightCBs.emplace_back(name, slot);
    }
    // Names a feature recognised in this shader (a cbuffer, texture or variable it reads), for per-pass decisions.
    std::vector<std::string> tags;
    // Byte offsets a feature located in this shader's cbuffers, under a key the feature chooses.
    std::vector<std::pair<std::string, std::vector<int>>> featureOffsets;
    // Shader resources a feature named, and their t-slots (any SRV kind: texture or buffer).
    std::vector<std::pair<std::string, int>> namedSRVs;
    // Texture slots where the right eye reads the game's (left) copy: light-space maps a profile names (LeftEyeTextures).
    std::vector<int> leftSRVSlots;
    // The output signature's elements in order, for a geometry shader that takes this vertex shader's outputs.
    struct OutputElement { std::string name; UINT index, reg, mask, type, sysValue; };
    std::vector<OutputElement> outputs;
    // SV_Position is the vertex input with w = 1.0: the vertex buffer holds NDC the CPU projected (ClipSpaceSprites).
    bool clipSpacePosition = false;
    // SV_Position's x, y and z built from vertex inputs alone (no constant-buffer or system-value term, z an input's),
    // w = 1.0: sprites the CPU projected and the shader rotates and scales (Andromeda's flares; ClipSpaceSprites 3).
    bool clipSpaceBuilt = false;
    // With clipSpaceBuilt: the input holding the sprite's world position and the camera position the shader measures
    // it from (cbN[k].xyz added to it), for the sprite's own depth (ClipSpaceSprites 3); -1 = none.
    int  spriteWorldInput = -1;
    int  spriteCameraCB = -1;
    UINT spriteCameraReg = 0;
    // With clipSpacePosition: the index in outputs of a float3 the shader copies from an input, a per-vertex world
    // ray (Frostbite's sky quad carries its frustum corner rays this way); -1 = none (ClipSpaceRays).
    int rayOutput = -1;
    // The bytecode: during shader creation for every stage, kept afterwards for compute shaders only (null past the
    // keep budget).
    std::shared_ptr<const std::vector<BYTE>> bytecode;
    // A feature's right eye draws a twin of this shader: its code is kept past the budget.
    bool keepCode = false;
    // 0.4.1 inline ShaderOverride: true if this shader binds an SRV at the
    // reserved stereo-params slot (t125 by convention — see StereoParams11.h).
    // Context11Proxy::EditRightCBs sees this and skips the CB-feature edit
    // pass for the stage; the shader is doing its own per-eye offset via the
    // stereo-params texture, so applying our offset too would double-shift.
    bool usesStereoParams = false;
};

// Analyze a DXBC bytecode blob. Returns true if a SHDR/SHEX + OSGN pair was
// found and at least walked; the caller still needs to inspect
// projection.matrixData.cb.empty() to know whether any projection matrices
// were detected. Safe to call with nullptr/0 — returns parsed=false.
bool AnalyzeShader11(const void* pBytecode, SIZE_T byteLength,
                     ShaderAnalysis11Result& outResult);

// CRC32 over the raw bytecode. Matches ShaderAnalysis11Result::crc32
// (same algorithm). Used by 0.4.1 ShaderOverride to look up a profile
// entry by CRC before creating the game-side shader, so an overridden
// shader can substitute its bytecode before the real D3D11 create call.
DWORD ShaderBytecodeCRC32(const void* pBytecode, SIZE_T byteLength);

// A DXBC chunk's data (after its tag and size) and size, by four-character tag; null when the blob has none.
const BYTE* FindDxbcChunk(const void* blob, SIZE_T blobLen, DWORD tag, DWORD& outSize);

// What a shader without RDEF reads, from its code (cb slot, byte offset): float4x4s, and 3x4 affines read as columns
// that may be the camera's view (ShaderAnalysis11Result::viewMatrices).
void CodeMatrixReads(const void* pBytecode, SIZE_T byteLength, std::vector<std::pair<int, UINT>>& mats,
                     std::vector<std::pair<int, UINT>>& views);
// Float4x4s a shader without RDEF reads as rows k, k + 1 and k + 3 only: a pixel position (ShaderAnalysis11Result::pixelMatrices).
void CodePixelMatrices(const void* pBytecode, SIZE_T byteLength, std::vector<std::pair<int, UINT>>& out);
// Matrices a shader without RDEF reads as (x*W, y*W, W, 1): (cb slot, byte offset).
void CodeScreenMatrices(const void* pBytecode, SIZE_T byteLength, std::vector<std::pair<int, UINT>>& out);
// Every constant register the code reads: (cb slot, register), ~0u where it indexes the slot at run time.
void CodeConstantReads(const void* pBytecode, SIZE_T byteLength, std::vector<std::pair<int, UINT>>& out);
// The constant registers the code reads whole in a dp4: (cb slot, register).
void CodeDp4ConstantReads(const void* pBytecode, SIZE_T byteLength, std::vector<std::pair<int, UINT>>& out);

} // namespace wiz3d
