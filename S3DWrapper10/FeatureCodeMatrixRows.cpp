/*
* Project : wiz3D
* Copyright (C) Akshay Hegde 2026
*/

// <CodeMatrixRows Value="1"/>: a matrix CodeMatrices found from dp4 reads is classified as the rows it is read as, not in
// whichever orientation first looks like a camera (an axis-aligned camera-relative one passes both).

#include "StdAfx.h"
#include "ProfileFeature.h"

namespace wiz3d
{
namespace
{

class CodeMatrixRowsFeature : public ProfileFeature
{
};

} // namespace

bool CodeMatrixRowsOn()
{
    static const bool s_on = ProfileSwitchOn("CodeMatrixRows");
    return s_on;
}

std::unique_ptr<ProfileFeature> MakeCodeMatrixRows(const ProfileEntry* e)
{
    if (!e || EntryValue(*e, "Value") == "0") return nullptr;
    return std::unique_ptr<ProfileFeature>(new CodeMatrixRowsFeature());
}

} // namespace wiz3d
