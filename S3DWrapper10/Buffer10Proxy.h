/* wiz3D - ID3D10Buffer proxy
 *
 * A constant or structured buffer's right-eye copy, and the game's bytes + matrix
 * targets its right constants come from. Mirrors the DX11 Buffer11Proxy member/API
 * shape so the DX11 feature engine + <ShaderPatch> primitive engine can be ported
 * to DX10 without diverging data models (task #62 "Option A").
 *
 * Coexistence: the 0.4.0-era Option B frame-replay path (Map/Unmap + PushFrameCommand)
 * remains live for basic CB eye-shift. The Option A right-sibling + per-draw edit
 * infrastructure added here is the storage; Device10Proxy's EditRightCBs wiring
 * (Cycle 2) is what actually consumes it. Until then these fields stay dormant,
 * so no existing DX10 behaviour changes with this commit.
 *
 * Ring CB support (D3D11.1-only) is omitted — DX10 has no ring constant buffers.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d10.h>
#include <vector>
#include <bitset>

namespace wiz3d
{

class Device10Proxy;

class Buffer10Proxy : public ID3D10Buffer
{
public:
    Buffer10Proxy(ID3D10Buffer* real, Device10Proxy* parent);
    virtual ~Buffer10Proxy();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef() override                  { return InterlockedIncrement(&m_refs); }
    ULONG   STDMETHODCALLTYPE Release() override;

    void    STDMETHODCALLTYPE GetDevice(ID3D10Device** ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override                  { return m_real->GetPrivateData(guid, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override              { return m_real->SetPrivateData(guid, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override                { return m_real->SetPrivateDataInterface(guid, pData); }

    void    STDMETHODCALLTYPE GetType(D3D10_RESOURCE_DIMENSION* pResourceDimension) override                       { m_real->GetType(pResourceDimension); }
    void    STDMETHODCALLTYPE SetEvictionPriority(UINT EvictionPriority) override                                  { m_real->SetEvictionPriority(EvictionPriority); }
    UINT    STDMETHODCALLTYPE GetEvictionPriority() override                                                       { return m_real->GetEvictionPriority(); }

    HRESULT STDMETHODCALLTYPE Map(D3D10_MAP MapType, UINT MapFlags, void** ppData) override;
    void    STDMETHODCALLTYPE Unmap() override;
    void    STDMETHODCALLTYPE GetDesc(D3D10_BUFFER_DESC* pDesc) override                                           { m_real->GetDesc(pDesc); }

    // wiz3D accessors
    ID3D10Buffer*  GetReal()       const { return m_real;   }
    Device10Proxy* GetParent()     const { return m_parent; }

    // Right-eye CB sibling so draw duplication switches eyes by rebinding
    // rather than rewriting a shared buffer. Null unless the sibling has been
    // created. Lazy-created via RightCopy() on first use.
    ID3D10Buffer*  GetRealRight()  const { return m_realRight; }
    void           SetRealRight(ID3D10Buffer* p) { m_realRight = p; }
    // The right copy a pass reads (Device10Proxy::EditRightCBs when Cycle 2 lands):
    // 0 = the sibling above, 1 = a second sibling for passes that see the right
    // camera with its origin moved, made on first use with the same desc.
    ID3D10Buffer*  RightCopy(int variant);

    // Sticky VS-bind tag — set when *SetConstantBuffers fires on the vertex
    // pipeline. Once set, never cleared: the buffer stays eligible for stereo
    // math for its lifetime. Preserved from Option B.
    bool IsVSBound() const { return m_vsBound; }
    void TagVSBound()      { m_vsBound = true; }

    // When a matrix in this buffer was last eye-shifted, for the frame
    // report's uncorrected-draw counts.
    bool     EverShifted() const        { return m_everShifted; }
    void     MarkShifted(unsigned frame) { m_everShifted = true; m_shiftedFrame = frame; }
    unsigned ShiftedFrame() const        { return m_shiftedFrame; }

    // Matrix registers learned for this buffer: a game often writes a CB before
    // binding its consumer, and a register holding a view-projection holds one
    // whoever is bound. 24, as UE3's view buffer holds a dozen projective matrices.
    struct MatrixTarget { DWORD reg; BOOL transposed; BOOL inverse; };
    const std::vector<MatrixTarget>& LearnedMatrices() const { return m_learned; }
    void LearnMatrix(const MatrixTarget& t)
    {
        for (size_t i = 0; i < m_learned.size(); ++i)
            if (m_learned[i].reg == t.reg) return;
        if (m_learned.size() < 24) m_learned.push_back(t);
    }

    // Last bytes written + counter that changes with them: patching at draw time
    // needs the current contents and a way to tell whether the sibling is already
    // correct for the shader about to read it.
    void SetShadow(const unsigned char* p, size_t n)
    {
        if (!p || !n || n > kMaxShadowBytes) return;
        m_shadow.assign(p, p + n);
        ++m_writeSeq;
    }
    const std::vector<unsigned char>& Shadow() const { return m_shadow; }
    // A copy or a boxed write filled the buffer behind the shadow's back:
    // nothing may be rebuilt from it until the next whole write.
    void ForgetShadow() { m_shadow.clear(); ++m_writeSeq; }
    // The shift is part of the key, as the eye offset is baked into the bytes:
    // a buffer the game stops rewriting would otherwise keep an old separation.
    bool NeedsDrawPatch(DWORD crc, float shift) const
    { return m_patchedSeq != m_writeSeq || m_patchedCRC != crc || m_patchedShift != shift; }
    void MarkDrawPatched(DWORD crc, float shift)
    { m_patchedSeq = m_writeSeq; m_patchedCRC = crc; m_patchedShift = shift; }
    static const size_t kMaxShadowBytes = 64 * 1024;

    unsigned WriteSeq() const { return m_writeSeq; }

    // What the right sibling holds beyond the shadow bytes: forward (geometry)
    // shifts and inverse patches applied to them; plain shadow is neither.
    // A copy into the buffer counts as unknown, i.e. shifted.
    void SetRightFromShadow(bool v) { m_rightForward = v ? 0 : 1; m_rightInverse = 0; }
    void NoteRightShift(unsigned forward, unsigned inverse) { m_rightForward = forward; m_rightInverse = inverse; }
    bool RightIsShadow() const       { return !m_rightForward && !m_rightInverse; }
    bool RightForwardShifted() const { return m_rightForward != 0; }
    // A shader's camera matrices are rebuilt from the shadow once per write
    // and stereo setting; a later rewrite of the sibling by another path asks
    // for the check again.
    bool InverseDrawCheckDue(float shift, float conv) const
    { return m_invCheckSeq != m_writeSeq || m_invCheckShift != shift || m_invCheckConv != conv; }
    void MarkInverseDrawChecked(float shift, float conv)
    { m_invCheckSeq = m_writeSeq; m_invCheckShift = shift; m_invCheckConv = conv; }
    void InvalidateInverseDrawCheck() { m_invCheckSeq = 0xFFFFFFFFu; }

    // Registers vertex shaders read as their position matrix, by pass.
    // One read only in shadow passes is a light's view (MP3's ShadowParam.gShadowMatrix),
    // the same for both eyes.
    void NoteMatrixUse(DWORD reg, bool shadowPass) { if (reg < 256) { if (shadowPass) m_lightRegs.set(reg); else m_cameraRegs.set(reg); } }
    bool LightOnlyMatrix(DWORD reg) const { return reg < 256 && m_lightRegs.test(reg) && !m_cameraRegs.test(reg); }

    // Every write to the right sibling counts, so a feature's draw-time
    // rewrite knows when its bytes are still in place (pooled buffers change
    // content between draws).
    void NoteRightWrite() { ++m_rightSeq; }
    // The features' edit of a right copy (Device10Proxy::EditRightCBs when
    // Cycle 2 lands) was made from these bytes + settings.
    bool NeedsRightEdit(int v, float eyeShift, float conv) const
    {
        const EditKey& k = m_edit[v & 1];
        return k.write != m_writeSeq || k.right != CopySeq(v) || k.shift != eyeShift || k.conv != conv;
    }
    void MarkRightEdited(int v, float eyeShift, float conv)
    {
        EditKey& k = m_edit[v & 1];
        k.write = m_writeSeq; k.right = CopySeq(v); k.shift = eyeShift; k.conv = conv;
    }
    void NoteMovedWrite() { ++m_movedSeq; }

    // Inverse-detection backoff: a buffer that keeps scanning clean is
    // rescanned only sparsely.
    bool ShouldScanForInverse()
    {
        const unsigned n = m_invScans++;
        return n < 16 || (n % 64) == 0;
    }

private:
    ID3D10Buffer*  m_real;                    // owned (released in dtor)
    ID3D10Buffer*  m_realRight       = nullptr;  // owned, null unless a stereo CB sibling
    ID3D10Buffer*  m_realRightMoved  = nullptr;  // owned, RightCopy(1)
    Device10Proxy* m_parent;                  // not owned
    LONG           m_refs;

    // Option B state (Map/Unmap frame-replay). Kept so existing CB eye-shift
    // keeps working until Cycle 2's Device10Proxy EditRightCBs replaces it.
    bool      m_vsBound;
    bool      m_activeMapValid;
    void*     m_activeMapData;
    UINT      m_activeMapByteWidth;
    D3D10_MAP m_activeMapType;

    // Option A (DX11-shape) state. Dormant in this commit; Cycle 2 wires it in.
    bool           m_everShifted     = false;
    unsigned       m_shiftedFrame    = 0;
    unsigned       m_invScans        = 0;
    unsigned       m_writeSeq        = 0;
    unsigned       m_patchedSeq      = 0xFFFFFFFFu;
    DWORD          m_patchedCRC      = 0;
    float          m_patchedShift    = 0.f;
    unsigned       m_rightForward    = 0;
    unsigned       m_rightInverse    = 0;
    unsigned       m_invCheckSeq     = 0xFFFFFFFFu;
    float          m_invCheckShift   = 0.f;
    float          m_invCheckConv    = 0.f;
    unsigned       m_rightSeq        = 0;
    unsigned       m_movedSeq        = 0;

    std::vector<unsigned char> m_shadow;
    std::vector<MatrixTarget>  m_learned;
    std::bitset<256>           m_lightRegs;
    std::bitset<256>           m_cameraRegs;

    struct EditKey { unsigned write = ~0u, right = ~0u; float shift = 0.f, conv = 0.f; };
    EditKey        m_edit[2];
    unsigned       CopySeq(int v) const { return (v & 1) ? m_movedSeq : m_rightSeq; }
};

} // namespace wiz3d
