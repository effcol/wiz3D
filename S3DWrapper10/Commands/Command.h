#pragma once

#include <boost\noncopyable.hpp>
#include <boost\intrusive_ptr.hpp>

// _CRTDBG_MAP_ALLOC (injected via PCH when DEBUG_MEMORY_LEAKS is defined in
// Shared/SharedInclude.h) macro-replaces free(p) with _free_dbg(...) and
// malloc(s) with _malloc_dbg(...). Both collide with boost::object_pool's
// member functions named free() and malloc(). The collision is purely a
// preprocessor name-substitution problem; no actual CRT debug-heap tracking
// is at stake here, because these pool member calls don't invoke the CRT
// free/malloc -- they manipulate the pool's own internal free-list.
//
// We solve it once, in two parts:
//   1. Include the Pool header with the colliding macros temporarily off.
//   2. Define a small pair of helpers (pool_acquire / pool_release) in the
//      same shielded scope. The helpers expose non-colliding names, so the
//      call sites in operator new / operator delete below stay clean and
//      no scattered per-call-site shielding is needed.
#ifdef _CRTDBG_MAP_ALLOC
#pragma push_macro("free")
#pragma push_macro("malloc")
#undef free
#undef malloc
#endif
#include <boost\pool\object_pool.hpp>
namespace detail
{
	template <typename Pool>
	inline void* pool_acquire(Pool* pool) { return pool->malloc(); }

	template <typename Pool, typename T>
	inline void pool_release(Pool* pool, T* ptr) { pool->free(ptr); }
}
#ifdef _CRTDBG_MAP_ALLOC
#pragma pop_macro("malloc")
#pragma pop_macro("free")
#endif

#include "..\Array.h"
#define SMALL_OBJECT_ALLOCATOR_STATISTICS
#include "..\SmallObjectAllocator.h"
#include "..\Streamer\CodeGenerator.h"
#include "..\CommonUtils\System.h"
#include <vector>
#include <map>
#include <set>

#include "..\streamer\DumpState.h"
#include "..\streamer\BinaryDumper.h"
#include "..\DeviceWrapperRegistry.h"

enum TextureType 
{
	TT_MONO,		// Only left RT contain correct image
	TT_STEREO,		// Left and right RT's contain correct stereo images
	TT_CLEARED,		// Left and right RT's contain identical images
	TT_SETLIKESRC	// For internal use in StereoCommandBuffer only
};

enum VIEWINDEX
{
	VIEWINDEX_LEFT  = 0,
	VIEWINDEX_RIGHT = 1,

	VIEWINDEX_MONO  = 2,
};

// pipeline order is important! 
enum SHADER_PIPELINE
{
	SP_NONE  = -1,
	SP_GS    =  0,
	SP_DS    =  1,
	SP_VS    =  2,
	SP_HS    =  3,
	SP_PS    =  4,
	SP_CS    =  5,
	SP_COUNT =  6
};

enum CommandsId
{
	idNone = -1,
	//DXGIFunctions = 1,
	idBlt = 0,
	idPresent,
	idGetGammaCaps,
	idSetDisplayMode,
	idSetResourcePriority,
	idQueryResourceResidency,
	idRotateResourceIdentities,
	idResolveSharedResource,

	//D3D10Functions,
	idGsSetShader,
	idGsSetConstantBuffers,
	idGsSetSamplers,
	idGsSetShaderResources,
	idPsSetShader,
	idPsSetConstantBuffers,
	idPsSetSamplers,
	idPsSetShaderResources,
	idVsSetShader,
	idVsSetConstantBuffers,
	idVsSetSamplers,
	idVsSetShaderResources,
	idIaSetIndexBuffer,
	idIaSetInputLayout,
	idIaSetTopology,
	idIaSetVertexBuffers,
	idSetRasterizerState,
	idSetViewports,
	idSetScissorRects,
	idSetBlendState,
	idSetDepthStencilState,
	idSetRenderTargets,
	idSetPredication,
	idSoSetTargets,
	idSetTextFilterSize,

	idDefaultConstantBufferUpdateSubresourceUP,
	idDrawIndexed,
	idDraw,
	idDynamicIABufferMapNoOverwrite,
	idDynamicIABufferUnmap,
	idDynamicConstantBufferMapDiscard,
	idDynamicIABufferMapDiscard,
	idDynamicConstantBufferUnmap,
	idDrawIndexedInstanced,
	idDrawInstanced,
	idDynamicResourceMapDiscard,
	idDynamicResourceUnmap,
	idStagingResourceMap,
	idStagingResourceUnmap,
	idShaderResourceViewReadAfterWriteHazard,
	idResourceReadAfterWriteHazard,
	idQueryEnd,
	idQueryBegin,
	idResourceCopyRegion,
	idResourceUpdateSubresourceUP,
	idDrawAuto,
	idClearRenderTargetView,
	idClearDepthStencilView,
	idQueryGetData,
	idFlush,
	idGenMips,
	idResourceCopy,
	idResourceResolveSubresource,
	idResourceMap,
	idResourceUnmap,
	idResourceIsStagingBusy,
	idRelocateDeviceFuncs,
	idCalcPrivateResourceSize,
	idCalcPrivateOpenedResourceSize,
	idCreateResource,
	idOpenResource,
	idDestroyResource,
	idCalcPrivateShaderResourceViewSize,
	idCreateShaderResourceView,
	idDestroyShaderResourceView,
	idCalcPrivateRenderTargetViewSize,
	idCreateRenderTargetView,
	idDestroyRenderTargetView,
	idCalcPrivateDepthStencilViewSize,
	idCreateDepthStencilView,
	idDestroyDepthStencilView,
	idCalcPrivateElementLayoutSize,
	idCreateElementLayout,
	idDestroyElementLayout,
	idCalcPrivateBlendStateSize,
	idCreateBlendState,
	idDestroyBlendState,
	idCalcPrivateDepthStencilStateSize,
	idCreateDepthStencilState,
	idDestroyDepthStencilState,
	idCalcPrivateRasterizerStateSize,
	idCreateRasterizerState,
	idDestroyRasterizerState,
	idCalcPrivateShaderSize,
	idCreateVertexShader,
	idCreateGeometryShader,
	idCreatePixelShader,
	idCalcPrivateGeometryShaderWithStreamOutput,
	idCreateGeometryShaderWithStreamOutput,
	idDestroyShader,
	idCalcPrivateSamplerSize,
	idCreateSampler,
	idDestroySampler,
	idCalcPrivateQuerySize,
	idCreateQuery,
	idDestroyQuery,
	idCheckFormatSupport,
	idCheckMultisampleQualityLevels,
	idCheckCounterInfo,
	idCheckCounter,
	idDestroyDevice,

	//D3D10_1Functions,
	idRelocateDeviceFuncs10_1,
	idCalcPrivateShaderResourceViewSize10_1,
	idCreateShaderResourceView10_1,
	idCalcPrivateBlendStateSize10_1,
	idCreateBlendState10_1,
	idResourceConvert10_1,
	idResourceConvertRegion10_1,

	//D3D11_0Functions,
	idSetRenderTargets11_0,
	idRelocateDeviceFuncs11_0,
	idCalcPrivateResourceSize11_0,
	idCreateResource11_0,
	idCalcPrivateShaderResourceViewSize11_0,
	idCreateShaderResourceView11_0,
	idCalcPrivateDepthStencilViewSize11_0,
	idCreateDepthStencilView11_0,
	idCalcPrivateGeometryShaderWithStreamOutput11_0,
	idCreateGeometryShaderWithStreamOutput11_0,
	idDrawIndexedInstancedIndirect11_0,
	idDrawInstancedIndirect11_0,
	idCommandListExecute11_0,
	idHsSetShaderResources11_0,
	idHsSetShader11_0,
	idHsSetSamplers11_0,
	idHsSetConstantBuffers11_0,
	idDsSetShaderResources11_0,
	idDsSetShader11_0,
	idDsSetSamplers11_0,
	idDsSetConstantBuffers11_0,
	idCreateHullShader11_0,
	idCreateDomainShader11_0,
	idCheckDeferredContextHandleSizes11_0,
	idCalcDeferredContextHandleSize11_0,
	idCalcPrivateDeferredContextSize11_0,
	idCreateDeferredContext11_0,
	idAbandonCommandList11_0,
	idCalcPrivateCommandListSize11_0,
	idCreateCommandList11_0,
	idDestroyCommandList11_0,
	idCalcPrivateTessellationShaderSize11_0,
	idPsSetShaderWithIfaces11_0,
	idVsSetShaderWithIfaces11_0,
	idGsSetShaderWithIfaces11_0,
	idHsSetShaderWithIfaces11_0,
	idDsSetShaderWithIfaces11_0,
	idCsSetShaderWithIfaces11_0,
	idCreateComputeShader11_0,
	idCsSetShader11_0,
	idCsSetShaderResources11_0,
	idCsSetSamplers11_0,
	idCsSetConstantBuffers11_0,
	idCalcPrivateUnorderedAccessViewSize11_0,
	idCreateUnorderedAccessView11_0,
	idDestroyUnorderedAccessView11_0,
	idClearUnorderedAccessViewUint11_0,
	idClearUnorderedAccessViewFloat11_0,
	idCsSetUnorderedAccessViews11_0,
	idDispatch11_0,
	idDispatchIndirect11_0,
	idSetResourceMinLOD11_0,
	idCopyStructureCount11_0,

	//Driver commands
	idChangeDeviceView,
	idChangeShaderMatrixView,
	idSetDestResourceType,

	//Total num of cmd
	idTotal,
};

inline std::size_t hash_value(CommandsId const& id)
{
	return static_cast<std::size_t>(id);
}

namespace std
{
template<>
struct hash<CommandsId>
{
	size_t operator()(CommandsId _Keyval) const
	{
		return (std::hash<char>()((char)_Keyval));
	}
};
}

#ifndef FINAL_RELEASE
//#define ENABLE_TIMING
#endif

#define ENABLE_THREAD_GUARD

class CodeGenerator;
class D3DDeviceWrapper;
class D3DDeviceState;
struct ProjectionShaderData;
struct UnprojectionShaderData;
struct ModifiedShaderData;

typedef enum COMMAND_STATES
{
	COMMAND_CHANGE_DEVICE_STATE 			 = 0x01,
	COMMAND_CHANGE_DEVICE_STEREO_STATE 		 = 0x02,
	COMMAND_DEPENDED_ON_DEVICE_STATE 		 = 0x04,	// Draw...
	COMMAND_DEPENDED_ON_COMPUTE_DEVICE_STATE = 0x08,	// Dispatch...
	COMMAND_MAY_USE_STEREO_RESOURCES 		 = 0x10,	// Clear, CopyResource
} COMMAND_STATES;

typedef std::set<D3D10DDI_HRESOURCE> DestResourcesSet;

namespace Commands
{
	class Command;
	typedef	boost::intrusive_ptr<Command> TCmdPtr;
	
	class CreateResource;
	class CreateResource11_0;

	class xxSetConstantBuffers;
	typedef boost::intrusive_ptr<xxSetConstantBuffers> TSetConstantBuffersCmdPtr;

	class xxSetShaderResources;
	typedef boost::intrusive_ptr<xxSetShaderResources> TSetShaderResourcesCmdPtr;

	class xxSetSamplers;
	typedef boost::intrusive_ptr<xxSetSamplers> TSetSamplersCmdPtr;

	class xxSetRenderTargets;
	typedef boost::intrusive_ptr<xxSetRenderTargets> TSetRenderTargetsCmdPtr;

	class IaSetVertexBuffers;
	typedef boost::intrusive_ptr<IaSetVertexBuffers> TIaSetVertexBuffersCmdPtr;

	class ChangeShaderMatrixView;
	typedef boost::intrusive_ptr<ChangeShaderMatrixView> TChangeShaderMatrixViewCmdPtr;
	
	class CsSetUnorderedAccessViews11_0;
	typedef boost::intrusive_ptr<CsSetUnorderedAccessViews11_0> TCsSetUnorderedAccessViewsCmdPtr;

	class xxUpdataSubresourceUp;
	class xxMap;
	class xxUnmap;

	class DrawBase;

	class Command : boost::noncopyable
	{
	public:
		enum output_target
		{
			draw,
			init,
			decl,
			glob,

			output_target_count
		};

		Command()
		{
			m_RefCount = 0;
			CommandId = idNone;
			State_ = 0;
#ifndef FINAL_RELEASE
			_output_target = draw;
			EventID_ = GetEventID();
#ifdef ENABLE_TIMING
			StartTime_.QuadPart = Duration_.QuadPart = 0;
#endif
#endif
		};
		virtual								~Command() {};
		virtual void						Execute	( D3DDeviceWrapper* pWrapper )	= 0;

		virtual void						UpdateDeviceState( D3DDeviceWrapper* /*pWrapper*/, D3DDeviceState* /*pState*/ ) { _ASSERT(FALSE); }
		// for states IsUsedStereoResources() should be called after UpdateDeviceState
		virtual bool						IsUsedStereoResources( D3DDeviceWrapper* /*pWrapper*/ ) const { _ASSERT(FALSE); return false; }
		virtual void						SetDestResourceType( TextureType /*type*/ ) { _ASSERT(FALSE); }
		virtual void						GetDestResources(DestResourcesSet& /*res*/) { _ASSERT(FALSE); }

		virtual void						Init() {};
		virtual bool						WriteToFile( D3DDeviceWrapper* pWrapper ) const	= 0;
		virtual bool						WriteToFileSimple( D3DDeviceWrapper* /*pWrapper*/ )	const { return false; }
		virtual bool						ReadFromFile() = 0;
		virtual void						SendToCodeGenerator( CodeGenerator *cg /*pCodeGenerator_*/ );

		virtual void 						Dump(std::ostream& os) const;
		
#ifndef FINAL_RELEASE
		virtual void						BuildCommand(CDumpState* /*ds*/) { }

		std::string GetBuiltCommand(CDumpState *ds,std::string &sinit,std::string &sdecl,std::string &sglob,std::set<UINT> &skipped);

		void set_output_target(output_target targ) { _output_target = targ; }
		output_target _output_target;

		void output(output_target targ,const char *format,...);
		void output(const char *format,...);
		void output_(output_target targ,const char *format,...);
		void output_(const char *format,...);

		virtual bool empty_output() { return false; }

		std::string _output[output_target_count];

		virtual void *Creates();
		virtual std::vector<void *> DependsOn();

		CBinaryDumper &Bd() const { return CBinaryDumper::GetInstance(); }
#else
		struct _bdstub
		{
			std::vector<int> SaveResourceToBinaryFileEx(...) { return std::vector<int>(); }
			UINT SaveToBinaryFile(...) { return 0; }
			UINT64 SaveToBinaryFile2(...) { return 0; }
			void SetResourceOfView(...) { }
			void SetLastResources(...) { }
			void FlushIndexes(...) { }
		};
		
		_bdstub &Bd() const
		{
			static _bdstub bd;
			return bd;
		}

		UINT EventID_;
		void output(...) { }
		void output_(...) { }

		void set_output_target(output_target /*targ*/) { }
#endif // FINAL_RELEASE

		CommandsId							CommandId;
		DWORD								State_;

		static VIEWINDEX						CurrentView_;
		static DWORD							CurrentShaderCRC32_;
		static const ProjectionShaderData*		CurrentShaderData_[SP_COUNT];
		static const ModifiedShaderData*		CurrentModifiedShaderData_[SP_COUNT];
		static const UnprojectionShaderData*	CurrentPixelShaderData_[SP_COUNT];

#ifndef FINAL_RELEASE	
		UINT								EventID_;
		static	CommandsId					LastCommandId_;

#ifdef ENABLE_TIMING
		LARGE_INTEGER						StartTime_;
		LARGE_INTEGER						Duration_;
#endif

		static	UINT						GetEventID();
		static	double						GetTimeMilliseconds();

	public:
		static Command*						CreateCommandFromCID	( int _nCID );
		static void							InitCommandsCreateFunc	();
	protected:

		typedef Command*					( * pfCreateCMDFunc )();
		static pfCreateCMDFunc				m_CreateCMDFuncs[ idTotal ];		
#endif
	private:		
		mutable LONG 						m_RefCount;
	public:
		inline	LONG						GetRefCount	() 
		{ 
			return m_RefCount;
		};
		inline	LONG						AddRef	() 
		{ 
			//return InterlockedIncrement(&m_RefCount);
			m_RefCount++;
			return m_RefCount;
		};
		inline  LONG						Release	() 
		{ 
			//LONG refCount = InterlockedDecrement(&m_RefCount); 
			m_RefCount--;
			LONG refCount = m_RefCount;
			if ( refCount == 0 )
				delete this;
			return refCount;
		};
	protected:		
		//static	MemoryManager m_MemoryPool;
	};

	template<typename T, typename Base = Command>
	class CommandWithAllocator :
		public Base
	{
	friend class Command;
	public:
		typedef CommandWithAllocator			base_type;
		typedef boost::object_pool<T>			pool_type;

	public:
		CommandWithAllocator() {}

		template<typename T0>
		CommandWithAllocator(T0 a0) : Base(a0) {}

		template<typename T0, typename T1>
		CommandWithAllocator(T0 a0, T1 a1) : Base(a0, a1) {}

		template<typename T0, typename T1, typename T2>
		CommandWithAllocator(T0 a0, T1 a1, T2 a2) : Base(a0, a1, a2) {}

		template<typename T0, typename T1, typename T2, typename T3>
		CommandWithAllocator(T0 a0, T1 a1, T2 a2, T3 a3) : Base(a0, a1, a2, a3) {}

		template<typename T0, typename T1, typename T2, typename T3, typename T4>
		CommandWithAllocator(T0 a0, T1 a1, T2 a2, T3 a3, T4 a4) : Base(a0, a1, a2, a3, a4) {}

		void* operator new ( size_t nSize_ )
		{
			CriticalSectionLocker lock(m_csAllocator);
			//std::string message = std::string("Trying to create/destroy commands concurrently: ") + typeid(T).name();
			//CriticalSectionDebugLocker locker( m_csAllocator, message.c_str() );

			_ASSERT( nSize_ == sizeof(T) );
			if (!m_InternalPool) {
				m_InternalPool = new boost::object_pool<T>;
			}
			++m_InternalPoolUseCount;
			return detail::pool_acquire(m_InternalPool);
		}

		void* operator new ( size_t nSize_, int _Block, const char * _Filename, int _Line )
		{
			CriticalSectionLocker lock(m_csAllocator);
			//std::string message = std::string("Trying to create/destroy commands concurrently: ") + typeid(T).name();
			//CriticalSectionDebugLocker locker( m_csAllocator, message.c_str() );

			_ASSERT( nSize_ == sizeof(T) );
			if (!m_InternalPool) {
				m_InternalPool = new boost::object_pool<T>;
			}
			++m_InternalPoolUseCount;
			return detail::pool_acquire(m_InternalPool);
		}

		void operator delete ( void * pAddr_ )
		{
			CriticalSectionLocker lock(m_csAllocator);
			//std::string message = std::string("Trying to create/destroy commands concurrently: ") + typeid(T).name();
			//CriticalSectionDebugLocker locker( m_csAllocator, message.c_str() );

			assert(m_InternalPoolUseCount > 0);
			detail::pool_release(m_InternalPool, (T*)pAddr_);
			if (--m_InternalPoolUseCount == 0)
			{
				delete m_InternalPool;
				m_InternalPool = 0;
			}
		}

		void operator delete ( void * pAddr_, int _Block, const char * _Filename, int _Line )
		{
			CriticalSectionLocker lock(m_csAllocator);
			//std::string message = std::string("Trying to create/destroy commands concurrently: ") + typeid(T).name();
			//CriticalSectionDebugLocker locker( m_csAllocator, message.c_str() );

			assert(m_InternalPoolUseCount > 0);
			detail::pool_release(m_InternalPool, (T*)pAddr_);
			if (--m_InternalPoolUseCount == 0)
			{
				delete m_InternalPool;
				m_InternalPool = 0;
			}
		}

#ifndef FINAL_RELEASE
		virtual void SendToCodeGenerator( CodeGenerator* pCodeGenerator_ )
		{
			_ASSERT( pCodeGenerator_ );
			pCodeGenerator_->ProcessCommand( static_cast<T&>(*this) ); // T is real command type derived from this
		}

	private:
		static Command* InternalCreate() { return DNew T(); }
#endif

	private:
		static size_t		m_InternalPoolUseCount;
		static pool_type*	m_InternalPool;

		static CriticalSection m_csAllocator;
	};

	template<typename T, typename Base>
	typename CommandWithAllocator<T, Base>::pool_type* CommandWithAllocator<T, Base>::m_InternalPool = 0;

	template<typename T, typename Base>
	size_t CommandWithAllocator<T, Base>::m_InternalPoolUseCount;

	template<typename T, typename Base>
	CriticalSection CommandWithAllocator<T, Base>::m_csAllocator;

	template<typename T>
	struct fast_vector_policy
	{
		typedef boost::true_type no_construct;
		typedef boost::true_type no_destroy;
	};

	template<typename T>
	struct fast_vector
	{
		//typedef boost::pool_allocator<T, boost::default_user_allocator_new_delete, boost::details::pool::null_mutex> Allocator;
		typedef small_object_allocator<T>							allocator_type;
		typedef array<T, allocator_type, fast_vector_policy<T> >	type;
		//typedef std::vector<T, Allocator> Type;
	};

	inline std::vector<void *> ListHelper(void *p)
	{
		std::vector<void *> v;
		v.push_back(p);
		return v;
	}
	inline std::vector<void *> ListHelper(void *p,void *q)
	{
		std::vector<void *> v;
		v.push_back(p);
		v.push_back(q);
		return v;
	}

#ifndef FINAL_RELEASE

#define EMPTY_OUTPUT() bool empty_output() { return true; }

#define CREATES(x) \
	void *Creates() \
	{ \
		return x.pDrvPrivate; \
	}

#define DEPENDS_ON(x) \
	std::vector<void *> DependsOn() \
	{ \
		return ListHelper(x.pDrvPrivate); \
	}

#define DEPENDS_ON2(x,y) \
	std::vector<void *> DependsOn() \
	{ \
		return ListHelper(x.pDrvPrivate,y.pDrvPrivate); \
	}

#define DEPENDS_ON_VECTOR(x) \
	std::vector<void *> DependsOn() \
	{ \
		std::vector<void *> v; \
		for(unsigned i = 0; i < x.size(); i++)	v.push_back(x[i].pDrvPrivate); \
		return v; \
	}

#define DEPENDS_ON_VECTOR_MEMBER(x,y) \
	std::vector<void *> DependsOn() \
	{ \
		std::vector<void *> v; \
		for(unsigned i = 0; i < x.size(); i++)	v.push_back(x[i].y.pDrvPrivate); \
		return v; \
	}

#define DEPENDS_ON_SCALARVECTOR(s,x) \
	std::vector<void *> DependsOn() \
	{ \
		std::vector<void *> v; \
		v.push_back(s.pDrvPrivate); \
		for(unsigned i = 0; i < x.size(); i++)	v.push_back(x[i].pDrvPrivate); \
		return v; \
	}

#define DEPENDS_ON_SCALARVECTOR2(s,x,y) \
	std::vector<void *> DependsOn() \
	{ \
		std::vector<void *> v; \
		v.push_back(s.pDrvPrivate); \
		for(unsigned i = 0; i < x.size(); i++)	v.push_back(x[i].pDrvPrivate); \
		for(unsigned i = 0; i < y.size(); i++)	v.push_back(y[i].pDrvPrivate); \
		return v; \
	}

#else // FINAL_RELEASE

#define EMPTY_OUTPUT()
#define CREATES(x)
#define DEPENDS_ON(x)
#define DEPENDS_ON2(x,y)
#define DEPENDS_ON_VECTOR(x)
#define DEPENDS_ON_VECTOR_MEMBER(x,y)
#define DEPENDS_ON_SCALARVECTOR(s,x)
#define DEPENDS_ON_SCALARVECTOR2(s,x,y)

#endif // FINAL_RELEASE

	inline UINT GetClearCount(UINT Current, UINT &Previous)
	{
		UINT r = Current >= Previous ? 0 : Previous - Current;
		Previous = Current;
		return r;
	}
	
// Side-table lookup replaces the legacy "wrapper at offset 0 of runtime
// private buffer" cast. See DeviceWrapperRegistry.h for rationale (Win11
// D3D11.10+ DDI struct layouts broke the pointer-shift trick).
#define D3D10_GET_WRAPPER()					((D3DDeviceWrapper*)iZ3D::LookupDeviceWrapper(hDevice.pDrvPrivate))
#define D3D10_GET_ORIG()					D3D10_GET_WRAPPER()->OriginalDeviceFuncs
#define D3D10_1_GET_ORIG()					D3D10_GET_WRAPPER()->OriginalDeviceFuncs10_1
#define D3D11_GET_ORIG()					D3D10_GET_WRAPPER()->OriginalDeviceFuncs11
#define D3D10_DEVICE						D3D10_GET_WRAPPER()->hDevice

#define DXGI_MODIFY_DEVICE(x)				DXGI_DDI_HDEVICE  hOrigDevice = x->hDevice; \
	x->hDevice = (DXGI_DDI_HDEVICE)DXGI_GET_WRAPPER()->hDevice.pDrvPrivate;
#define DXGI_GET_WRAPPER()					((D3DDeviceWrapper*)iZ3D::LookupDeviceWrapper((void*)hOrigDevice))
#define DXGI_DEVICE							((DXGI_DDI_HDEVICE)DXGI_GET_WRAPPER()->hDevice.pDrvPrivate)
#define DXGI_GET_ORIG()						DXGI_GET_WRAPPER()->OriginalDXGIDDIBaseFunctions
#define DXGI_GET_ORIG2()					DXGI_GET_WRAPPER()->OriginalDXGIDDIBaseFunctions2
#define DXGI_RESTORE_DEVICE(x)				x->hDevice = hOrigDevice;

#define REGISTER_CREATEFUNC(name)           Command::m_CreateCMDFuncs[ id##name ] = name##::InternalCreate;

}

#ifndef OUTPUT_LIBRARY
template<typename T>
inline	T*	GetPointerToVector(std::vector<T> &src)
{
	if (!src.empty())
		return &src.front();
	else
		return NULL;
}

template<typename T>
inline	CONST T*	GetPointerToVector(CONST std::vector<T> &src)
{
	if (!src.empty())
		return &src.front();
	else
		return NULL;
}

template<typename T,typename A>
inline	T*	GetPointerToVector(std::vector<T, A> &src)
{
	if (!src.empty())
		return &src.front();
	else
		return NULL;
}

template<typename T,typename A>
inline	CONST T*	GetPointerToVector(CONST std::vector<T, A> &src)
{
	if (!src.empty())
		return &src.front();
	else
		return NULL;
}

template<typename T,typename A,typename Tr>
inline	T*	GetPointerToVector(array<T, A, Tr> &src)
{
	return src.begin();
}

template<typename T,typename A,typename Tr>
inline	CONST T*	GetPointerToVector(CONST array<T, A, Tr> &src)
{
	return src.begin();
}
#endif

template<typename T>
inline	void	CopyDataToArray(T* &dst, CONST T* src, UINT&  dstNumBuffers, UINT  numBuffers)
{
	dstNumBuffers = numBuffers;
	if (numBuffers > 0)
	{
		dst = DNew T[numBuffers];
		memcpy(dst, src, numBuffers * sizeof(T) );
	}
	else
		dst = NULL;
}

template<typename T, typename A>
inline	void	CopyDataToArray(std::vector<T, A> &dst, CONST T* src, UINT&  dstNumBuffers, UINT  numBuffers)
{
	dstNumBuffers = numBuffers;
	if (numBuffers > 0)
	{
		dst.resize(numBuffers);
		memcpy(&dst.front(), src, numBuffers * sizeof(T) );
	}
	else
		dst.clear();
}

template<typename T, typename A, typename Tr>
inline	void	CopyDataToArray(array<T, A, Tr> &dst, CONST T* src, UINT&  dstNumBuffers, UINT  numBuffers)
{
	dstNumBuffers = numBuffers;
	dst.assign(src, src + numBuffers);
}

template<typename T, size_t S>
inline	void	ZeroArray(T (&dst)[S])
{
	ZeroMemory(dst, S * sizeof(T));
}

template<typename T>
inline	void	ZeroArray(T* dst, size_t  size)
{
	ZeroMemory(dst, size * sizeof(T));
}

#define COPY_DATA_TO_ARRAY(array, num) CopyDataToArray(array##_, array, num##_, num)

inline const char GetShaderPipelineChar(SHADER_PIPELINE ShaderPipeline)
{
	switch(ShaderPipeline)
	{
	case SP_GS:
		return 'G';
	case SP_DS:
		return 'D';
	case SP_VS:
		return 'V';
	case SP_HS:
		return 'H';
	case SP_PS:
		return 'P';
	case SP_CS:
		return 'C';
	}
	return '?';
}

#include "..\Streamer\CmdFlowStreamers.h"

//#define EXECUTE_IMMEDIATELY_G1	// uncomment only for debug
#ifdef FINAL_RELEASE
//#define EXECUTE_IMMEDIATELY_G2
#define EXECUTE_IMMEDIATELY_G3
#else
//#define EXECUTE_IMMEDIATELY_G2
//#define EXECUTE_IMMEDIATELY_G3		// uncomment if you need skip some check commands
#endif


#ifdef ENABLE_THREAD_GUARD
#define THREAD_GUARD_D3D10()		CriticalSectionLocker locker( D3D10_GET_WRAPPER()->m_CSUMCall )			// FIXME!
#define THREAD_GUARD_DXGI()			CriticalSectionLocker locker( DXGI_GET_WRAPPER()->m_CSUMCall  )			// FIXME!
#define THREAD_GUARD(wrapper)		CriticalSectionLocker locker( wrapper->m_CSUMCall )
#else
#define THREAD_GUARD_D3D10()
#define THREAD_GUARD_DXGI()			
#define THREAD_GUARD(wrapper)
#endif

#ifdef ENABLE_TIMING
#define BEFORE_EXECUTE(x)	QueryPerformanceCounter( &x->StartTime_ );
#define AFTER_EXECUTE(x)	LARGE_INTEGER	endTime; QueryPerformanceCounter( &endTime );		\
	x->Duration_.QuadPart = endTime.QuadPart - x->StartTime_.QuadPart;
#else
#define BEFORE_EXECUTE(x)
#define AFTER_EXECUTE(x)
#endif

#define NEW_HANDLE(handle, size)	_ASSERT(size); handle.pDrvPrivate = DNew BYTE[size]
#define DELETE_HANDLE(handle)		delete [] handle.pDrvPrivate; handle.pDrvPrivate = NULL

namespace boost
{
	inline void intrusive_ptr_add_ref( Commands::Command* pCmd_ )
	{
		pCmd_->AddRef();
	}
	inline void intrusive_ptr_release( Commands::Command* pCmd_ )
	{
		pCmd_->Release();
	}
}

