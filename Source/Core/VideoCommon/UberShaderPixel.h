// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "VideoCommon/PixelShaderGen.h"

namespace UberShader
{
ShaderCode GenPixelShader(APIType ApiType, bool per_pixel_depth, bool dual_src_blend, bool msaa,
                          bool ssaa);
}