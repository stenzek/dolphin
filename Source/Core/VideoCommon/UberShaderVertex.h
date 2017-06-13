// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "VideoCommon/PixelShaderGen.h"

namespace UberShader
{
// TODO: Look at combining the vertex/pixel UIDs into a single ubershader UID
// TODO: Worth having components as part of the UID?
#pragma pack(1)
struct vertex_ubershader_uid_data
{
  u32 num_texgens : 4;

  u32 NumValues() const { return sizeof(vertex_ubershader_uid_data); }
};
#pragma pack()

typedef ShaderUid<vertex_ubershader_uid_data> VertexShaderUid;

VertexShaderUid GetVertexShaderUid();

ShaderCode GenVertexShader(APIType api_type, const vertex_ubershader_uid_data* uid_data);

}