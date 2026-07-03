/*
 * S3DAPI/LlamaXML/XMLWriter.h - wiz3D LGPL 2.1 reimplementation of
 * LlamaXML's XMLWriter, backed by ticpp (MIT). See XMLException.h header
 * for the rationale + licensing notes on this shim.
 *
 * Match LlamaXML's writer API surface:
 *   - ctor(FileOutputStream&, TextEncoding)
 *   - StartDocument() / EndDocument()
 *   - StartElement(name) / EndElement()
 *   - WriteAttribute<T>(name, value) — templated, accepts int / float / string / etc.
 *   - WriteComment(text)
 *
 * Implementation strategy: build the document in memory (ticpp::Document)
 * with a stack of currently-open elements. EndDocument flushes to disk
 * via ticpp::Document::SaveFile().
 */

#pragma once

#include "FileOutputStream.h"
#include "TextEncoding.h"
#include "XMLException.h"

#include <memory>
#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>

// ticpp pulls in tinyxml; both ship in lib/ticpp/include/.
// For WideCharToMultiByte in the std::wstring WriteAttribute overload.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ticpp.h"

namespace LlamaXML {

class XMLWriter
{
public:
	// LlamaXML signature: XMLWriter(OutputStream& output, TextEncoding encoding)
	// We hold a reference to the FileOutputStream because we need its
	// filename at EndDocument() time. Encoding is ignored — we always
	// emit UTF-8, which is what every caller in wiz3D uses anyway.
	XMLWriter(FileOutputStream& output, TextEncoding /*encoding*/)
		: m_output(output)
		, m_saved(false)
	{
		m_doc.reset(new ticpp::Document());
	}

	~XMLWriter()
	{
		// Defensive: if the caller forgot to call EndDocument(), still
		// flush — original LlamaXML behaviour. Don't propagate exceptions
		// from destructors though.
		try { if (!m_saved) EndDocument(); } catch (...) {}
	}

	// ----------------- LlamaXML public API -----------------

	void StartDocument()
	{
		// Add the <?xml version="1.0" encoding="UTF-8"?> declaration.
		// Safe to call multiple times — LlamaXML's behaviour was a no-op
		// after the first; ticpp would dupe it, so guard.
		if (m_doc->NoChildren())
		{
			ticpp::Declaration decl("1.0", "UTF-8", "");
			m_doc->InsertEndChild(decl);
		}
	}

	// LlamaXML 3-arg form: StartDocument(version, encoding, standalone).
	// Used by XMLStreamer / CommandDumper as `StartDocument("1.0", NULL, NULL)`
	// to pin a specific version. We accept any of these as `const char*` —
	// NULL is treated as "omit". Encoding stays UTF-8 (see TextEncoding.h
	// for why we don't actually honour caller-requested encodings).
	void StartDocument(const char* version, const char* /*encoding*/, const char* standalone)
	{
		if (m_doc->NoChildren())
		{
			ticpp::Declaration decl(version ? version : "1.0",
			                        "UTF-8",
			                        standalone ? standalone : "");
			m_doc->InsertEndChild(decl);
		}
	}

	void EndDocument()
	{
		if (m_saved) return;
		// ticpp's SaveFile takes a std::string. The FileOutputStream we
		// were handed holds a wide-char path; narrow it. Filenames here
		// are always plain ASCII in practice (config XML in the game dir),
		// so a simple narrowing conversion is safe.
		const std::wstring& wpath = m_output.GetFileName();
		std::string apath(wpath.begin(), wpath.end());
		m_doc->SaveFile(apath);
		m_saved = true;
	}

	// Open a new element as the child of the current element (or root if
	// none are open). The new element becomes the current target for
	// WriteAttribute / WriteComment / nested StartElement.
	void StartElement(const char* name)
	{
		if (!name) name = "";
		// Pre-flight: ensure the XML declaration exists so callers that
		// skip StartDocument() still produce valid XML (matches LlamaXML's
		// laxer behaviour).
		if (m_doc->NoChildren()) StartDocument();

		// We need a heap-allocated element that ticpp's container hangs
		// onto. ticpp::Element's InsertEndChild model deep-copies, so we
		// hold owned scratch instances on a stack and rebind via the
		// inserted node pointer returned by InsertEndChild.
		ticpp::Element scratch(name);
		ticpp::Node* inserted = nullptr;
		if (m_openStack.empty())
			inserted = m_doc->InsertEndChild(scratch);
		else
			inserted = m_openStack.back()->InsertEndChild(scratch);
		// InsertEndChild returns ticpp::Node*; cast back to Element*
		// (we just inserted an Element so the cast is sound).
		m_openStack.push_back(static_cast<ticpp::Element*>(inserted));
	}

	void EndElement()
	{
		if (!m_openStack.empty()) m_openStack.pop_back();
	}

	void WriteComment(const char* text)
	{
		if (!text) text = "";
		ticpp::Comment c(text);
		if (m_openStack.empty())
			m_doc->InsertEndChild(c);
		else
			m_openStack.back()->InsertEndChild(c);
	}

	// Templated attribute writer. Delegate straight to ticpp::Element's
	// SetAttribute<T> — it handles all the integer / float / string types
	// LlamaXML supported, via its own ToString helper that uses
	// std::stringstream internally. Avoids us reimplementing the type
	// dispatch (and the C2678 errors we'd hit on types ostringstream
	// doesn't take, like void* or arrays of unknown size).
	template <typename T>
	void WriteAttribute(const char* name, const T& value)
	{
		if (m_openStack.empty()) throw XMLException("WriteAttribute called with no open element");
		m_openStack.back()->SetAttribute(std::string(name ? name : ""), value);
	}

	// Overload for string literals / const char* — avoid ostringstream
	// boxing the pointer instead of the contents.
	void WriteAttribute(const char* name, const char* value)
	{
		if (m_openStack.empty()) throw XMLException("WriteAttribute called with no open element");
		m_openStack.back()->SetAttribute(std::string(name ? name : ""),
		                                 std::string(value ? value : ""));
	}

	void WriteAttribute(const char* name, const std::string& value)
	{
		if (m_openStack.empty()) throw XMLException("WriteAttribute called with no open element");
		m_openStack.back()->SetAttribute(std::string(name ? name : ""), value);
	}

	// Overload for std::wstring — ticpp's ToString uses std::stringstream
	// which has no operator<<(wstring), so we have to narrow ourselves
	// before forwarding. XMLStreamer passes app names this way (line 40 of
	// XMLStreamer.cpp). UTF-8 encode via Windows API so non-ASCII names
	// round-trip correctly.
	void WriteAttribute(const char* name, const std::wstring& value)
	{
		if (m_openStack.empty()) throw XMLException("WriteAttribute called with no open element");
		std::string narrow;
		if (!value.empty())
		{
			int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(),
			                                 nullptr, 0, nullptr, nullptr);
			if (needed > 0)
			{
				narrow.resize(needed);
				WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(),
				                    &narrow[0], needed, nullptr, nullptr);
			}
		}
		m_openStack.back()->SetAttribute(std::string(name ? name : ""), narrow);
	}

private:
	FileOutputStream&                m_output;
	std::unique_ptr<ticpp::Document> m_doc;
	std::vector<ticpp::Element*>     m_openStack;
	bool                             m_saved;
};

} // namespace LlamaXML
