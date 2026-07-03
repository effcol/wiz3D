/*
 * S3DAPI/LlamaXML/XMLException.h - wiz3D LGPL 2.1 reimplementation of
 * LlamaXML's XMLException class. Source-compatible drop-in replacement for
 * <LlamaXML/XMLException.h> so existing callers (S3DWrapper9/CommandDumper,
 * S3DWrapper10/Streamer, DX9GenerateCodeFromDump) don't need to change.
 *
 * Replaces the GPL v2 lib/LlamaXML/ dependency. APIs are not copyrightable
 * (Google v. Oracle); this re-implementation owes nothing to LlamaXML's
 * source, only matches its public surface.
 *
 * Copyright (C) 2026 wiz3D contributors
 * Licensed under LGPL 2.1, same as the rest of wiz3D.
 */

#pragma once

#include <stdexcept>
#include <string>

namespace LlamaXML {

class XMLException : public std::runtime_error
{
public:
	explicit XMLException(const char* what)        : std::runtime_error(what)            {}
	explicit XMLException(const std::string& what) : std::runtime_error(what.c_str())    {}
};

} // namespace LlamaXML
