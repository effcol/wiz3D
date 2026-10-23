/* wiz3D - DXBC analyzer adapter implementation */

#include "StdAfx.h"
#include "ShaderAnalyzer11.h"
// Disasm.h pulls in d3d11tokenizedprogramformat.hpp via the analyzer's
// internal includes, which is the SM5 superset of the d3d10 version. Don't
// directly include the d3d10 variant — both define the same D3D10_SB_NAME
// enum and the linker barfs on the duplicate.
#include "..\ShaderAnalysis\Disasm.h"  // shader_analyzer::ParseShader / GetProjectionMatrices
#include <d3d10umddi.h>                // D3D10DDIARG_SIGNATURE_ENTRY / STAGE_IO_SIGNATURES
#include <algorithm>

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
                          std::vector<D3D10DDIARG_SIGNATURE_ENTRY>& outEntries,
                          std::vector<wiz3d::ShaderAnalysis11Result::OutputElement>& outElements)
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
        // The element as HLSL sees it: name, index, type and the components written (low byte of el[5]).
        wiz3d::ShaderAnalysis11Result::OutputElement oe = {};
        if (el[0] < chunkSize)
        {
            const char* s = reinterpret_cast<const char*>(chunkData) + el[0];
            const SIZE_T room = chunkSize - el[0];
            oe.name.assign(s, strnlen(s, room));
        }
        oe.index = el[1]; oe.sysValue = el[2]; oe.type = el[3]; oe.reg = el[4]; oe.mask = el[5] & 0xFF;
        outElements.push_back(oe);
    }
    return true;
}

// SV_Position written by a mov from an input for x, y and z and by a mov of the literal 1.0 for w, each component
// once: the vertex buffer holds positions the CPU projected to NDC (Frostbite's lens flares and light glows).
bool PositionPassedThrough(const shader_analyzer::TShaderList& shList, unsigned posReg)
{
    using shader_analyzer::ShaderOperand;
    unsigned fromInput = 0, literalOne = 0, writes[4] = {};
    for (const auto& inst : shList)
    {
        const shader_analyzer::ShaderOperation* op = inst.getOperation();
        if (!op || op->isDeclaration() || op->isCustomData() || inst.getOperandsCount() < 1) continue;
        const ShaderOperand* dst = inst.getOperand(0);
        if (!dst || dst->getOperandType() != D3D10_SB_OPERAND_TYPE_OUTPUT || dst->getIndex(0) != posReg) continue;
        const unsigned mask = dst->getComponentState();
        for (unsigned c = 0; c < 4; ++c) if (mask & (1u << c)) ++writes[c];
        if (op->getType() != D3D10_SB_OPCODE_MOV || inst.getOperandsCount() != 2) continue;
        const ShaderOperand* src = inst.getOperand(1);
        if (!src) continue;
        if (src->getOperandType() == D3D10_SB_OPERAND_TYPE_INPUT) { fromInput |= mask; continue; }
        if (src->getOperandType() != D3D10_SB_OPERAND_TYPE_IMMEDIATE32) continue;
        // The literal's dwords follow its token(s); a one-component literal serves every written component.
        const std::vector<unsigned>& d = inst.GetData();
        const size_t at = (size_t)op->getLength() + dst->getLength() + (src->isOperandExtended() ? 2 : 1);
        const bool one = src->getOperandNumComponents() == D3D10_SB_OPERAND_1_COMPONENT;
        for (unsigned c = 0; c < 4; ++c)
        {
            if (!(mask & (1u << c))) continue;
            const size_t k = one ? at : at + c;
            if (k < d.size() && d[k] == 0x3F800000u) literalOne |= 1u << c;
        }
    }
    const unsigned xyz = ShaderOperand::X_STATE | ShaderOperand::Y_STATE | ShaderOperand::Z_STATE;
    return writes[0] == 1 && writes[1] == 1 && writes[2] == 1 && writes[3] == 1 &&
           (fromInput & xyz) == xyz && (literalOne & ShaderOperand::W_STATE) != 0;
}

// SV_Position x, y, z each written once from vertex inputs alone (no cbuffer, table or system value), w a literal 1.0:
// CPU-projected sprites the shader rotates and scales (Andromeda's flares). Per component: bit 0 foreign, bit 1 input.
bool PositionBuiltFromInputs(const shader_analyzer::TShaderList& shList, unsigned posReg)
{
    using shader_analyzer::ShaderOperand;
    struct Temp { unsigned char c[4]; };
    // What every register holds at a point of the code; an if saves it and its endif merges the branches' results.
    struct State
    {
        std::vector<Temp> temps;
        unsigned char indexable = 0, pos[4] = {};
        void Merge(const State& o)
        {
            if (o.temps.size() > temps.size()) temps.resize(o.temps.size(), Temp{});
            for (size_t r = 0; r < o.temps.size(); ++r)
                for (unsigned c = 0; c < 4; ++c) temps[r].c[c] |= o.temps[r].c[c];
            indexable |= o.indexable;
            for (unsigned c = 0; c < 4; ++c) pos[c] |= o.pos[c];
        }
    };
    State cur;
    std::vector<unsigned> sysInputs;   // input registers the input assembler generates (vertex and instance ids)
    unsigned writes[4] = {};
    bool wOne = false, hasLoop = false;
    for (const auto& inst : shList)
        if (inst.getOperation() && inst.getOperation()->getType() == D3D10_SB_OPCODE_LOOP) hasLoop = true;
    auto componentOf = [](const ShaderOperand* s, unsigned c) -> unsigned
    {
        if (s->getOperandNumComponents() != D3D10_SB_OPERAND_4_COMPONENT) return 0;
        const unsigned mode = s->getOperand4ComponentSelectionMode();
        if (mode == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE)
            return s->getOperand4ComponentSwizzleSource((D3D10_SB_4_COMPONENT_NAME)c);
        if (mode == D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE) return s->getOperand4ComponentSelect1();
        return c;
    };
    auto traceOf = [&](const ShaderOperand* s, unsigned c) -> unsigned char
    {
        switch (s->getOperandType())
        {
        case D3D10_SB_OPERAND_TYPE_IMMEDIATE32: case D3D10_SB_OPERAND_TYPE_IMMEDIATE64:
        case D3D10_SB_OPERAND_TYPE_RESOURCE: case D3D10_SB_OPERAND_TYPE_SAMPLER:
        case D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW: case D3D11_SB_OPERAND_TYPE_THREAD_GROUP_SHARED_MEMORY:
            return 0;
        case D3D10_SB_OPERAND_TYPE_TEMP:
        {
            const unsigned r = s->getIndex(0);
            return r < cur.temps.size() ? cur.temps[r].c[componentOf(s, c) & 3] : 0;
        }
        case D3D10_SB_OPERAND_TYPE_INDEXABLE_TEMP: return cur.indexable;
        case D3D10_SB_OPERAND_TYPE_INPUT:
            return std::find(sysInputs.begin(), sysInputs.end(), s->getIndex(0)) != sysInputs.end() ? 1 : 2;
        default: return 1;   // constant buffers, the immediate table, system values
        }
    };
    // Branches run from the state at their if and merge at the endif; in a loop a write only adds, and only a loop earns a
    // second pass, since one seeded from the end's values would mix later constant-buffer terms into earlier writes.
    for (int pass = 0; pass < (hasLoop ? 2 : 1); ++pass)
    {
        unsigned loops = 0;
        std::vector<State> atIf, thenDone;   // per open if: the state before it, and its then-branch's result once in else
        std::vector<bool> inElse;
        for (const auto& inst : shList)
        {
            const shader_analyzer::ShaderOperation* op = inst.getOperation();
            if (!op || op->isCustomData()) continue;
            const unsigned code = op->getType();
            const unsigned n = (unsigned)inst.getOperandsCount();
            if (op->isDeclaration())
            {
                // Its operand is the token after the opcode (declarations are not parsed into operands).
                if (pass == 0 && (code == D3D10_SB_OPCODE_DCL_INPUT_SGV || code == D3D10_SB_OPCODE_DCL_INPUT_SIV) &&
                    inst.GetData().size() >= 3)
                    sysInputs.push_back(shader_analyzer::ShaderOperand(const_cast<UINT*>(&inst.GetData()[1])).getIndex(0));
                continue;
            }
            if (code == D3D10_SB_OPCODE_IF) { atIf.push_back(cur); thenDone.push_back(State()); inElse.push_back(false); continue; }
            if (code == D3D10_SB_OPCODE_ELSE)
            {
                if (!atIf.empty()) { thenDone.back() = cur; cur = atIf.back(); inElse.back() = true; }
                continue;
            }
            if (code == D3D10_SB_OPCODE_ENDIF)
            {
                if (!atIf.empty())
                {
                    cur.Merge(inElse.back() ? thenDone.back() : atIf.back());   // without an else, the branch may not run
                    atIf.pop_back(); thenDone.pop_back(); inElse.pop_back();
                }
                continue;
            }
            if (code == D3D10_SB_OPCODE_LOOP || code == D3D10_SB_OPCODE_SWITCH) { ++loops; continue; }
            if (code == D3D10_SB_OPCODE_ENDLOOP || code == D3D10_SB_OPCODE_ENDSWITCH) { if (loops) --loops; continue; }
            if (code == D3D10_SB_OPCODE_BREAKC || code == D3D10_SB_OPCODE_CONTINUEC || code == D3D10_SB_OPCODE_RETC ||
                code == D3D10_SB_OPCODE_DISCARD || code == D3D10_SB_OPCODE_CASE || code == D3D10_SB_OPCODE_CALLC)
                continue;
            const unsigned dsts = (code == D3D10_SB_OPCODE_SINCOS || code == D3D10_SB_OPCODE_UDIV || code == D3D10_SB_OPCODE_IMUL ||
                                   code == D3D10_SB_OPCODE_UMUL || code == D3D11_SB_OPCODE_SWAPC ||
                                   code == D3D11_SB_OPCODE_UADDC || code == D3D11_SB_OPCODE_USUBB) ? 2u : 1u;
            if (n <= dsts) continue;
            // Dot products and resource reads mix their sources' components; everything else maps component to component.
            bool mixes = code == D3D10_SB_OPCODE_DP2 || code == D3D10_SB_OPCODE_DP3 || code == D3D10_SB_OPCODE_DP4;
            for (unsigned i = dsts; i < n && !mixes; ++i)
            {
                const unsigned t = inst.getOperand(i) ? inst.getOperand(i)->getOperandType() : 0;
                mixes = t == D3D10_SB_OPERAND_TYPE_RESOURCE || t == D3D10_SB_OPERAND_TYPE_SAMPLER ||
                        t == D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW || t == D3D11_SB_OPERAND_TYPE_THREAD_GROUP_SHARED_MEMORY;
            }
            unsigned char value[4] = {};
            for (unsigned i = dsts; i < n; ++i)
            {
                const ShaderOperand* s = inst.getOperand(i);
                if (!s) continue;
                unsigned char all = 0;
                if (mixes) for (unsigned c = 0; c < 4; ++c) all |= traceOf(s, c);
                for (unsigned c = 0; c < 4; ++c) value[c] |= mixes ? all : traceOf(s, c);
            }
            for (unsigned d = 0; d < dsts; ++d)
            {
                const ShaderOperand* dst = inst.getOperand(d);
                if (!dst) continue;
                const unsigned t = dst->getOperandType();
                const unsigned mask = dst->getComponentState();
                if (t == D3D10_SB_OPERAND_TYPE_INDEXABLE_TEMP)
                {
                    for (unsigned c = 0; c < 4; ++c) if (mask & (1u << c)) cur.indexable |= value[c];
                    continue;
                }
                unsigned char* to = nullptr;
                if (t == D3D10_SB_OPERAND_TYPE_TEMP)
                {
                    const unsigned r = dst->getIndex(0);
                    if (r >= cur.temps.size()) cur.temps.resize(r + 1, Temp{});
                    to = cur.temps[r].c;
                }
                else if (t == D3D10_SB_OPERAND_TYPE_OUTPUT && dst->getIndex(0) == posReg)
                {
                    to = cur.pos;
                    if (pass == 0)
                    {
                        for (unsigned c = 0; c < 4; ++c) if (mask & (1u << c)) ++writes[c];
                        // w from a literal 1.0 (the literal's dwords follow the operand tokens).
                        const ShaderOperand* src = inst.getOperand(1);
                        if (code == D3D10_SB_OPCODE_MOV && n == 2 && (mask & ShaderOperand::W_STATE) && src &&
                            src->getOperandType() == D3D10_SB_OPERAND_TYPE_IMMEDIATE32)
                        {
                            const std::vector<unsigned>& data = inst.GetData();
                            const size_t at = (size_t)op->getLength() + dst->getLength() + (src->isOperandExtended() ? 2 : 1);
                            const size_t k = src->getOperandNumComponents() == D3D10_SB_OPERAND_1_COMPONENT ? at : at + 3;
                            wOne = k < data.size() && data[k] == 0x3F800000u;
                        }
                    }
                }
                if (!to) continue;
                for (unsigned c = 0; c < 4; ++c)
                    if (mask & (1u << c)) to[c] = loops ? (unsigned char)(to[c] | value[c]) : value[c];
            }
        }
    }
    return writes[0] == 1 && writes[1] == 1 && writes[2] == 1 && writes[3] == 1 && wOne &&
           cur.pos[0] == 2 && cur.pos[1] == 2 && cur.pos[2] == 2;
}

// A sprite's world position and the camera it is measured from: an xyz add of cbN[k] and vI, or of a temp a mov filled from
// vI. false = none.
bool SpriteWorldPosition(const shader_analyzer::TShaderList& shList, int& input, int& cbSlot, UINT& cbReg)
{
    using shader_analyzer::ShaderOperand;
    auto xyzInOrder = [](const ShaderOperand* s)
    {
        return s->getOperand4ComponentSelectionMode() == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE &&
               s->getOperand4ComponentSwizzleSource(D3D10_SB_4_COMPONENT_X) == D3D10_SB_4_COMPONENT_X &&
               s->getOperand4ComponentSwizzleSource(D3D10_SB_4_COMPONENT_Y) == D3D10_SB_4_COMPONENT_Y &&
               s->getOperand4ComponentSwizzleSource(D3D10_SB_4_COMPONENT_Z) == D3D10_SB_4_COMPONENT_Z;
    };
    std::vector<std::pair<unsigned, unsigned>> fromInput;   // (temp, input) of mov rT.xyz, vI.xyz
    for (const auto& inst : shList)
    {
        const shader_analyzer::ShaderOperation* op = inst.getOperation();
        if (!op || op->isDeclaration() || op->isCustomData()) continue;
        const unsigned n = (unsigned)inst.getOperandsCount();
        const ShaderOperand* d = n ? inst.getOperand(0) : nullptr;
        if (!d || d->getOperandType() != D3D10_SB_OPERAND_TYPE_TEMP || (d->getComponentState() & 7) != 7) continue;
        if (op->getType() == D3D10_SB_OPCODE_MOV && n == 2)
        {
            const ShaderOperand* s = inst.getOperand(1);
            if (s && s->getOperandType() == D3D10_SB_OPERAND_TYPE_INPUT && xyzInOrder(s))
                fromInput.push_back(std::make_pair(d->getIndex(0), s->getIndex(0)));
            continue;
        }
        if (op->getType() != D3D10_SB_OPCODE_ADD || n != 3) continue;
        for (unsigned i = 1; i < 3; ++i)
        {
            const ShaderOperand* cb = inst.getOperand(i);
            const ShaderOperand* v = inst.getOperand(3 - i);
            if (!cb || !v || cb->getOperandType() != D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER ||
                cb->getOperandIndexDimension() != D3D10_SB_OPERAND_INDEX_2D ||
                cb->getOperandIndexRepresentation((D3D10_SB_OPERAND_INDEX_DIMENSION)1) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
                !xyzInOrder(cb) || !xyzInOrder(v))
                continue;
            int in = -1;
            if (v->getOperandType() == D3D10_SB_OPERAND_TYPE_INPUT) in = (int)v->getIndex(0);
            else if (v->getOperandType() == D3D10_SB_OPERAND_TYPE_TEMP)
                for (const auto& m : fromInput) if (m.first == v->getIndex(0)) in = (int)m.second;
            if (in < 0) continue;
            input = in; cbSlot = (int)cb->getIndex(0); cbReg = cb->getIndex(1);
            return true;
        }
    }
    return false;
}

// Output register reg written only by movs from inputs, x y z each once: the vertex buffer's own value passes through.
bool OutputPassedThrough(const shader_analyzer::TShaderList& shList, unsigned reg)
{
    using shader_analyzer::ShaderOperand;
    unsigned fromInput = 0, writes[4] = {};
    for (const auto& inst : shList)
    {
        const shader_analyzer::ShaderOperation* op = inst.getOperation();
        if (!op || op->isDeclaration() || op->isCustomData() || inst.getOperandsCount() < 1) continue;
        const ShaderOperand* dst = inst.getOperand(0);
        if (!dst || dst->getOperandType() != D3D10_SB_OPERAND_TYPE_OUTPUT || dst->getIndex(0) != reg) continue;
        const unsigned mask = dst->getComponentState();
        for (unsigned c = 0; c < 4; ++c) if (mask & (1u << c)) ++writes[c];
        if (op->getType() != D3D10_SB_OPCODE_MOV || inst.getOperandsCount() != 2) continue;
        const ShaderOperand* src = inst.getOperand(1);
        if (src && src->getOperandType() == D3D10_SB_OPERAND_TYPE_INPUT) fromInput |= mask;
    }
    const unsigned xyz = ShaderOperand::X_STATE | ShaderOperand::Y_STATE | ShaderOperand::Z_STATE;
    return writes[0] == 1 && writes[1] == 1 && writes[2] == 1 && (fromInput & xyz) == xyz;
}

// Outputs other than posReg whose four components are each a dp4 with cbN[k + component]: a float4x4 at (N, k * 16), as
// last frame's transform for velocity. posToo (no projection found): the position too, its z allowed apart.
void Dp4OutputMatrices(const shader_analyzer::TShaderList& shList, unsigned posReg, bool posToo,
                       std::vector<std::pair<int, UINT>>& out)
{
    using shader_analyzer::ShaderOperand;
    struct Comp { unsigned writes; int slot; unsigned reg; };
    Comp regs[32][4] = {};
    for (auto& r : regs) for (auto& c : r) c.slot = -1;
    for (const auto& inst : shList)
    {
        const shader_analyzer::ShaderOperation* op = inst.getOperation();
        if (!op || op->isDeclaration() || op->isCustomData() || inst.getOperandsCount() < 1) continue;
        const ShaderOperand* dst = inst.getOperand(0);
        if (!dst || dst->getOperandType() != D3D10_SB_OPERAND_TYPE_OUTPUT) continue;
        const unsigned reg = dst->getIndex(0);
        if (reg >= 32 || (reg == posReg && !posToo)) continue;
        const unsigned mask = dst->getComponentState();
        int comp = -1;
        for (unsigned c = 0; c < 4; ++c)
            if (mask & (1u << c)) { ++regs[reg][c].writes; comp = comp < 0 ? (int)c : 4; }
        if (comp < 0 || comp > 3 || op->getType() != D3D10_SB_OPCODE_DP4 || inst.getOperandsCount() != 3) continue;
        for (unsigned i = 1; i < 3; ++i)
        {
            const ShaderOperand* src = inst.getOperand(i);
            if (!src || src->getOperandType() != D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER ||
                src->getOperandIndexDimension() != D3D10_SB_OPERAND_INDEX_2D ||
                src->getOperandIndexRepresentation((D3D10_SB_OPERAND_INDEX_DIMENSION)0) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
                src->getOperandIndexRepresentation((D3D10_SB_OPERAND_INDEX_DIMENSION)1) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
                src->getOperand4ComponentSelectionMode() != D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE ||
                src->getOperand4ComponentSwizzle() != D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE)
                continue;
            regs[reg][comp].slot = (int)src->getIndex(0);
            regs[reg][comp].reg = src->getIndex(1);
        }
    }
    for (unsigned o = 0; o < 32; ++o)
    {
        const Comp* r = regs[o];
        bool ok = true;
        for (unsigned c = 0; c < 4 && ok; ++c)
            if (o != posReg || c != 2 || r[c].slot >= 0)
                ok = r[c].writes == 1 && r[c].slot >= 0 && r[c].slot == r[0].slot && r[c].reg == r[0].reg + c;
            else
                ok = r[c].writes == 1;
        if (ok) out.push_back(std::make_pair(r[0].slot, r[0].reg * 16u));
    }
}

void AddOnce(std::vector<std::pair<int, UINT>>& v, int slot, UINT off)
{
    for (const auto& m : v) if (m.first == slot && m.second == off) return;
    v.push_back(std::make_pair(slot, off));
}

// A shader without RDEF: float4x4s read as four nearby dp4 of one vector with cbN[k..k + 3], or a dp4 picking its row at
// run time (cbN[r + k]); 3x4 views read as mul/mad of cbN[k..k + 2] by one vector's x, y and z.
void CodeMatrices(const shader_analyzer::TShaderList& shList, std::vector<std::pair<int, UINT>>& mats,
                  std::vector<std::pair<int, UINT>>& views, std::vector<std::pair<int, UINT>>* pixels = nullptr)
{
    using shader_analyzer::ShaderOperand;
    // A cb register a dp4 reads beside a vector, with the vector's selector.
    struct Use { size_t at; unsigned type, index, mode, sel; int slot; unsigned reg; };
    std::vector<Use> uses;
    // A cb register a mul or mad scales by one replicated component of a vector.
    struct Column { size_t at; unsigned type, index, comp; int slot; unsigned reg; };
    std::vector<Column> columns;
    size_t at = 0;
    for (const auto& inst : shList)
    {
        ++at;
        const shader_analyzer::ShaderOperation* op = inst.getOperation();
        if (!op || op->isDeclaration() || op->isCustomData()) continue;
        if ((op->getType() == D3D10_SB_OPCODE_MUL && inst.getOperandsCount() == 3) ||
            (op->getType() == D3D10_SB_OPCODE_MAD && inst.getOperandsCount() == 4))
        {
            for (unsigned i = 1; i < 3; ++i)
            {
                const ShaderOperand* cb = inst.getOperand(i);
                const ShaderOperand* src = inst.getOperand(3 - i);
                if (!cb || !src || cb->getOperandType() != D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER ||
                    cb->getOperandIndexDimension() != D3D10_SB_OPERAND_INDEX_2D ||
                    cb->getOperandIndexRepresentation((D3D10_SB_OPERAND_INDEX_DIMENSION)0) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
                    cb->getOperandIndexRepresentation((D3D10_SB_OPERAND_INDEX_DIMENSION)1) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
                    continue;
                const unsigned type = src->getOperandType();
                if ((type != D3D10_SB_OPERAND_TYPE_TEMP && type != D3D10_SB_OPERAND_TYPE_INPUT) ||
                    src->getOperandIndexDimension() != D3D10_SB_OPERAND_INDEX_1D)
                    continue;
                unsigned comp;
                const unsigned mode = src->getOperand4ComponentSelectionMode();
                if (mode == D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE) comp = src->getOperand4ComponentSelect1();
                else if (mode == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE)
                {
                    comp = src->getOperand4ComponentSwizzleSource(D3D10_SB_4_COMPONENT_X);
                    bool same = true;
                    for (unsigned c = 1; c < 4; ++c)
                        same &= src->getOperand4ComponentSwizzleSource((D3D10_SB_4_COMPONENT_NAME)c) == comp;
                    if (!same) continue;
                }
                else continue;
                columns.push_back({ at, type, src->getIndex(0), comp, (int)cb->getIndex(0), cb->getIndex(1) });
                break;
            }
            continue;
        }
        if (op->getType() != D3D10_SB_OPCODE_DP4 || inst.getOperandsCount() != 3) continue;
        for (unsigned i = 1; i < 3; ++i)
        {
            const ShaderOperand* cb = inst.getOperand(i);
            const ShaderOperand* src = inst.getOperand(3 - i);
            if (!cb || !src || cb->getOperandType() != D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER ||
                cb->getOperandIndexDimension() != D3D10_SB_OPERAND_INDEX_2D ||
                cb->getOperandIndexRepresentation((D3D10_SB_OPERAND_INDEX_DIMENSION)0) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
                continue;
            const unsigned rep = cb->getOperandIndexRepresentation((D3D10_SB_OPERAND_INDEX_DIMENSION)1);
            const int slot = (int)cb->getIndex(0);
            const unsigned reg = cb->getIndex(1);
            // A row picked at run time; a bare relative index (cb0[r2.w + 0] in the listing) starts at 0.
            if (rep == D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE) { AddOnce(mats, slot, reg * 16u); break; }
            if (rep == D3D10_SB_OPERAND_INDEX_RELATIVE) { AddOnce(mats, slot, 0u); break; }
            const unsigned type = src->getOperandType();
            if (rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
                (type != D3D10_SB_OPERAND_TYPE_TEMP && type != D3D10_SB_OPERAND_TYPE_INPUT) ||
                src->getOperandIndexDimension() != D3D10_SB_OPERAND_INDEX_1D ||
                cb->getOperand4ComponentSelectionMode() != D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE ||
                cb->getOperand4ComponentSwizzle() != D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE)
                continue;
            const unsigned mode = src->getOperand4ComponentSelectionMode();
            const unsigned sel = mode == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE ? src->getOperand4ComponentSwizzle()
                                                                                   : src->getOperand4ComponentMask();
            uses.push_back({ at, type, src->getIndex(0), mode, sel, slot, reg });
            break;
        }
    }
    for (const auto& u : uses)
    {
        // u starts four rows: cbN[k + 1..3] read beside the same vector nearby, as bits 1..3 (bit 0 is u's own).
        unsigned have = 0;
        for (const auto& r : uses)
            if ((r.at > u.at ? r.at - u.at : u.at - r.at) <= 12 && r.type == u.type && r.index == u.index &&
                r.mode == u.mode && r.sel == u.sel && r.slot == u.slot && r.reg >= u.reg && r.reg <= u.reg + 3)
                have |= 1u << (r.reg - u.reg);
        if (have == 0xF) AddOnce(mats, u.slot, u.reg * 16u);
        // Rows x, y and w alone: a pixel position (x * w, y * w, w) to divide.
        else if (pixels && have == 0xB) AddOnce(*pixels, u.slot, u.reg * 16u);
    }
    for (const auto& u : columns)
    {
        // u is column x: cbN[k + 1] scales the same vector's y and cbN[k + 2] its z nearby (bits 0..2).
        if (u.comp != 0) continue;
        unsigned have = 0;
        for (const auto& r : columns)
            if ((r.at > u.at ? r.at - u.at : u.at - r.at) <= 12 && r.type == u.type && r.index == u.index &&
                r.slot == u.slot && r.reg >= u.reg && r.reg <= u.reg + 2 && r.comp == r.reg - u.reg)
                have |= 1u << r.comp;
        if (have == 0x7) AddOnce(views, u.slot, u.reg * 16u);
    }
}

// The constant register an operand reads at an immediate index: (slot, register); a run-time index reads ~0u.
bool ConstantOperand(const shader_analyzer::ShaderOperand* o, int& slot, unsigned& reg)
{
    if (!o || o->getOperandType() != D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER ||
        o->getOperandIndexDimension() != D3D10_SB_OPERAND_INDEX_2D ||
        o->getOperandIndexRepresentation((D3D10_SB_OPERAND_INDEX_DIMENSION)0) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
        return false;
    slot = (int)o->getIndex(0);
    reg = o->getOperandIndexRepresentation((D3D10_SB_OPERAND_INDEX_DIMENSION)1) == D3D10_SB_OPERAND_INDEX_IMMEDIATE32
        ? o->getIndex(1) : ~0u;
    return true;
}

// A shader without RDEF: matrices read as (x*W, y*W, W, 1), cbN[k] and cbN[k + 1] scaling one vector's x and y,
// cbN[k + 2] a scalar and cbN[k + 3] added (UE4's ScreenToShadowMatrix and ScreenToTranslatedWorld).
void ScreenForm(const shader_analyzer::TShaderList& shList, std::vector<std::pair<int, UINT>>& out)
{
    using shader_analyzer::ShaderOperand;
    struct Column { size_t at; unsigned type, index, comp; int slot; unsigned reg; };
    struct Added  { size_t at; int slot; unsigned reg; };
    std::vector<Column> columns;
    std::vector<Added>  adds;
    size_t at = 0;
    for (const auto& inst : shList)
    {
        ++at;
        const shader_analyzer::ShaderOperation* op = inst.getOperation();
        if (!op || op->isDeclaration() || op->isCustomData()) continue;
        const unsigned n = (unsigned)inst.getOperandsCount();
        const bool mul = op->getType() == D3D10_SB_OPCODE_MUL && n == 3, mad = op->getType() == D3D10_SB_OPCODE_MAD && n == 4;
        int slot;
        unsigned reg;
        if (op->getType() == D3D10_SB_OPCODE_ADD && n == 3)
        {
            for (unsigned i = 1; i < 3; ++i)
                if (ConstantOperand(inst.getOperand(i), slot, reg) && reg != ~0u) adds.push_back({ at, slot, reg });
            continue;
        }
        if (!mul && !mad) continue;
        if (mad && ConstantOperand(inst.getOperand(3), slot, reg) && reg != ~0u) adds.push_back({ at, slot, reg });
        for (unsigned i = 1; i < 3; ++i)
        {
            const ShaderOperand* src = inst.getOperand(3 - i);
            if (!ConstantOperand(inst.getOperand(i), slot, reg) || reg == ~0u || !src) continue;
            const unsigned type = src->getOperandType();
            if ((type != D3D10_SB_OPERAND_TYPE_TEMP && type != D3D10_SB_OPERAND_TYPE_INPUT) ||
                src->getOperandIndexDimension() != D3D10_SB_OPERAND_INDEX_1D)
                continue;
            unsigned comp;
            const unsigned mode = src->getOperand4ComponentSelectionMode();
            if (mode == D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE) comp = src->getOperand4ComponentSelect1();
            else if (mode == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE)
            {
                comp = src->getOperand4ComponentSwizzleSource(D3D10_SB_4_COMPONENT_X);
                bool same = true;
                for (unsigned c = 1; c < 4; ++c)
                    same &= src->getOperand4ComponentSwizzleSource((D3D10_SB_4_COMPONENT_NAME)c) == comp;
                if (!same) continue;
            }
            else continue;
            columns.push_back({ at, type, src->getIndex(0), comp, slot, reg });
            break;
        }
    }
    auto near12 = [](size_t a, size_t b) { return (a > b ? a - b : b - a) <= 12; };
    for (const auto& u : columns)
    {
        if (u.comp != 0) continue;
        bool y = false, s = false, w = false;
        for (const auto& r : columns)
        {
            if (!near12(r.at, u.at) || r.slot != u.slot) continue;
            y |= r.reg == u.reg + 1 && r.type == u.type && r.index == u.index && r.comp == 1;
            s |= r.reg == u.reg + 2;
        }
        for (const auto& a : adds) w |= near12(a.at, u.at) && a.slot == u.slot && a.reg == u.reg + 3;
        if (y && s && w) AddOnce(out, u.slot, u.reg * 16u);
    }
}

// Every constant register the code reads: (slot, register), ~0u for a slot it indexes at run time.
void ConstantReads(const shader_analyzer::TShaderList& shList, std::vector<std::pair<int, UINT>>& out)
{
    for (const auto& inst : shList)
    {
        const shader_analyzer::ShaderOperation* op = inst.getOperation();
        if (!op || op->isDeclaration() || op->isCustomData()) continue;
        for (unsigned i = 1; i < (unsigned)inst.getOperandsCount(); ++i)
        {
            int slot;
            unsigned reg;
            if (ConstantOperand(inst.getOperand(i), slot, reg)) AddOnce(out, slot, reg);
        }
    }
}

// The constant registers a dp4 reads whole: (slot, register).
void Dp4ConstantReads(const shader_analyzer::TShaderList& shList, std::vector<std::pair<int, UINT>>& out)
{
    for (const auto& inst : shList)
    {
        const shader_analyzer::ShaderOperation* op = inst.getOperation();
        if (!op || op->isDeclaration() || op->isCustomData() || op->getType() != D3D10_SB_OPCODE_DP4) continue;
        for (unsigned i = 1; i < (unsigned)inst.getOperandsCount(); ++i)
        {
            int slot;
            unsigned reg;
            if (ConstantOperand(inst.getOperand(i), slot, reg) && reg != ~0u) AddOnce(out, slot, reg);
        }
    }
}

bool ParseCode(const void* pBytecode, SIZE_T byteLength, shader_analyzer::TShaderList& shList)
{
    DWORD size = 0;
    const BYTE* code = FindChunk(static_cast<const BYTE*>(pBytecode), byteLength, kSHEX, size);
    if (!code) code = FindChunk(static_cast<const BYTE*>(pBytecode), byteLength, kSHDR, size);
    if (!code) return false;
    shader_analyzer::ParseShader(reinterpret_cast<const unsigned*>(code), shList);
    return true;
}

} // anonymous namespace

namespace wiz3d
{

const BYTE* FindDxbcChunk(const void* blob, SIZE_T blobLen, DWORD tag, DWORD& outSize)
{
    return FindChunk(static_cast<const BYTE*>(blob), blobLen, tag, outSize);
}

// 0.4.1 ShaderOverride: expose the CRC32 the analyser uses so Device11Proxy
// can look up a profile entry by CRC before CreateShader runs — a shader
// with an inline <ShaderOverride> replaces its bytecode in-place and the
// rest of the pipeline analyses the replacement, not the game's original.
DWORD ShaderBytecodeCRC32(const void* pBytecode, SIZE_T byteLength)
{
    if (!pBytecode || byteLength == 0) return 0;
    return ComputeCRC32(static_cast<const BYTE*>(pBytecode), byteLength);
}

void CodeMatrixReads(const void* pBytecode, SIZE_T byteLength, std::vector<std::pair<int, UINT>>& mats,
                     std::vector<std::pair<int, UINT>>& views)
{
    DWORD size = 0;
    const BYTE* code = FindChunk(static_cast<const BYTE*>(pBytecode), byteLength, kSHEX, size);
    if (!code) code = FindChunk(static_cast<const BYTE*>(pBytecode), byteLength, kSHDR, size);
    if (!code) return;
    shader_analyzer::TShaderList shList;
    shader_analyzer::ParseShader(reinterpret_cast<const unsigned*>(code), shList);
    CodeMatrices(shList, mats, views);
}

void CodePixelMatrices(const void* pBytecode, SIZE_T byteLength, std::vector<std::pair<int, UINT>>& out)
{
    shader_analyzer::TShaderList shList;
    std::vector<std::pair<int, UINT>> mats, views;
    if (pBytecode && ParseCode(pBytecode, byteLength, shList)) CodeMatrices(shList, mats, views, &out);
}

void CodeScreenMatrices(const void* pBytecode, SIZE_T byteLength, std::vector<std::pair<int, UINT>>& out)
{
    shader_analyzer::TShaderList shList;
    if (pBytecode && ParseCode(pBytecode, byteLength, shList)) ScreenForm(shList, out);
}

void CodeConstantReads(const void* pBytecode, SIZE_T byteLength, std::vector<std::pair<int, UINT>>& out)
{
    shader_analyzer::TShaderList shList;
    if (pBytecode && ParseCode(pBytecode, byteLength, shList)) ConstantReads(shList, out);
}

void CodeDp4ConstantReads(const void* pBytecode, SIZE_T byteLength, std::vector<std::pair<int, UINT>>& out)
{
    shader_analyzer::TShaderList shList;
    if (pBytecode && ParseCode(pBytecode, byteLength, shList)) Dp4ConstantReads(shList, out);
}

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
    if (!ParseOutputSignature(osg, osgSize, osgTag, outputEntries, out.outputs))
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
    Dp4OutputMatrices(shList, pOutPos->Register, out.projection.matrixData.cb.empty(), out.dp4OutputMatrices);
    out.clipSpacePosition = out.projection.matrixData.cb.empty() && PositionPassedThrough(shList, pOutPos->Register);
    out.clipSpaceBuilt = out.projection.matrixData.cb.empty() && PositionBuiltFromInputs(shList, pOutPos->Register);
    if (out.clipSpaceBuilt && !SpriteWorldPosition(shList, out.spriteWorldInput, out.spriteCameraCB, out.spriteCameraReg))
        out.spriteWorldInput = out.spriteCameraCB = -1;
    // The ray attribute: the first float3 (mask xyz, float) copied from an input beside a pass-through position.
    if (out.clipSpacePosition)
        for (size_t i = 0; i < out.outputs.size() && out.rayOutput < 0; ++i)
        {
            const ShaderAnalysis11Result::OutputElement& e = out.outputs[i];
            if (e.sysValue == 0 && e.mask == 0x7 && e.type == 3 && OutputPassedThrough(shList, e.reg)) out.rayOutput = (int)i;
        }

    out.parsed = true;
    return true;
}

} // namespace wiz3d
