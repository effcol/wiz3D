/*
 * S3DAPI/LlamaXML/XMLReader.h - wiz3D LGPL 2.1 reimplementation of
 * LlamaXML's XMLReader, backed by ticpp (MIT). See XMLException.h header
 * for the rationale + licensing notes on this shim.
 *
 * LlamaXML's XMLReader is a SAX-like streaming reader. We emulate that
 * over a DOM tree (ticpp::Document) by tracking two pieces of state:
 *
 *   - m_openStack[] — the elements the caller has called ReadStartElement
 *     on but not yet ReadEndElement. The most recently opened element is
 *     m_openStack.back(); this is the element that GetAttribute / etc.
 *     query against.
 *
 *   - m_cursor — the next element the caller will see if they call
 *     ReadStartElement() with no name (or IsStartElement() to peek).
 *     Advances on every Read*Element / Skip. NULL when at end-of-document
 *     or at end of current parent's children.
 *
 * Semantics of the key methods, traced from a typical caller pattern:
 *
 *   while (reader.IsStartElement("Items")) {       // peek: cursor == <Items>?
 *       reader.ReadStartElement("Items");          // push cursor, descend
 *       while (reader.IsStartElement("Item")) {    // any child <Item> left?
 *           reader.ReadStartElement("Item");       // push, descend
 *           int x = reader.GetIntAttribute("x");   // query open element
 *           reader.ReadEndElement();               // pop, advance sibling
 *       }
 *       reader.ReadEndElement();                   // pop Items, advance sibling
 *   }
 *
 * Match LlamaXML's reader API surface:
 *   - ctor(FileInputStream&, TextEncoding)
 *   - ReadStartElement() / ReadStartElement(name)
 *   - ReadEndElement()
 *   - IsStartElement() / IsStartElement(name)
 *   - Skip() — advance past current element + its children
 *   - GetAttribute(name) / GetIntAttribute(name, def) / GetBoolAttribute(name, def)
 *   - GetFloatAttribute(name, def) / GetDoubleAttribute(name, def)
 *   - GetAttributeValue<T>(name, def)
 *   - GetLocalName() — name of current element (whichever ReadStartElement just opened)
 *   - HasAttribute(name)
 *   - IsEmptyElement()
 */

#pragma once

#include "FileInputStream.h"
#include "TextEncoding.h"
#include "XMLException.h"

#include <memory>
#include <string>
#include <vector>
#include <sstream>

// Original LlamaXML headers transitively included <windows.h> via
// PlatformConfig.h. Callers in DX9GenerateCodeFromDump/Command.h rely on
// Windows macros (TRUE, BOOL) being defined wherever they include our
// XMLReader.h, so preserve that transitive include or callers fail with
// C2065 'TRUE': undeclared identifier.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// Also pull in <shellapi.h> — DX9GenerateCodeFromDump.cpp calls ShellExecute
// and used to get this transitively through LlamaXML's PlatformConfig.h.
// WIN32_LEAN_AND_MEAN excludes shellapi from windows.h, so include it
// explicitly here.
#include <shellapi.h>

#include "ticpp.h"

namespace LlamaXML {

class XMLReader
{
public:
	// LlamaXML's low-level NodeType. We expose just the subset wiz3D
	// callers actually use: kElement (entering an element), kEndElement
	// (leaving one), kNone (start-of-document / end-of-document / between
	// states). kText / kComment / kWhitespace exist for source-compat
	// but never fire in our DOM-backed implementation — the underlying
	// ticpp DOM doesn't surface text/comment nodes through the cursor we
	// maintain.
	enum NodeType {
		kNone,
		kElement,
		kEndElement,
		kText,
		kComment,
		kWhitespace,
		kXmlDeclaration
	};

	XMLReader(FileInputStream& input, TextEncoding /*encoding*/)
		: m_nodeType(kNone)
	{
		m_doc.reset(new ticpp::Document());
		const std::wstring& wpath = input.GetFileName();
		std::string apath(wpath.begin(), wpath.end());
		try {
			m_doc->LoadFile(apath);
		} catch (const ticpp::Exception& e) {
			throw XMLException(e.what());
		}
		// Start cursor at the first element of the document (skip the
		// declaration / comments / whitespace).
		m_cursor = m_doc->FirstChildElement(false);
		// Initial node type reflects whether we have any elements.
		// LlamaXML callers typically check IsStartElement() / GetNodeType()
		// before doing anything; both work without an explicit Read() since
		// we pre-position on the first element.
		m_nodeType = m_cursor ? kElement : kNone;
	}

	~XMLReader() = default;

	// ----------------- Peek API -----------------

	// XMLStreamer mixes the high-level (IsStartElement / ReadStartElement)
	// and low-level (GetNodeType + Read) interfaces. Tie IsStartElement to
	// both — m_nodeType must be kElement AND m_cursor non-null — so callers
	// that switched in via Read() still report consistent peek state.
	bool IsStartElement() const { return m_nodeType == kElement && m_cursor != nullptr; }

	bool IsStartElement(const char* name) const
	{
		if (!IsStartElement() || !name) return false;
		return m_cursor->Value() == name;
	}

	bool IsEmptyElement() const
	{
		// "Empty" in LlamaXML semantics = no children. We check the
		// element we just opened (top of stack).
		if (m_openStack.empty()) return true;
		return m_openStack.back()->NoChildren();
	}

	// ----------------- Read API: descend / ascend / skip -----------------

	// Descend into the current cursor element. After this, cursor points
	// at the first child element (if any). m_nodeType updated so callers
	// mixing high-level Read*Element with low-level GetNodeType stay
	// consistent — see comment on IsStartElement.
	void ReadStartElement()
	{
		if (!m_cursor) throw XMLException("ReadStartElement: no element at cursor");
		m_openStack.push_back(m_cursor);
		m_cursor = m_cursor->FirstChildElement(false);
		m_nodeType = m_cursor ? kElement : kEndElement;
	}

	void ReadStartElement(const char* name)
	{
		if (!m_cursor) throw XMLException("ReadStartElement: no element at cursor");
		if (name && m_cursor->Value() != name) {
			std::string msg = "ReadStartElement: expected <";
			msg += name;
			msg += ">, found <";
			msg += m_cursor->Value();
			msg += ">";
			throw XMLException(msg);
		}
		ReadStartElement();
	}

	// Ascend out of the current element. Cursor moves to the parent's
	// next sibling.
	void ReadEndElement()
	{
		if (m_openStack.empty()) throw XMLException("ReadEndElement: no open element");
		ticpp::Element* closing = m_openStack.back();
		m_openStack.pop_back();
		m_cursor = closing->NextSiblingElement(false);
		m_nodeType = m_cursor ? kElement : (m_openStack.empty() ? kNone : kEndElement);
	}

	// Skip past the cursor's element (and its children) — move directly
	// to the next sibling. Used to ignore unknown / unwanted elements.
	void Skip()
	{
		if (!m_cursor) return;
		m_cursor = m_cursor->NextSiblingElement(false);
		m_nodeType = m_cursor ? kElement : (m_openStack.empty() ? kNone : kEndElement);
	}

	// ----------------- Low-level streaming API (LlamaXML SAX-style) -----------------
	//
	// XMLStreamer / DX9GenerateCodeFromDump use the low-level loop:
	//     while (reader.Read()) {
	//         switch (reader.GetNodeType()) {
	//             case kElement: ...
	//             case kEndElement: ...
	//         }
	//     }
	// We emulate this over the DOM by walking elements depth-first and
	// alternating kElement (entering) → kEndElement (leaving), using the
	// open-stack to track descent. kText/kComment/kWhitespace are declared
	// for compile-compat but never reported — ticpp doesn't surface those
	// node types through our element-only cursor.

	NodeType GetNodeType() const { return m_nodeType; }

	bool EndOfFile() const { return m_nodeType == kNone && m_cursor == nullptr && m_openStack.empty(); }

	// Advance to the next "event". Returns true on success, false at EOF.
	bool Read()
	{
		// State machine:
		//   kElement on m_cursor → descend if children; else emit kEndElement
		//     for the same element on next call.
		//   kEndElement → next sibling becomes new kElement; if no sibling,
		//     pop stack and emit kEndElement for parent.
		if (m_nodeType == kElement && m_cursor)
		{
			ticpp::Element* first = m_cursor->FirstChildElement(false);
			if (first)
			{
				m_openStack.push_back(m_cursor);
				m_cursor = first;
				m_nodeType = kElement;
				return true;
			}
			// Self-closing / empty element: emit its kEndElement without descent.
			m_openStack.push_back(m_cursor);
			m_cursor = nullptr;
			m_nodeType = kEndElement;
			return true;
		}
		if (m_nodeType == kEndElement)
		{
			// Pop the element we just closed; advance to its next sibling.
			if (m_openStack.empty()) { m_nodeType = kNone; return false; }
			ticpp::Element* closed = m_openStack.back();
			m_openStack.pop_back();
			ticpp::Element* sib = closed->NextSiblingElement(false);
			if (sib)
			{
				m_cursor = sib;
				m_nodeType = kElement;
				return true;
			}
			// No sibling — emit kEndElement for the parent now (if any).
			if (!m_openStack.empty())
			{
				m_cursor = nullptr;
				m_nodeType = kEndElement;
				return true;
			}
			// Top of tree, no more — end of document.
			m_cursor = nullptr;
			m_nodeType = kNone;
			return false;
		}
		// kNone or invalid — nothing more to read.
		return false;
	}

	// MoveToContent: in real LlamaXML this skips comments / whitespace and
	// stops on the next kElement / kEndElement / kText. Since we only
	// report element events, the current state is already "content".
	NodeType MoveToContent() { return m_nodeType; }

	// ----------------- Element queries (operate on top of openStack) -----------------

	// LlamaXML's GetLocalName returns const UnicodeString& — a wide-string
	// type. Callers do `std::wstring x = reader.GetLocalName().c_str()` and
	// pass `.c_str()` to ATL's CW2AEX (which expects LPCWSTR). Returning
	// std::wstring matches that contract.
	std::wstring GetLocalName() const
	{
		const std::string utf8 = GetLocalNameUtf8();
		// Pure ASCII passthrough — element names in wiz3D's XML are always
		// ASCII identifiers (command class names, attribute names). This
		// avoids dragging in MultiByteToWideChar for what's effectively a
		// no-op widen. If a non-ASCII byte ever shows up here, it'll get
		// reinterpreted but caller is exceedingly unlikely to care since
		// nothing downstream interprets the wstring beyond identifier
		// comparison.
		std::wstring out;
		out.reserve(utf8.size());
		for (char c : utf8) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
		return out;
	}

	// Internal: raw UTF-8 element name from ticpp.
	std::string GetLocalNameUtf8() const
	{
		if (m_openStack.empty()) {
			// LlamaXML lets you query before ReadStartElement when peeking;
			// fall back to cursor in that case.
			if (m_cursor) return m_cursor->Value();
			return std::string();
		}
		return m_openStack.back()->Value();
	}

	bool HasAttribute(const char* name) const
	{
		if (m_openStack.empty() || !name) return false;
		return m_openStack.back()->HasAttribute(std::string(name));
	}

	// Returns "" if the attribute is missing — matches LlamaXML's
	// GetAttribute(name) which returns an empty UnicodeString in that case.
	std::string GetAttribute(const char* name) const
	{
		if (m_openStack.empty() || !name) return std::string();
		std::string val;
		try {
			m_openStack.back()->GetAttribute(name, &val, false);
		} catch (...) {
			return std::string();
		}
		return val;
	}

	// Overload that takes a TextEncoding (LlamaXML callers use this for
	// the explicit-encoding variant). We ignore encoding — always UTF-8.
	std::string GetAttribute(const char* name, TextEncoding /*enc*/) const
	{
		return GetAttribute(name);
	}

	int GetIntAttribute(const char* name, int defVal = 0) const
	{
		std::string s = GetAttribute(name);
		if (s.empty()) return defVal;
		int ival = defVal;
		std::istringstream iss(s);
		iss >> ival;
		return iss.fail() ? defVal : ival;
	}

	bool GetBoolAttribute(const char* name, bool defVal = false) const
	{
		int ival = GetIntAttribute(name, defVal ? 1 : 0);
		return ival != 0;
	}

	double GetDoubleAttribute(const char* name, double defVal = 0.0) const
	{
		std::string s = GetAttribute(name);
		if (s.empty()) return defVal;
		double dval = defVal;
		std::istringstream iss(s);
		iss >> dval;
		return iss.fail() ? defVal : dval;
	}

	float GetFloatAttribute(const char* name, float defVal = 0.0f) const
	{
		return static_cast<float>(GetDoubleAttribute(name, defVal));
	}

	template <typename T>
	T GetAttributeValue(const char* name, T defValue) const
	{
		std::string s = GetAttribute(name);
		if (s.empty()) return defValue;
		T outValue = defValue;
		std::istringstream iss(s);
		iss >> outValue;
		return iss.fail() ? defValue : outValue;
	}

private:
	std::unique_ptr<ticpp::Document> m_doc;
	ticpp::Element*                  m_cursor = nullptr;
	std::vector<ticpp::Element*>     m_openStack;
	NodeType                         m_nodeType = kNone;
};

} // namespace LlamaXML
