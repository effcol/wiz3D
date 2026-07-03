/*
 * S3DAPI/LlamaXML/TextEncoding.h - wiz3D LGPL 2.1 reimplementation of
 * LlamaXML's TextEncoding class. See XMLException.h header for the full
 * rationale + licensing notes on this shim.
 *
 * The wiz3D callers only use TextEncoding::UTF8() and TextEncoding::Application()
 * as static factory constants passed to XMLReader / GetAttribute<encoding>
 * overloads. Our ticpp backend treats everything as UTF-8 internally
 * (TinyXML's default), so this shim collapses both factories to the same
 * tag value and ignores it downstream.
 */

#pragma once

namespace LlamaXML {

class TextEncoding
{
public:
	// We don't actually need to distinguish encodings — ticpp/TinyXML uses
	// UTF-8 throughout, and that matches both the on-disk XML and the
	// std::string interfaces wiz3D's callers expect. The factories exist
	// only to satisfy the LlamaXML API shape.
	static TextEncoding UTF8()        { return TextEncoding(); }
	static TextEncoding Application() { return TextEncoding(); }
	// LlamaXML's "system" encoding factory — typically the OS default code
	// page on Windows. ticpp/TinyXML reads/writes UTF-8 throughout, which
	// is a safe superset for the ASCII attributes wiz3D uses; tag here
	// keeps the LlamaXML API shape intact.
	static TextEncoding System()      { return TextEncoding(); }
};

} // namespace LlamaXML
