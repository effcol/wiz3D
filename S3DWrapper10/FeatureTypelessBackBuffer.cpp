/*
* Project : wiz3D
* Copyright (C) Akshay Hegde 2026
*/

// <TypelessBackBuffer Value="1"/>: the back-buffer stand-ins are typeless, so an _SRGB view of the swap chain's UNORM format
// works as on a flip-model buffer; GetDesc and views without a desc keep the swap chain's format.

#include "StdAfx.h"
#include "ProfileFeature.h"

namespace wiz3d
{
namespace
{

class TypelessBackBufferFeature : public ProfileFeature
{
};

} // namespace

bool TypelessBackBufferOn()
{
    static const bool s_on = ProfileSwitchOn("TypelessBackBuffer");
    return s_on;
}

std::unique_ptr<ProfileFeature> MakeTypelessBackBuffer(const ProfileEntry* e)
{
    if (!e || EntryValue(*e, "Value") == "0") return nullptr;
    return std::unique_ptr<ProfileFeature>(new TypelessBackBufferFeature());
}

} // namespace wiz3d
