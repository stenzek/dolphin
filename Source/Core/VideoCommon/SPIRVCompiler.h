// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <cstddef>
#include <vector>

#include "VideoCommon/VideoCommon.h"

namespace VideoCommon::SPIRVCompiler
{
// SPIR-V compiled code type
using CodeVector = std::vector<u32>;

// Compile a vertex shader to SPIR-V.
bool CompileVertexShader(CodeVector* out_code, APIType api_type, const char* source_code,
                         size_t source_code_length);

// Compile a geometry shader to SPIR-V.
bool CompileGeometryShader(CodeVector* out_code, APIType api_type, const char* source_code,
                           size_t source_code_length);

// Compile a fragment shader to SPIR-V.
bool CompileFragmentShader(CodeVector* out_code, APIType api_type, const char* source_code,
                           size_t source_code_length);

// Compile a compute shader to SPIR-V.
bool CompileComputeShader(CodeVector* out_code, APIType api_type, const char* source_code,
                          size_t source_code_length);

}  // namespace VideoCommon::SPIRVCompiler
