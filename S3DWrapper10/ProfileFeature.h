/*
* Project : wiz3D
* Copyright (C) Akshay Hegde 2026
*/

// Per-game behaviour as ProfileFeatures, one per <Profile> element, called by the game-agnostic core at fixed draw points.
// Adding one: a class in its own Feature*.cpp, plus its element in the registry in ProfileFeature.cpp.

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace wiz3d
{

class Context11Proxy;
class Buffer11Proxy;
struct ShaderAnalysis11Result;

// One shader's reflection (its RDEF chunk), queried by name.
class RdefView
{
public:
    RdefView(const void* bytecode, SIZE_T length);
    // false: the shader ships without RDEF (its reflection stripped), so no name lookup can succeed.
    bool Present() const { return m_rd != nullptr; }
    // The t-slot of a texture binding, -1 = none.
    int TextureSlot(const std::string& name) const;
    // The t-slot of any shader resource (texture, structured or byte-address buffer), -1 = none.
    int SrvSlot(const std::string& name) const;
    // Every cbuffer variable of that name: (cb slot, byte offset). usedOnly = only where the shader reads it.
    std::vector<std::pair<int, UINT>> Variables(const std::string& name, bool usedOnly) const;
    // The last of them; false when there is none.
    bool Variable(const std::string& name, bool usedOnly, int& cbSlot, UINT& offset) const;
    // Every float4x4 cbuffer variable the shader reads: (cb slot, byte offset).
    std::vector<std::pair<int, UINT>> UsedMatrices() const;
    // Every texture, cbuffer and variable name, as "texture t9 Name", "cbuffer cb0 Name", "variable cb0+192 Name".
    std::vector<std::string> Names() const;
    // The cb slot of a cbuffer by name, -1 = none.
    int CBufferSlot(const std::string& name) const;
    // A cbuffer variable by name alone (the first cbuffer bound in this shader that declares it): its cbuffer, slot,
    // byte offset and size. false = none.
    bool FindVariable(const std::string& name, std::string& cbuffer, int& slot, UINT& offset, UINT& size) const;

private:
    const BYTE* m_rd = nullptr;
    DWORD       m_size = 0;
    DWORD       m_varStride = 24;
    DWORD       Rd32(DWORD off) const;
    const char* Str(DWORD off) const;
};

// A profile element's attributes, name -> value.
typedef std::map<std::string, std::string> ProfileEntry;

// The left camera's p00 and the right camera's move: d along view x, world = d * the camera's right axis (right, unit).
struct CameraMove
{
    float p00 = 0.f, d = 0.f;
    float world[3] = {};
    float right[3] = {};
};
// One attribute, "" when absent.
const std::string& EntryValue(const ProfileEntry& entry, const char* attr);
// A profile element's child elements in order, each with its own name under "@".
std::vector<const ProfileEntry*> ChildEntries(const char* element);

class ProfileFeature
{
public:
    virtual ~ProfileFeature() {}

    // Shader creation (device side, every stage): locate this feature's names in the shader; a cbuffer the feature edits
    // for the right eye is noted on the shader (ShaderAnalysis11Result::NoteRightCB).
    virtual void OnShaderCreated(const RdefView&, ShaderAnalysis11Result&) const {}

    // A right draw's or dispatch's copy of a noted cbuffer (the game's bytes plus earlier features' edits), in a pass at
    // the presented aspect: edit it for the right eye in place; true = edited.
    virtual bool EditRightConstants(Context11Proxy&, const std::string& cbuffer, unsigned char* data, size_t size)
    { return false; }

    // Draw points (context side), in order: targets bound; the game's (left) draw about to run; the right copy bound;
    // the right copy drawn; both eyes done. Then once per Present (report = this Present writes the log rows).
    virtual void OnTargetsSet(Context11Proxy&) {}
    virtual void OnLeftDraw(Context11Proxy&) {}
    virtual void OnRightDraw(Context11Proxy&) {}
    virtual void OnRightDispatch(Context11Proxy&) {}    // a right dispatch's resources are bound (compute)
    virtual void OnRightDrawDone(Context11Proxy&) {}
    virtual void OnDrawDone(Context11Proxy&) {}
    virtual void OnFrameEnd(Context11Proxy&, unsigned frame, bool report) {}

    // What the shared stereo code asks of every feature.
    virtual float EyeShiftSign() const { return 1.f; }       // -1 while drawing into an x-flipped target
    virtual bool  LightViewPass() const { return false; }    // the bound pass renders a light's view
    virtual bool  VertexCameraMatrices() const { return false; }   // vertex shaders' declared camera matrices are patched
    virtual bool  UiProjectionParallax() const { return false; }   // far-off focals take the scene camera's parallax
    virtual bool  RingKeepsShifts() const { return false; }  // a ring's right bytes never lose a shift in place
    virtual bool  KeepBareProjections() const { return false; }   // pixel and compute shaders' view-to-clip stays the game's
    virtual bool  WholeBufferRanges() const { return false; }     // a whole-buffer rebind's 11.1 count rounds up to 16
    virtual bool  ViewMatrices() const { return false; }     // pixel and compute shaders' camera views move with the eye
    virtual float CameraP00() const { return 0.f; }          // the scene camera's p00, 0 = unknown
    // The camera of the block the bound shaders read, and the right camera's move from it. false = none bound.
    virtual bool  BoundCameraMove(Context11Proxy&, CameraMove&) const { return false; }
    // The bound pass needs the right camera's origin moved (it rebuilds position as camera position + ray * depth); its
    // edits go to a second right copy (Context11Proxy::MovedOrigin).
    virtual bool  MovedOriginPass(Context11Proxy&) const { return false; }
};

// A feature's setup lines (names found, targets learned), written only with CollectDebugInformation; its per-frame
// rows are gated the same way by OnFrameEnd's report flag.
void FeatureLog(const char* fmt, ...);

typedef std::vector<std::unique_ptr<ProfileFeature>> ProfileFeatures;

// The features the game's profile names (g_ProfileData), in registry order.
ProfileFeatures CreateProfileFeatures();

// The factories the registry calls; null when the profile has no such element.
std::unique_ptr<ProfileFeature> MakeStereoTexture(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeMirroredTarget(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeHudDepth(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeLightViewPasses(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeVertexCameraMatrices(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeUiProjectionParallax(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeAdditionalMatrices(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeAdditionalMatrixLayout(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeRightCamera(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeRayCorners(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeDuniaLocalLights(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeCameraProjection(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeInverseCameraProjection(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeLeftEyeTextures(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeCameraRelativePlanes(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeScreenRayMatrix(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeClipSpaceSprites(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeClipSpaceRays(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeRingKeepsShifts(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeKeepBareProjections(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeWholeBufferRanges(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeViewMatrices(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeDeferredContexts(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakePixelRayBasis(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeDeferredLightCamera(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeRawResources(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeConstantBlocks(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeComputeOutputsPerEye(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeIndirectArgsPerEye(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeCameraInverses(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeCodeMatrices(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeFactory2SwapChains(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeHudSceneDepth(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeStereoScissor(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeBatchRightEye(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakePrivateBackBufferRefs(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeRecycleRightCopies(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeDecalMatrices(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeFogVolumeLookups(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeTypelessBackBuffer(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeViewportMatrices(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeCodeMatrixRows(const ProfileEntry* entry);
std::unique_ptr<ProfileFeature> MakeTiledLightVolumes(const ProfileEntry* entry);

// The game's profile has the element and it is not Value="0" (for switches the core asks outside a context).
bool ProfileSwitchOn(const char* element);

// <RawResources>: copies resolve a real texture the game reached past the wrapper to its proxy (FeatureRawResources.cpp).
bool RawResourcesOn();
void NoteRawResolved();

// Core switches, each in its own Feature*.cpp; absent, the core runs as it does for every other game.
bool ConstantBlocksOn();         // constant blocks rebuilt per draw and dispatch, 11.1 ring buffers included
bool ComputeOutputsPerEyeOn();   // UAV textures doubled like render targets
bool IndirectArgsPerEyeOn();     // structure-count destinations given a right sibling
bool CameraInversesOn();         // any camera's unprojection matched and patched with its own focal
bool CodeMatricesOn();           // matrices of shaders without reflection found from their code
bool CodeMatrixRowsOn();         // those matrices are classified as the dp4 rows they are read as, not in both orientations
bool Factory2SwapChainsOn();     // Factory2 swap chains wrapped on any factory, not only CreateDXGIFactory2's
bool BatchRightEyeOn();          // the right eye's draws recorded and replayed once per pass; on unless Value="0"
bool PrivateBackBufferRefsOn();  // our holds on the back buffer left out of the count the game reads
bool RecycleRightCopiesOn();     // a freed texture's right copy serves the next texture created with its desc
bool TypelessBackBufferOn();     // the stereo back-buffer stand-ins are typeless, so _SRGB views of a UNORM swap chain work
bool ViewportMatricesOn();       // camera matrices with the viewport folded in are declared camera matrices, shifted in pixels
// ViewportMatrices (FeatureViewportMatrices.cpp): the kind (1 world to pixel, 2 pixel to world, 0 neither), and the shift's
// focal in pixels and w basis norm (false once the shape is lost).
int  ClassifyViewportMatrix(const float* f, float scene, bool rowsOnly, bool& transposed, DWORD crc, UINT slot, DWORD reg);
bool ViewportFocal(const float* f, bool transposed, bool inverse, float scene, float& focal, float& wn);
void CountViewportPatch(bool inverse, bool ok);
float UiProjectionReferenceNear();   // UiProjectionParallax SceneNear, 0 = the frame's own scene camera

// Stereo state the core owns, for features to read or feed.
namespace stereo
{
float Convergence();               // zero-parallax distance in world units, 0 = convergence off
float FallbackB(float eyeShift);   // the shear from the last shifted camera matrix, for when the camera's p00 is unknown
void  PublishCameraP00(float p00); // the scene camera's p00, used for scaled matrices (0 = unknown)
float SceneP00();                  // the frame's scene camera p00 (0 = unknown)
float UiParallaxScale(float p00);  // under UiProjectionParallax SceneNear, the factor a camera of this p00 scales its
                                   // eye shift by to take the gameplay camera's parallax; 1 otherwise
bool  SceneZRow(float& m22, float& m23);   // its projection's z row (z_ndc = m22 + m23 / w); false = none this frame
}

} // namespace wiz3d
