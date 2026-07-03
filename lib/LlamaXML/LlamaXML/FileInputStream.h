/*
 * S3DAPI/LlamaXML/FileInputStream.h - wiz3D LGPL 2.1 reimplementation of
 * LlamaXML's FileInputStream. See XMLException.h header for the rationale.
 *
 * In the original LlamaXML, FileInputStream is a subclass of InputStream
 * passed to XMLReader so the reader can pull bytes during parse. ticpp
 * loads the whole file via Document::LoadFile(filename), so our shim
 * just holds the filename — the XMLReader pulls it out when constructing
 * the underlying ticpp::Document.
 */

#pragma once

#include <string>

namespace LlamaXML {

class FileInputStream
{
public:
	// LlamaXML accepts a wide-char filename (Windows-only API). Match the
	// signature exactly so existing call sites compile unchanged.
	explicit FileInputStream(const wchar_t* fileName)
		: m_fileName(fileName ? fileName : L"")
	{}

	// LlamaXML also accepted narrow filenames in older codepaths — match.
	// Used by DX9GenerateCodeFromDump which opens "./CommandFlow.xml".
	explicit FileInputStream(const char* fileName)
	{
		if (fileName)
			while (*fileName) m_fileName.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*fileName++)));
	}

	const std::wstring& GetFileName() const { return m_fileName; }

private:
	std::wstring m_fileName;
};

} // namespace LlamaXML
