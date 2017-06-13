// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "VideoCommon/PixelShaderGen.h"

namespace UberShader
{
#pragma pack(1)
struct pixel_ubershader_uid_data
{
  // Nice and simple

  // This is the current state, not to be confused with the final state.
  // Currently: 32 different ubershaders
  u32 num_texgens : 4;
  u32 early_depth : 1;

  u32 NumValues() const { return sizeof(pixel_ubershader_uid_data); }
};
#pragma pack()

typedef ShaderUid<pixel_ubershader_uid_data> PixelShaderUid;

PixelShaderUid GetPixelShaderUid();

ShaderCode GenPixelShader(APIType ApiType, const pixel_ubershader_uid_data* uid_data);
}