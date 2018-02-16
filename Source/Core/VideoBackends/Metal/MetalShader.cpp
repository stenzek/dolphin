// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <fstream>
#include <sstream>

#include "Common/Assert.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "VideoBackends/Metal/MetalContext.h"
#include "VideoBackends/Metal/MetalShader.h"

static const std::string SHADER_HEADER = R"(
#include <metal_common>
#include <metal_geometric>
#include <metal_graphics>
#include <metal_math>
#include <metal_matrix>
#include <metal_stdlib>
#include <metal_texture>
using namespace metal;

template<typename T>
T frac(T val) { return fract(val); }
template<typename T, typename F>
T lerp(T lower, T upper, F factor) { return mix(lower, upper, factor); }

// Metal doesn't have sign() for integer types.
// TODO: Is there a better way to do this?
int sign(int val) { return val < 0 ? -1 : (val == 0 ? 0 : 1); }
int2 sign(int2 val) { return int2(sign(val.x), sign(val.y)); }
int3 sign(int3 val) { return int3(sign(val.x), sign(val.y), sign(val.z)); }
int4 sign(int4 val) { return int4(sign(val.x), sign(val.y), sign(val.z), sign(val.w)); }
)";

namespace Metal
{
MetalShader::MetalShader(ShaderStage stage, mtlpp::Library library, mtlpp::Function function)
    : AbstractShader(stage), m_library(library), m_function(function)
{
}

MetalShader::~MetalShader()
{
}

mtlpp::Library MetalShader::GetLibrary() const
{
  return m_library;
}

mtlpp::Function MetalShader::GetFunction() const
{
  return m_function;
}

bool MetalShader::HasBinary() const
{
  return false;
}

AbstractShader::BinaryData MetalShader::GetBinary() const
{
  return {};
}

static void LogCompileError(const char* type, const char* part, const std::string& source,
                            const ns::Error& err)
{
  std::stringstream ss;
  ss << "Failed to compile Metal " << type << " shader to " << part << ".\n";
  ss << "Error Code: " << err.GetCode() << " Domain: " << err.GetDomain().GetCStr() << "\n";
  ss << "Error Message: " << err.GetLocalizedDescription().GetCStr() << "\n";
  ss << "Shader Source:\n" << source << "\n";
  ERROR_LOG(VIDEO, "%s", ss.str().c_str());

  static int counter = 0;
  std::string filename =
      StringFromFormat("%sbad_%s_%04i.txt", File::GetUserPath(D_DUMP_IDX).c_str(), type, counter++);
  std::ofstream stream;
  File::OpenFStream(stream, filename, std::ios::out);
  if (stream.good())
    stream << ss.str();
}

std::unique_ptr<MetalShader> MetalShader::CreateFromSource(ShaderStage stage,
                                                           const std::string& source)
{
  std::string full_code = SHADER_HEADER + source;

  // We can't use "main" as an entry point in Metal.
  const char* entry_point;
  const char* shader_type;
  switch (stage)
  {
  case ShaderStage::Vertex:
    entry_point = "vmain";
    shader_type = "vertex";
    break;
  case ShaderStage::Pixel:
    entry_point = "pmain";
    shader_type = "pixel";
    break;
  case ShaderStage::Compute:
    entry_point = "cmain";
    shader_type = "compute";
    break;
  case ShaderStage::Geometry:
  default:
    ERROR_LOG(VIDEO, "Geometry shaders are not supported in Metal.");
    return nullptr;
  }

  ns::Error error;
  mtlpp::CompileOptions options;
  mtlpp::Library library =
      g_metal_context->GetDevice().NewLibrary(full_code.c_str(), options, &error);
  if (!library)
  {
    LogCompileError(shader_type, "library", full_code, error);
    return nullptr;
  }

  mtlpp::FunctionConstantValues constants;
  mtlpp::Function func = library.NewFunction(entry_point, constants, &error);
  if (!func)
  {
    LogCompileError(shader_type, "function", full_code, error);
    return nullptr;
  }

  return std::make_unique<MetalShader>(stage, library, func);
}

std::unique_ptr<MetalShader> MetalShader::CreateFromBinary(ShaderStage stage, const void* data,
                                                           size_t length)
{
  return nullptr;
}

}  // namespace Metal
