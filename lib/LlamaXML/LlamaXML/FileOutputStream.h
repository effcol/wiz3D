/*
 * S3DAPI/LlamaXML/FileOutputStream.h - wiz3D LGPL 2.1 reimplementation of
 * LlamaXML's FileOutputStream. See XMLException.h header for the rationale.
 *
 * Companion to FileInputStream. Holds the wide-char filename; the
 * corresponding XMLWriter's destructor (or explicit EndDocument()) calls
 * ticpp::Document::SaveFile against it.
 */

#pragma once

#include <string>

namespace LlamaXML {

class FileOutputStream
{
public:
	explicit FileOutputStream(const wchar_t* fileName)
		: m_fileName(fileName ? fileName : L"")
	{}

	const std::wstring& GetFileName() const { return m_fileName; }

private:
	std::wstring m_fileName;
};

} // namespace LlamaXML
