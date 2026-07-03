#ifndef __S3D_WRAPPER_DIRECT3D_10_STEREO_COMMAND_BUFFER_H__
#define __S3D_WRAPPER_DIRECT3D_10_STEREO_COMMAND_BUFFER_H__

#include "Commands\CommandSet.h"
#include "ShaderWrapper.h"
#include <unordered_map>
#include <unordered_set>

// _CRTDBG_MAP_ALLOC (via SharedInclude.h) redefines 'free' as '_free_dbg',
// which corrupts boost::object_pool::free(). Shield the Pool header only.
#pragma push_macro("free")
#undef free
#include <boost\pool\pool_alloc.hpp>
#pragma pop_macro("free")

#include "CommandBuffer.h"
#include "../DX10SharedLibrary/ExternalConstantBuffer.h"

/** Organize DX commands to generate left and right views.
 * There are several commands buffers inside StereoCommandBuffer (listed in the order of execution):
 * \li \c LeftBuffer - stores commands to generate left view.
 * \li \c PrologueBuffer - stores commands to switch to the right view, i.e. restore state at the beginning of
 * the left buffer execution. Includes: xxUpdateSubresourceUp, SetRenderTargets.
 * \li \c RightBuffer - stores commands to generate right view (usually equal to the left buffer).
 * \li \c EpilogueBuffer - stores commands to switch to the left view from right. Includes: SetRenderTargets, 
 * delayed xxSetConstantBuffers and xxSetShaderResources for every shader pipeline state (to restore mono constant buffers and resources).
 * We also delay commands xxSetConstantBuffers until draw call, to setup stereo/mono constant buffers.
 */
class StereoCommandBuffer : 
	public CommandBuffer
{
private:
	typedef std::unordered_map<CommandsId, Commands::TCmdPtr>			TIdCommandMap;
	typedef std::unordered_map<D3D10DDI_HRESOURCE, Commands::TCmdPtr>	TResourceCommandMap;
	typedef std::unordered_set<D3D10DDI_HRESOURCE>						TResourceSet;

	enum TDrawType
	{
		drCasual	= 0x00,
		drAuto		= 0x01,
		drIndexed	= 0x02,
		drInstanced	= 0x04,
		drIndirect	= 0x08,
	};

public:
	StereoCommandBuffer();
	~StereoCommandBuffer();
		
	void AddFinalizeCommand( Commands::Command* pCmd_, bool WantFinalize = false );
	void AddCommand( Commands::Command* pCmd_ )									{ AddFinalizeCommand(pCmd_, true); }
	
	// Change device state G1
	void AddCommand( Commands::IaSetIndexBuffer* pCmd_ )						{ AddChangeStateCommand( pCmd_ ); }
	void AddCommand( Commands::IaSetInputLayout* pCmd_ )						{ AddChangeStateCommand( pCmd_ ); }
	void AddCommand( Commands::IaSetTopology* pCmd_ )							{ AddChangeStateCommand( pCmd_ ); }
	void AddCommand( Commands::IaSetVertexBuffers* pCmd_ )						{ AddChangeStateCommand( pCmd_ ); }
	void AddCommand( Commands::SetBlendState* pCmd_ )							{ AddChangeStateCommand( pCmd_ ); }
	void AddCommand( Commands::SetDepthStencilState* pCmd_ )					{ AddChangeStateCommand( pCmd_ ); }
	void AddCommand( Commands::SetPredication* pCmd_ )							{ AddChangeStateCommand( pCmd_ ); }
	void AddCommand( Commands::SetRasterizerState* pCmd_ )						{ AddChangeStateCommand( pCmd_ ); }
	void AddCommand( Commands::SetScissorRects* pCmd_ )							{ AddChangeStateCommand( pCmd_ ); }
	void AddCommand( Commands::SetTextFilterSize* pCmd_ )						{ AddChangeStateCommand( pCmd_ ); }
	void AddCommand( Commands::SetViewports* pCmd_ )							{ AddChangeStateCommand( pCmd_ ); }
	void AddCommand( Commands::SoSetTargets* pCmd_ )							{ AddChangeStateCommand( pCmd_ ); }
	void AddCommand( Commands::xxSetShader* pCmd_ )								{ AddChangeStateCommand( pCmd_ ); }
	void AddCommand( Commands::xxSetShaderWithIfaces11_0* pCmd_ )				{ AddChangeStateCommand( pCmd_ ); }

	// Delayed
	void AddCommand( Commands::xxSetSamplers* pCmd_ )							{ AddChangeStateCommand(pCmd_); }
	void AddCommand( Commands::xxSetShaderResources* pCmd_ )					{ AddChangeStateCommand(pCmd_); }
	void AddCommand( Commands::xxSetConstantBuffers* pCmd_ );	// delay them until draw call to setup necessary(left/right) CBs

	// Cache command if render target is stereo
	void AddCommand( Commands::xxSetRenderTargets* pCmd_ );
	void AddCommand( Commands::CsSetUnorderedAccessViews11_0* pCmd_ );
	
	// Draw commands
	void AddCommand( Commands::Draw* pCmd_ )									{ AddDrawCommand(pCmd_, drCasual); }
	void AddCommand( Commands::DrawAuto* pCmd_ )								{ AddDrawCommand(pCmd_, drAuto); }
	void AddCommand( Commands::DrawIndexed* pCmd_ )								{ AddDrawCommand(pCmd_, drIndexed); }
	void AddCommand( Commands::DrawIndexedInstanced* pCmd_ )					{ AddDrawCommand(pCmd_, drIndexed | drInstanced); }
	void AddCommand( Commands::DrawInstanced* pCmd_ )							{ AddDrawCommand(pCmd_, drInstanced); }

	void AddCommand( Commands::DrawIndexedInstancedIndirect11_0* pCmd_ )		{ AddDrawCommand(pCmd_, drIndexed | drInstanced | drIndirect); }
	void AddCommand( Commands::DrawInstancedIndirect11_0* pCmd_ )				{ AddDrawCommand(pCmd_, drInstanced | drIndirect); }

	void AddCommand( Commands::Dispatch11_0* pCmd_ )							{ AddDispatchCommand(pCmd_, drCasual); }
	void AddCommand( Commands::DispatchIndirect11_0* pCmd_ )					{ AddDispatchCommand(pCmd_, drIndirect); }

	// G1
	void AddCommand( Commands::ClearDepthStencilView* pCmd_ )					{ AddCommandThatMayUseStereoResources(pCmd_, TT_CLEARED); }
	void AddCommand( Commands::ClearRenderTargetView* pCmd_ )					{ AddCommandThatMayUseStereoResources(pCmd_, TT_CLEARED); }
	void AddCommand( Commands::GenMips* pCmd_ )									{ AddCommandThatMayUseStereoResources(pCmd_); }
	void AddCommand( Commands::ResourceConvert10_1* pCmd_ )						{ AddCommandThatMayUseStereoResources(pCmd_, TT_SETLIKESRC); }
	void AddCommand( Commands::ResourceConvertRegion10_1* pCmd_ )				{ AddCommandThatMayUseStereoResources(pCmd_, TT_SETLIKESRC); }
	// TODO: Support gInfo.BltSrcEye & gInfo.BltDstEye
	void AddCommand( Commands::ResourceCopy* pCmd_ )							{ AddCommandThatMayUseStereoResources(pCmd_, TT_SETLIKESRC); }
	// TODO: Support gInfo.BltSrcEye & gInfo.BltDstEye
	void AddCommand( Commands::ResourceCopyRegion* pCmd_ )						{ AddCommandThatMayUseStereoResources(pCmd_, TT_SETLIKESRC); }
	void AddCommand( Commands::ResourceReadAfterWriteHazard* pCmd_ )			{ AddCommandThatMayUseStereoResources(pCmd_); }
	void AddCommand( Commands::ResourceResolveSubresource* pCmd_ )				{ AddCommandThatMayUseStereoResources(pCmd_, TT_SETLIKESRC); }
	void AddCommand( Commands::ShaderResourceViewReadAfterWriteHazard* pCmd_ )	{ AddCommandThatMayUseStereoResources(pCmd_); }
	void AddCommand( Commands::QueryBegin* pCmd_ )								{ m_RightBuffer.push_back( Commands::TCmdPtr(pCmd_) ); }
	void AddCommand( Commands::QueryEnd* pCmd_ )								{ m_RightBuffer.push_back( Commands::TCmdPtr(pCmd_) ); }

	void AddCommand( Commands::ClearUnorderedAccessViewUint11_0* pCmd_ )		{ AddCommandThatMayUseStereoResources(pCmd_); }
	void AddCommand( Commands::ClearUnorderedAccessViewFloat11_0* pCmd_ )		{ AddCommandThatMayUseStereoResources(pCmd_); }
	void AddCommand( Commands::CopyStructureCount11_0* pCmd_ )					{ AddCommandThatMayUseStereoResources(pCmd_); }
	void AddCommand( Commands::SetResourceMinLOD11_0* pCmd_ )					{ AddCommandThatMayUseStereoResources(pCmd_); }

	// Resources
	void AddCommand( Commands::CreateResource* pCmd_ )							{ AddFinalizeCommand(pCmd_); }
	void AddCommand( Commands::CreateResource11_0* pCmd_ )						{ AddFinalizeCommand(pCmd_); }
	void AddCommand( Commands::xxUpdateSubresourceUP* pCmd_ );
	void AddCommand( Commands::xxMap* pCmd_ );
	void AddCommand( Commands::xxUnmap* pCmd_ );

	// DXGI G2
	void AddCommand( Commands::Blt* pCmd_ )										{ AddFinalizeCommand(pCmd_, true); }
	void AddCommand( Commands::Present* pCmd_ )									{ AddFinalizeCommand(pCmd_, true); }
		
	// Override CommandBuffer
	void Begin();
	void FlushAll();

#ifndef FINAL_RELEASE
	void ClearStatistics();
	void DumpStatistics();
#endif

protected:
	/** Clear internal buffers and state. */
	void Clear();
	
	/** Execute stereo command buffer. */
	void Execute();

	/** Execute provided command buffer. */
	void ExecuteBuffer(const TCommandsVector& buffer, bool bDump = false, bool bLeft = true);

	/** Restore state at the beginning of the buffer execution. */
	void ExecutePrologueBuffer();

	/** Execute commands to restore state as it was executed mono command buffer. */
	void ExecuteEpilogueBuffer();
	
	/** Just add command to left and right buffer respectively. */
	void AddStereoCommand(const Commands::TCmdPtr& pCmd1_, const Commands::TCmdPtr& pCmd2_);

	/** Just add command to left and right buffer respectively. 
	 * @param pCmd_ - command to add into buffers.
	 * @param require - require command to be stereo. Otherwise it will be added to rest buffer queue and if
	 * there is no stereo commands after it, it will be executed only once.
	 */
	void AddStereoCommand(const Commands::TCmdPtr& pCmd_, bool require = false);

	/** Copy rest commands to left and right buffer respectively. */
	void FlushRestBuffer();
	
	/** Add command SetDestResourceType. */
	void AddSetDestResourceTypeCommand(Commands::Command *cmd, TextureType type);

	// Clear..., GenMips, Resource..., ShaderResourceViewReadAfterWriteHazard
	void AddCommandThatMayUseStereoResources(Commands::Command* pCmd_, int type = -1);
	
	/** Add command that changes device state, need to cache it to successfully restore device state before
	 * right buffer execution. Includes (SetBlendState, xxSetConstantBuffers, xxSetShaderResources, ...)
	 */
	void AddChangeStateCommand(Commands::Command* pCmd_);
	
	/** Add draw command to left and right buffer, also add delayed xxSetConstantBuffers, xxSetShaderResources to
	 * properly setup stereo/mono command buffer and resources.
	 */
	void AddDrawCommand(Commands::DrawBase* pCmd_, int drawType);
	
	void AddDispatchCommand(Commands::DispatchBase11_0* pCmd_, int drawType);

	/** Setup constant buffers with modified shader data */
	void AddModifiedShaderCBs( ShaderWrapper* pWrp );

private:
	// Do not modify this buffers directly. Use AddXXX functions.
	TCommandsVector				m_RightBuffer;
	TCommandsVector				m_PrologueBuffer;
	TCommandsVector				m_EpilogueBuffer;
	TCommandsVector				m_RestBuffer;

	// states changed in the left buffer that have to be restored in right prologue buffer
	TIdCommandMap				m_StatesForRestoration;	

	// for the purpose of the following members see functions related to the resource updating, xxMap, xxUpdateSubresourceUp and also AddDrawCommand (rather heavy logic)
	DestResourcesSet			m_ResourcesToRestoreType;
	TResourceCommandMap			m_ResourcesForRestoration;
	TResourceSet				m_ResourcesUsedNotUpdated;
	TResourceSet				m_ResourcesUpdated;
		
#ifndef FINAL_RELEASE
	int							m_BufferDrawCount;
	int							m_ViewChangeCount;
	std::map<size_t, size_t>	m_MapResourceSizeCount;
	size_t						m_FinalizeCommandCount[idTotal];
#endif
};

#endif//__S3D_WRAPPER_DIRECT3D_10_STEREO_COMMAND_BUFFER_H__
