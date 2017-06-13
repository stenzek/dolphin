// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/VideoCommon.h"

namespace UberShader
{
// Common functions across all ubershaders
void WriteUberShaderCommonHeader(ShaderCode& out, APIType api_type);

// bitfieldExtract generator for BitField types
template <typename T>
std::string BitfieldExtract(const std::string& source, T type)
{
  return StringFromFormat("bitfieldExtract(%s, %u, %u)", source.c_str(),
                          static_cast<u32>(type.StartBit()), static_cast<u32>(type.NumBits()));
}

// TODO: A single ubershader uid for vertex, geometry, pixel combinations
// This means more compilations of shader stages, but simpler tracking and lookup
// For GL/Vulkan, the full compile is done at program link/pipeline creation time anyway..
// Ignoring host state such as stereo, wireframe, MSAA, SSAA, as these will require invalidating the shader cache anyway.
// Total of 128 ubershaders, but we don't necessarily need to precompile every combination, or we can do it in the background.
#pragma pack(1)
struct ubershader_uid_data
{
  u32 num_texgens : 4;    // V/G/P
  u32 early_depth : 1;    // P
  u32 primitive_type : 2; // G

  u32 NumValues() const { return sizeof(ubershader_uid_data); }
};
#pragma pack()
}   // namespace UberShader