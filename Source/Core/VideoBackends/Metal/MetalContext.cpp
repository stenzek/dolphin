// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>

#include "Common/Assert.h"
#include "Common/CommonFuncs.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "VideoBackends/Metal/MetalContext.h"
#include "VideoBackends/Metal/StateTracker.h"
#include "VideoBackends/Metal/StreamBuffer.h"
#include "VideoCommon/DriverDetails.h"

namespace Metal
{
std::unique_ptr<MetalContext> g_metal_context;

MetalContext::MetalContext(mtlpp::Device device) : m_device(device)
{
  if (!CreateSamplerStates())
    PanicAlert("Failed to create default sampler states.");
}

MetalContext::~MetalContext()
{
}

void MetalContext::PopulateBackendInfo(VideoConfig* config)
{
  config->backend_info.api_type = APIType::Metal;
  config->backend_info.bSupportsExclusiveFullscreen = false;  // Currently WSI does not allow this.
  config->backend_info.bSupports3DVision = false;             // D3D-exclusive.
  config->backend_info.bSupportsOversizedViewports = true;    // Assumed support.
  config->backend_info.bSupportsEarlyZ = true;                // Assumed support.
  config->backend_info.bSupportsPrimitiveRestart = true;      // Assumed support.
  config->backend_info.bSupportsBindingLayout = false;        // Assumed support.
  config->backend_info.bSupportsPaletteConversion = true;     // Assumed support.
  config->backend_info.bSupportsClipControl = true;           // Assumed support.
  config->backend_info.bSupportsMultithreading = false;       // Assumed support.
  config->backend_info.bSupportsComputeShaders = true;        // Assumed support.
  config->backend_info.bSupportsGPUTextureDecoding = true;    // Assumed support.
  config->backend_info.bSupportsBitfield = true;              // Assumed support.
  config->backend_info.bSupportsDynamicSamplerIndexing = false;     // Assumed support.
  config->backend_info.bSupportsDualSourceBlend = false;            // Dependent on features.
  config->backend_info.bSupportsGeometryShaders = false;           // Dependent on features.
  config->backend_info.bSupportsGSInstancing = false;              // Dependent on features.
  config->backend_info.bSupportsBBox = false;                      // Dependent on features.
  config->backend_info.bSupportsFragmentStoresAndAtomics = false;  // Dependent on features.
  config->backend_info.bSupportsSSAA = false;                      // Dependent on features.
  config->backend_info.bSupportsDepthClamp = false;                // Dependent on features.
  config->backend_info.bSupportsST3CTextures = false;              // Dependent on features.
  config->backend_info.bSupportsBPTCTextures = false;              // Dependent on features.
  config->backend_info.bSupportsReversedDepthRange = false;        // No support.
  config->backend_info.bSupportsPostProcessing = false;            // No support yet.
  config->backend_info.bSupportsCopyToVram = true;                 // Assumed support.
  config->backend_info.bSupportsFramebufferFetch = false;
  config->backend_info.bSupportsBackgroundCompiling = true;
  config->backend_info.AAModes.clear();
}

void MetalContext::PopulateBackendInfoAdapters(VideoConfig* config,
                                               const ns::Array<mtlpp::Device>& gpu_list)
{
  config->backend_info.Adapters.clear();
  for (u32 i = 0; i < gpu_list.GetSize(); i++)
    config->backend_info.Adapters.push_back(gpu_list[i].GetName().GetCStr());
}

bool MetalContext::CreateStreamingBuffers()
{
  m_uniform_stream_buffer =
      StreamBuffer::Create(INITIAL_UNIFORM_BUFFER_SIZE, MAX_UNIFORM_BUFFER_SIZE);
  m_texture_stream_buffer =
      StreamBuffer::Create(INITIAL_TEXTURE_BUFFER_SIZE, MAX_TEXTURE_BUFFER_SIZE);
  if (!m_uniform_stream_buffer || !m_texture_stream_buffer)
  {
    PanicAlert("Failed to allocate streaming buffers.");
    return false;
  }

  return true;
}

static mtlpp::SamplerAddressMode MapSamplerAddressMode(SamplerState::AddressMode mode)
{
  switch (mode)
  {
  case SamplerState::AddressMode::Clamp:
    return mtlpp::SamplerAddressMode::ClampToEdge;
  case SamplerState::AddressMode::MirroredRepeat:
    return mtlpp::SamplerAddressMode::MirrorRepeat;
  case SamplerState::AddressMode::Repeat:
    return mtlpp::SamplerAddressMode::Repeat;
  default:
    return mtlpp::SamplerAddressMode::ClampToEdge;
  }
}

static mtlpp::SamplerMinMagFilter MapSamplerFilter(SamplerState::Filter filter)
{
  switch (filter)
  {
  case SamplerState::Filter::Point:
    return mtlpp::SamplerMinMagFilter::Nearest;
  case SamplerState::Filter::Linear:
  default:
    return mtlpp::SamplerMinMagFilter::Linear;
  }
}

static mtlpp::SamplerMipFilter MapSamplerMipmapFilter(const SamplerState& ss)
{
  if (ss.min_lod == ss.max_lod)
    return mtlpp::SamplerMipFilter::NotMipmapped;

  switch (ss.mipmap_filter)
  {
  case SamplerState::Filter::Point:
    return mtlpp::SamplerMipFilter::Nearest;
  case SamplerState::Filter::Linear:
  default:
    return mtlpp::SamplerMipFilter::Linear;
  }
}

mtlpp::SamplerState& MetalContext::GetSamplerState(const SamplerState& config)
{
  auto iter = m_sampler_states.find(config);
  if (iter != m_sampler_states.end())
    return iter->second;

  // TODO: lod bias is missing.
  mtlpp::SamplerDescriptor desc;
  desc.SetSAddressMode(MapSamplerAddressMode(config.wrap_u));
  desc.SetTAddressMode(MapSamplerAddressMode(config.wrap_v));
  desc.SetMinFilter(MapSamplerFilter(config.min_filter));
  desc.SetMagFilter(MapSamplerFilter(config.mag_filter));
  desc.SetMipFilter(MapSamplerMipmapFilter(config));
  desc.SetLodMinClamp(config.min_lod / 16.0f);
  desc.SetLodMaxClamp(config.max_lod / 16.0f);
  desc.SetMaxAnisotropy(config.anisotropic_filtering ? g_ActiveConfig.iMaxAnisotropy : 1);
  desc.SetNormalizedCoordinates(true);

  mtlpp::SamplerState ss = m_device.NewSamplerState(desc);
  if (!ss)
    ERROR_LOG(VIDEO, "Failed to create sampler state.");

  auto ip = m_sampler_states.emplace(config, ss);
  return ip.first->second;
}

void MetalContext::ResetSamplerStates()
{
  m_sampler_states.clear();
  if (!CreateSamplerStates())
    PanicAlert("Failed to re-create sampler states.");
}

bool MetalContext::CreateSamplerStates()
{
  SamplerState ss = RenderState::GetLinearSamplerState();
  m_linear_sampler = GetSamplerState(ss);

  ss.min_filter = SamplerState::Filter::Point;
  ss.mag_filter = SamplerState::Filter::Point;
  ss.mipmap_filter = SamplerState::Filter::Point;
  m_point_sampler = GetSamplerState(ss);

  return m_point_sampler && m_linear_sampler;
}

}  // namespace Metal
