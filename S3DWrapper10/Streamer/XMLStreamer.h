#ifndef __S3D_XMLSTREAMER_H__
#define __S3D_XMLSTREAMER_H__

#include "LlamaXML/XMLReader.h"
#include "LlamaXML/XMLWriter.h"
#include "LlamaXML/FileInputStream.h"
#include "LlamaXML/FileOutputStream.h"

#include "../S3DWrapper10/Array.h"
#include <d3d10umddi.h>
#include "ResourceManager.h"
#include "TypePtr.h"
#include "boost_singleton_compat.h"
#include <list>

//////////////////////////////////////////////////////////////////////////

namespace Commands
{
	class Command;
}

//////////////////////////////////////////////////////////////////////////

class XMLStreamer
{	
public:

	typedef enum
	{
		TXML_UnknownMode,
		TXML_OpenForWrite,
		TXML_OpenForRead,
	}
	TXMLFileMode;
												XMLStreamer			( TXMLFileMode mode );
	virtual										~XMLStreamer		();

	bool										OpenFile			( const wchar_t* fileName, const char* szComment = NULL );

	bool										IsWriterOK			();

public:

	void										SetCurrentRM		( ResourceManager * pNewRM );

	ResourceManager *							GetCurrentRM		()	const;

	void										DeviceStateBlockBegin();
	void										DeviceStateBlockEnd();

	void										WriteCommandBegin	( Commands::Command* pCmd, const char* szCommandName );
	void										WriteCommandEnd		();

	void										WriteComment		( const char* Val );

	template <typename T>
	void										WriteValue			( const char* szAttribute, const T& nVal );

	template <typename T>
	void										WriteValueWithComment( const char* szAttribute, const T& nVal );

	/*template <typename T>
	void										WriteRefValue_		( const char* szAttribute, const T* pVal );*/

	template <typename T>
	void										WriteValueByRef		( const char* szAttribute, const T* pVal );

	template <typename T>
	void										WriteReference		( const char* szAttribute, const T&	pVal  );

	template<typename T>
	void										WriteArrayValues	( const char* szAttribute, SIZE_T nCount, const T*  pArray );

	template<typename T, typename A>
	void										WriteArrayValues	( const char* szAttribute, SIZE_T nCount, const std::vector<T, A>  &pArray );

	template<typename T, typename A, typename Tr>
	void										WriteArrayValues	( const char* szAttribute, SIZE_T nCount, const array<T, A, Tr>  &pArray );

	template<typename T>
	void										WriteArrayReferences( const char* szAttribute, SIZE_T nCount, const T*  pArray );

	template<typename T, typename A>
	void										WriteArrayReferences( const char* szAttribute, SIZE_T nCount, const std::vector<T, A>  &pArray );

	template<typename T, typename A, typename Tr>
	void										WriteArrayReferences( const char* szAttribute, SIZE_T nCount, const array<T, A, Tr>  &pArray );

	void										StartNewElement		( const char* szName );
	void										EndElement			( );

	void										Skip				( );

	Commands::Command*							ReadNextCommand		( std::string& _sError );

	template <typename T>
	void										ReadValue			( T& val );

	template <typename T>
	void										ReadValueByRef		( T*& val );

	template <typename T>
	void										ReadValueByRef		( const T*& val );

	template <typename T>
	void										ReadValuePtr		( const T*& val );

	template<typename T>
	void										ReadReference		( T& val );	

	template<typename T>
	void										ReadArrayValues_		( SIZE_T nCount, T  pArray[] );

	template<typename T>
	void										ReadArrayValues		( SIZE_T nCount, T  *&pArray );

	template<typename T, typename A>
	void										ReadArrayValues		( SIZE_T nCount, std::vector<T, A>  &pArray );

	template<typename T, typename A, typename Tr>
	void										ReadArrayValues		( SIZE_T nCount, array<T, A, Tr>  &pArray );

	template<typename T>
	void										ReadArrayReferences	( SIZE_T _nCount, T*  _pArray );

	template<typename T, typename A>
	void										ReadArrayReferences	( SIZE_T _nCount, std::vector<T, A>  &_pArray );

	template<typename T, typename A, typename Tr>
	void										ReadArrayReferences	( SIZE_T _nCount, array<T, A, Tr>  &_pArray );

	void										ReadStartElement	() const;
	void										ReadEndElement		() const;

	void										FreeAllocatedMem	();

protected:

	template<typename T> void					InternalAllocateMem	( T*& pVal_ )
	{
		CTypePtr* ptr = DNew CValPtr< T >( pVal_ );
		m_vAllocatedMem.push_back( ptr );
	}

	template<typename T> void					InternalAllocateMem	( T*& pArray_, SIZE_T nArraySize_ )
	{
		CTypePtr* ptr = DNew CArrayPtr< T >( pArray_, nArraySize_ );
		m_vAllocatedMem.push_back( ptr );
	}
	template < class T > std::string ToString( const T& value ) const;
	std::string ToString( const std::string& value ) const;

	template < class T > void FromString( const std::string& temp, T* out ) const;
	void FromString( const std::string& temp, std::string* out ) const;
public:
	LlamaXML::XMLWriter*						m_pXMLWriter;
	LlamaXML::XMLReader*						m_pXMLReader;

	std::string m_appName;

private:

	TXMLFileMode								m_tFileMode;

	Commands::Command*							m_pCurrCmd;
	LARGE_INTEGER								m_Freq;

	LlamaXML::FileOutputStream*					m_pOutputStream;
	LlamaXML::FileInputStream*					m_pInputStream;

	bool										m_bIterateAllCmd;
	int											m_nLastSuccessfulEID;

	ResourceManager *							m_pCurrentRM;			// current ResourceManager for WRITE_REFERENCE macro

private:
	std::list< CTypePtr* >						m_vAllocatedMem;
};

//////////////////////////////////////////////////////////////////////////

#include "XMLStreamer_WriteValue.h"
#include "XMLStreamer_ReadValue.h"

//////////////////////////////////////////////////////////////////////////

#endif//__S3D_XMLSTREAMER_H__