// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoBackends/D3D/VertexManager.h"

#include <d3d11.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonTypes.h"

#include "VideoBackends/D3D/BoundingBox.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/Render.h"

#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"

namespace DX11
{
static ID3D11Buffer* AllocateConstantBuffer(u32 size)
{
  const u32 cbsize = Common::AlignUp(size, 16u);  // must be a multiple of 16
  const CD3D11_BUFFER_DESC cbdesc(cbsize, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC,
                                  D3D11_CPU_ACCESS_WRITE);
  ID3D11Buffer* cbuf;
  const HRESULT hr = D3D::device->CreateBuffer(&cbdesc, nullptr, &cbuf);
  CHECK(hr == S_OK, "shader constant buffer (size=%u)", cbsize);
  D3D::SetDebugObjectName(cbuf, "constant buffer used to emulate the GX pipeline");
  return cbuf;
}

static void UpdateConstantBuffer(ID3D11Buffer* const buffer, const void* data, u32 data_size)
{
  D3D11_MAPPED_SUBRESOURCE map;
  D3D::context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
  memcpy(map.pData, data, data_size);
  D3D::context->Unmap(buffer, 0);

  ADDSTAT(stats.thisFrame.bytesUniformStreamed, data_size);
}

static ID3D11ShaderResourceView*
CreateTexelBufferView(ID3D11Buffer* buffer, TexelBufferFormat format, DXGI_FORMAT srv_format)
{
  ID3D11ShaderResourceView* srv;
  CD3D11_SHADER_RESOURCE_VIEW_DESC srv_desc(buffer, srv_format, 0,
                                            VertexManager::TEXEL_STREAM_BUFFER_SIZE /
                                                VertexManager::GetTexelBufferElementSize(format));
  CHECK(SUCCEEDED(D3D::device->CreateShaderResourceView(buffer, &srv_desc, &srv)),
        "Create SRV for texel buffer");
  return srv;
}

VertexManager::VertexManager() = default;

VertexManager::~VertexManager()
{
  for (auto& srv_ptr : m_texel_buffer_views)
    SAFE_RELEASE(srv_ptr);
  SAFE_RELEASE(m_texel_buffer);
  SAFE_RELEASE(m_vertex_buffer);
  SAFE_RELEASE(m_index_buffer);
  SAFE_RELEASE(m_pixel_constant_buffer);
  SAFE_RELEASE(m_geometry_constant_buffer);
  SAFE_RELEASE(m_vertex_constant_buffer);
}

bool VertexManager::Initialize()
{
  if (!VertexManagerBase::Initialize())
    return false;

  m_vertex_constant_buffer = AllocateConstantBuffer(sizeof(VertexShaderConstants));
  m_geometry_constant_buffer = AllocateConstantBuffer(sizeof(GeometryShaderConstants));
  m_pixel_constant_buffer = AllocateConstantBuffer(sizeof(PixelShaderConstants));
  if (!m_vertex_constant_buffer || !m_geometry_constant_buffer || !m_pixel_constant_buffer)
    return false;

  const CD3D11_BUFFER_DESC vertex_buf_desc(VERTEX_STREAM_BUFFER_SIZE, D3D11_BIND_VERTEX_BUFFER,
                                           D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  const CD3D11_BUFFER_DESC index_buf_desc(INDEX_STREAM_BUFFER_SIZE, D3D11_BIND_INDEX_BUFFER,
                                          D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  const CD3D11_BUFFER_DESC texel_buf_desc(TEXEL_STREAM_BUFFER_SIZE, D3D11_BIND_SHADER_RESOURCE,
                                          D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  CHECK(SUCCEEDED(D3D::device->CreateBuffer(&vertex_buf_desc, nullptr, &m_vertex_buffer)),
        "create vertex buffer");
  CHECK(SUCCEEDED(D3D::device->CreateBuffer(&index_buf_desc, nullptr, &m_index_buffer)),
        "create index buffer");
  CHECK(SUCCEEDED(D3D::device->CreateBuffer(&texel_buf_desc, nullptr, &m_texel_buffer)),
        "create texel buffer");
  if (!m_vertex_buffer || !m_index_buffer || !m_texel_buffer)
    return false;

  static constexpr std::array<std::pair<TexelBufferFormat, DXGI_FORMAT>, NUM_TEXEL_BUFFER_FORMATS>
      format_mapping = {{
          {TEXEL_BUFFER_FORMAT_R8_UINT, DXGI_FORMAT_R8_UINT},
          {TEXEL_BUFFER_FORMAT_R16_UINT, DXGI_FORMAT_R16_UINT},
          {TEXEL_BUFFER_FORMAT_RGBA8_UINT, DXGI_FORMAT_R8G8B8A8_UNORM},
          {TEXEL_BUFFER_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_UINT},
      }};
  for (const auto& it : format_mapping)
  {
    m_texel_buffer_views[it.first] = CreateTexelBufferView(m_texel_buffer, it.first, it.second);
    if (!m_texel_buffer_views[it.first])
      return false;
  }

  return true;
}

void VertexManager::UploadUtilityUniforms(const void* uniforms, u32 uniforms_size)
{
  // Just use the one buffer for all three.
  InvalidateConstants();
  UpdateConstantBuffer(m_vertex_constant_buffer, uniforms, uniforms_size);
  D3D::stateman->SetVertexConstants(m_vertex_constant_buffer);
  D3D::stateman->SetGeometryConstants(m_vertex_constant_buffer);
  D3D::stateman->SetPixelConstants(m_vertex_constant_buffer);
}

bool VertexManager::MapBuffer(ID3D11Buffer* buffer, u32 buffer_size, u32& cursor, u32 required_size,
                              D3D11_MAPPED_SUBRESOURCE& sr)
{
  if ((cursor + required_size) > buffer_size)
  {
    // Restart buffer.
    HRESULT hr = D3D::context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &sr);
    CHECK(SUCCEEDED(hr), "Map buffer");
    if (FAILED(hr))
      return false;

    cursor = 0;
  }
  else
  {
    // Don't overwrite the earlier-used space.
    HRESULT hr = D3D::context->Map(buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &sr);
    CHECK(SUCCEEDED(hr), "Map buffer");
    if (FAILED(hr))
      return false;
  }

  return true;
}

bool VertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                      u32* out_offset)
{
  if (data_size > TEXEL_STREAM_BUFFER_SIZE)
    return false;

  const u32 elem_size = GetTexelBufferElementSize(format);
  m_texel_buffer_offset = Common::AlignUp(m_texel_buffer_offset, elem_size);

  D3D11_MAPPED_SUBRESOURCE sr;
  if (!MapBuffer(m_texel_buffer, TEXEL_STREAM_BUFFER_SIZE, m_texel_buffer_offset, data_size, sr))
    return false;

  *out_offset = m_texel_buffer_offset / elem_size;
  std::memcpy(static_cast<u8*>(sr.pData) + m_texel_buffer_offset, data, data_size);
  ADDSTAT(stats.thisFrame.bytesUniformStreamed, data_size);
  m_texel_buffer_offset += data_size;

  D3D::context->Unmap(m_texel_buffer, 0);
  D3D::stateman->SetTexture(0, m_texel_buffer_views[static_cast<size_t>(format)]);
  return true;
}

bool VertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                      u32* out_offset, const void* palette_data, u32 palette_size,
                                      TexelBufferFormat palette_format, u32* out_palette_offset)
{
  const u32 elem_size = GetTexelBufferElementSize(format);
  const u32 palette_elem_size = GetTexelBufferElementSize(palette_format);
  const u32 reserve_size = data_size + palette_size + palette_elem_size;
  if (reserve_size > TEXEL_STREAM_BUFFER_SIZE)
    return false;

  m_texel_buffer_offset = Common::AlignUp(m_texel_buffer_offset, elem_size);

  D3D11_MAPPED_SUBRESOURCE sr;
  if (!MapBuffer(m_texel_buffer, TEXEL_STREAM_BUFFER_SIZE, m_texel_buffer_offset, reserve_size, sr))
    return false;

  const u32 palette_byte_offset = Common::AlignUp(data_size, palette_elem_size);
  std::memcpy(static_cast<u8*>(sr.pData) + m_texel_buffer_offset, data, data_size);
  std::memcpy(static_cast<u8*>(sr.pData) + m_texel_buffer_offset + palette_byte_offset,
              palette_data, palette_size);
  ADDSTAT(stats.thisFrame.bytesUniformStreamed, palette_byte_offset + palette_size);
  *out_offset = m_texel_buffer_offset / elem_size;
  *out_palette_offset = (m_texel_buffer_offset + palette_byte_offset) / palette_elem_size;
  m_texel_buffer_offset += palette_byte_offset + palette_size;

  D3D::context->Unmap(m_texel_buffer, 0);
  D3D::stateman->SetTexture(0, m_texel_buffer_views[static_cast<size_t>(format)]);
  D3D::stateman->SetTexture(1, m_texel_buffer_views[static_cast<size_t>(palette_format)]);
  return true;
}

void VertexManager::ResetBuffer(u32 vertex_data_size, u32 vertex_stride, u32 index_data_size)
{
  if (vertex_data_size > 0)
  {
    const u32 vertex_padding = vertex_stride > 0 ? (m_vertex_buffer_cursor % vertex_stride) : 0;
    if (vertex_padding > 0)
      m_vertex_buffer_cursor += vertex_stride - vertex_padding;

    D3D11_MAPPED_SUBRESOURCE sr;
    if (!MapBuffer(m_vertex_buffer, VERTEX_STREAM_BUFFER_SIZE, m_vertex_buffer_cursor,
                   vertex_data_size, sr))
      return;

    m_base_buffer_pointer = static_cast<u8*>(sr.pData);
    m_cur_buffer_pointer = m_base_buffer_pointer + m_vertex_buffer_cursor;
    m_end_buffer_pointer = m_base_buffer_pointer + VERTEX_STREAM_BUFFER_SIZE;
  }

  if (index_data_size > 0)
  {
    D3D11_MAPPED_SUBRESOURCE sr;
    if (!MapBuffer(m_index_buffer, INDEX_STREAM_BUFFER_SIZE, m_index_buffer_cursor, index_data_size,
                   sr))
      return;

    IndexGenerator::Start(
        reinterpret_cast<u16*>(static_cast<u8*>(sr.pData) + m_index_buffer_cursor),
        reinterpret_cast<u16*>(static_cast<u8*>(sr.pData) + INDEX_STREAM_BUFFER_SIZE));
  }
}

void VertexManager::CommitBuffer(u32 vertex_data_size, u32 vertex_stride, u32 index_data_size,
                                 u32* out_base_vertex, u32* out_base_index)
{
  if (vertex_data_size > 0)
  {
    D3D::context->Unmap(m_vertex_buffer, 0);
    *out_base_vertex = m_vertex_buffer_cursor / vertex_stride;
    m_vertex_buffer_cursor += vertex_data_size;
  }
  else
  {
    *out_base_vertex = 0;
  }

  if (index_data_size > 0)
  {
    D3D::context->Unmap(m_index_buffer, 0);
    *out_base_index = m_index_buffer_cursor / sizeof(u16);
    m_index_buffer_cursor += index_data_size;
  }
  else
  {
    *out_base_index = 0;
  }

  ADDSTAT(stats.thisFrame.bytesVertexStreamed, static_cast<int>(vertex_data_size));
  ADDSTAT(stats.thisFrame.bytesIndexStreamed, static_cast<int>(index_data_size));

  D3D::stateman->SetVertexBuffer(m_vertex_buffer, vertex_stride, 0);
  D3D::stateman->SetIndexBuffer(m_index_buffer);
}

void VertexManager::UploadUniforms()
{
  if (VertexShaderManager::dirty)
  {
    UpdateConstantBuffer(m_vertex_constant_buffer, &VertexShaderManager::constants,
                         sizeof(VertexShaderConstants));
    VertexShaderManager::dirty = false;
  }
  if (GeometryShaderManager::dirty)
  {
    UpdateConstantBuffer(m_geometry_constant_buffer, &GeometryShaderManager::constants,
                         sizeof(GeometryShaderConstants));
    GeometryShaderManager::dirty = false;
  }
  if (PixelShaderManager::dirty)
  {
    UpdateConstantBuffer(m_pixel_constant_buffer, &PixelShaderManager::constants,
                         sizeof(PixelShaderConstants));
    PixelShaderManager::dirty = false;
  }

  D3D::stateman->SetPixelConstants(m_pixel_constant_buffer, g_ActiveConfig.bEnablePixelLighting ?
                                                                m_vertex_constant_buffer :
                                                                nullptr);
  D3D::stateman->SetVertexConstants(m_vertex_constant_buffer);
  D3D::stateman->SetGeometryConstants(m_geometry_constant_buffer);
}
}  // namespace DX11
