// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/ShaderCache.h"

#include "Common/Assert.h"
#include "Common/MsgHandler.h"
#include "Core/Host.h"

#include "VideoCommon/RenderBase.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"

std::unique_ptr<VideoCommon::ShaderCache> g_shader_cache;

namespace VideoCommon
{
ShaderCache::ShaderCache() = default;
ShaderCache::~ShaderCache() = default;

bool ShaderCache::Initialize()
{
  m_api_type = g_ActiveConfig.backend_info.api_type;
  m_host_config = ShaderHostConfig::GetCurrent();
  m_efb_multisamples = g_ActiveConfig.iMultisamples;

  // Create the async compiler, and start the worker threads.
  m_async_shader_compiler = std::make_unique<VideoCommon::AsyncShaderCompiler>();
  m_async_shader_compiler->ResizeWorkerThreads(g_ActiveConfig.GetShaderPrecompilerThreads());

  // Load shader and UID caches.
  if (g_ActiveConfig.bShaderCache)
  {
    LoadShaderCaches();
    LoadPipelineUIDCache();
  }

  // Queue ubershader precompiling if required.
  if (g_ActiveConfig.CanPrecompileUberShaders())
    PrecompileUberShaders();

  // Compile all known UIDs.
  CompileMissingPipelines();

  // Switch to the runtime shader compiler thread configuration.
  m_async_shader_compiler->ResizeWorkerThreads(g_ActiveConfig.GetShaderCompilerThreads());
  return true;
}

void ShaderCache::SetHostConfig(const ShaderHostConfig& host_config, u32 efb_multisamples)
{
  if (m_host_config.bits == host_config.bits && m_efb_multisamples == efb_multisamples)
    return;

  m_host_config = host_config;
  m_efb_multisamples = efb_multisamples;
  Reload();
}

void ShaderCache::Reload()
{
  m_async_shader_compiler->WaitUntilCompletion();
  m_async_shader_compiler->RetrieveWorkItems();

  InvalidateCachedPipelines();
  ClearShaderCaches();

  if (g_ActiveConfig.bShaderCache)
    LoadShaderCaches();

  // Switch to the precompiling shader configuration while we rebuild.
  m_async_shader_compiler->ResizeWorkerThreads(g_ActiveConfig.GetShaderPrecompilerThreads());
  CompileMissingPipelines();
  m_async_shader_compiler->ResizeWorkerThreads(g_ActiveConfig.GetShaderCompilerThreads());
}

void ShaderCache::RetrieveAsyncShaders()
{
  m_async_shader_compiler->RetrieveWorkItems();
}

void ShaderCache::Shutdown()
{
  m_async_shader_compiler->StopWorkerThreads();
  m_async_shader_compiler->RetrieveWorkItems();

  ClearShaderCaches();
  ClearPipelineCaches();
}

const AbstractPipeline* ShaderCache::GetPipelineForUid(const GXPipelineConfig& uid)
{
  auto it = m_gx_pipeline_cache.find(uid);
  if (it != m_gx_pipeline_cache.end() && !it->second.second)
    return it->second.first.get();

  std::unique_ptr<AbstractPipeline> pipeline;
  std::optional<AbstractPipelineConfig> pipeline_config = GetGXPipelineConfig(uid);
  if (pipeline_config)
    pipeline = g_renderer->CreatePipeline(*pipeline_config);
  if (g_ActiveConfig.bShaderCache)
    AppendGXPipelineUID(uid);
  return InsertGXPipeline(uid, std::move(pipeline));
}

std::optional<const AbstractPipeline*>
ShaderCache::GetPipelineForUidAsync(const GXPipelineConfig& uid)
{
  auto it = m_gx_pipeline_cache.find(uid);
  if (it != m_gx_pipeline_cache.end())
  {
    if (!it->second.second)
      return it->second.first.get();
    else
      return {};
  }

  auto vs_iter = m_vs_cache.shader_map.find(uid.vs_uid);
  if (vs_iter == m_vs_cache.shader_map.end())
  {
    QueueVertexShaderCompile(uid.vs_uid);
    return {};
  }
  else if (vs_iter->second.pending)
  {
    // VS is still compiling.
    return {};
  }

  auto ps_iter = m_ps_cache.shader_map.find(uid.ps_uid);
  if (ps_iter == m_ps_cache.shader_map.end())
  {
    QueuePixelShaderCompile(uid.ps_uid);
    return {};
  }
  else if (ps_iter->second.pending)
  {
    // PS is still compiling.
    return {};
  }

  if (NeedsGeometryShader(uid.gs_uid))
  {
    auto gs_iter = m_gs_cache.shader_map.find(uid.gs_uid);
    if (gs_iter == m_gs_cache.shader_map.end())
      CreateGeometryShader(uid.gs_uid);
  }

  // All shader stages are present, queue the pipeline compile.
  if (g_ActiveConfig.bShaderCache)
    AppendGXPipelineUID(uid);
  QueuePipelineCompile(uid);
  return {};
}

const AbstractPipeline* ShaderCache::GetUberPipelineForUid(const GXUberPipelineConfig& uid)
{
  auto it = m_gx_uber_pipeline_cache.find(uid);
  if (it != m_gx_uber_pipeline_cache.end() && !it->second.second)
    return it->second.first.get();

  std::unique_ptr<AbstractPipeline> pipeline;
  std::optional<AbstractPipelineConfig> pipeline_config = GetGXUberPipelineConfig(uid);
  if (pipeline_config)
    pipeline = g_renderer->CreatePipeline(*pipeline_config);
  return InsertGXUberPipeline(uid, std::move(pipeline));
}

void ShaderCache::WaitForAsyncCompiler(const std::string& msg)
{
  m_async_shader_compiler->WaitUntilCompletion([&msg](size_t completed, size_t total) {
    Host_UpdateProgressDialog(msg.c_str(), static_cast<int>(completed), static_cast<int>(total));
  });
  m_async_shader_compiler->RetrieveWorkItems();
  Host_UpdateProgressDialog("", -1, -1);
}

template <ShaderStage stage, typename K, typename T>
static void LoadShaderCache(T& cache, APIType api_type, const char* type, bool include_gameid)
{
  class CacheReader : public LinearDiskCacheReader<K, u8>
  {
  public:
    CacheReader(T& cache_) : cache(cache_) {}
    void Read(const K& key, const u8* value, u32 value_size)
    {
      auto shader = g_renderer->CreateShaderFromBinary(stage, value, value_size);
      if (shader)
      {
        auto& entry = cache.shader_map[key];
        entry.shader = std::move(shader);
        entry.pending = false;

        switch (stage)
        {
        case ShaderStage::Vertex:
          INCSTAT(stats.numVertexShadersCreated);
          INCSTAT(stats.numVertexShadersAlive);
          break;
        case ShaderStage::Pixel:
          INCSTAT(stats.numPixelShadersCreated);
          INCSTAT(stats.numPixelShadersAlive);
          break;
        default:
          break;
        }
      }
    }

  private:
    T& cache;
  };

  std::string filename = GetDiskShaderCacheFileName(api_type, type, include_gameid, true);
  CacheReader reader(cache);
  u32 count = cache.disk_cache.OpenAndRead(filename, reader);
  INFO_LOG(VIDEO, "Loaded %u cached shaders from %s", count, filename.c_str());
}

template <typename T>
static void ClearShaderCache(T& cache)
{
  cache.disk_cache.Sync();
  cache.disk_cache.Close();
  cache.shader_map.clear();
}

void ShaderCache::LoadShaderCaches()
{
  // Ubershader caches, if present.
  LoadShaderCache<ShaderStage::Vertex, UberShader::VertexShaderUid>(m_uber_vs_cache, m_api_type,
                                                                    "uber-vs", false);
  LoadShaderCache<ShaderStage::Pixel, UberShader::PixelShaderUid>(m_uber_ps_cache, m_api_type,
                                                                  "uber-ps", false);

  // We also share geometry shaders, as there aren't many variants.
  if (m_host_config.backend_geometry_shaders)
    LoadShaderCache<ShaderStage::Geometry, GeometryShaderUid>(m_gs_cache, m_api_type, "gs", false);

  // Specialized shaders, gameid-specific.
  LoadShaderCache<ShaderStage::Vertex, VertexShaderUid>(m_vs_cache, m_api_type, "specialized-vs",
                                                        true);
  LoadShaderCache<ShaderStage::Pixel, PixelShaderUid>(m_ps_cache, m_api_type, "specialized-ps",
                                                      true);
}

void ShaderCache::ClearShaderCaches()
{
  ClearShaderCache(m_vs_cache);
  ClearShaderCache(m_gs_cache);
  ClearShaderCache(m_ps_cache);

  ClearShaderCache(m_uber_vs_cache);
  ClearShaderCache(m_uber_ps_cache);

  SETSTAT(stats.numPixelShadersCreated, 0);
  SETSTAT(stats.numPixelShadersAlive, 0);
  SETSTAT(stats.numVertexShadersCreated, 0);
  SETSTAT(stats.numVertexShadersAlive, 0);
}

void ShaderCache::LoadPipelineUIDCache()
{
  // We use the async compiler here to speed up startup time.
  class CacheReader : public LinearDiskCacheReader<GXPipelineDiskCacheUid, u8>
  {
  public:
    CacheReader(ShaderCache* shader_cache_) : shader_cache(shader_cache_) {}
    void Read(const GXPipelineDiskCacheUid& key, const u8* data, u32 data_size)
    {
      GXPipelineConfig config = {};
      config.vertex_format = VertexLoaderManager::GetOrCreateMatchingFormat(key.vertex_decl);
      config.vs_uid = key.vs_uid;
      config.gs_uid = key.gs_uid;
      config.ps_uid = key.ps_uid;
      config.rasterization_state.hex = key.rasterization_state_bits;
      config.depth_state.hex = key.depth_state_bits;
      config.blending_state.hex = key.blending_state_bits;

      auto iter = shader_cache->m_gx_pipeline_cache.find(config);
      if (iter != shader_cache->m_gx_pipeline_cache.end())
        return;

      auto& entry = shader_cache->m_gx_pipeline_cache[config];
      entry.second = false;
    }

  private:
    ShaderCache* shader_cache;
  };

  std::string filename = GetDiskShaderCacheFileName(m_api_type, "pipeline-uid", true, false, false);
  CacheReader reader(this);
  u32 count = m_gx_pipeline_uid_disk_cache.OpenAndRead(filename, reader);
  INFO_LOG(VIDEO, "Read %u pipeline UIDs from %s", count, filename.c_str());
  CompileMissingPipelines();
}

void ShaderCache::CompileMissingPipelines()
{
  // Queue all uids with a null pipeline for compilation.
  for (auto& it : m_gx_pipeline_cache)
  {
    if (!it.second.second)
      QueuePipelineCompile(it.first);
  }
  for (auto& it : m_gx_uber_pipeline_cache)
  {
    if (!it.second.second)
      QueueUberPipelineCompile(it.first);
  }

  WaitForAsyncCompiler(GetStringT("Compiling shaders..."));
}

void ShaderCache::InvalidateCachedPipelines()
{
  // Set the pending flag to false, and destroy the pipeline.
  for (auto& it : m_gx_pipeline_cache)
  {
    it.second.first.reset();
    it.second.second = false;
  }
  for (auto& it : m_gx_uber_pipeline_cache)
  {
    it.second.first.reset();
    it.second.second = false;
  }
}

void ShaderCache::ClearPipelineCaches()
{
  m_gx_pipeline_cache.clear();
  m_gx_uber_pipeline_cache.clear();
}

std::unique_ptr<AbstractShader> ShaderCache::CompileVertexShader(const VertexShaderUid& uid) const
{
  ShaderCode source_code = GenerateVertexShaderCode(m_api_type, m_host_config, uid.GetUidData());
  return g_renderer->CreateShaderFromSource(ShaderStage::Vertex, source_code.GetBuffer().c_str(),
                                            source_code.GetBuffer().size());
}

std::unique_ptr<AbstractShader>
ShaderCache::CompileVertexUberShader(const UberShader::VertexShaderUid& uid) const
{
  ShaderCode source_code = UberShader::GenVertexShader(m_api_type, m_host_config, uid.GetUidData());
  return g_renderer->CreateShaderFromSource(ShaderStage::Vertex, source_code.GetBuffer().c_str(),
                                            source_code.GetBuffer().size());
}

std::unique_ptr<AbstractShader> ShaderCache::CompilePixelShader(const PixelShaderUid& uid) const
{
  ShaderCode source_code = GeneratePixelShaderCode(m_api_type, m_host_config, uid.GetUidData());
  return g_renderer->CreateShaderFromSource(ShaderStage::Pixel, source_code.GetBuffer().c_str(),
                                            source_code.GetBuffer().size());
}

std::unique_ptr<AbstractShader>
ShaderCache::CompilePixelUberShader(const UberShader::PixelShaderUid& uid) const
{
  ShaderCode source_code = UberShader::GenPixelShader(m_api_type, m_host_config, uid.GetUidData());
  return g_renderer->CreateShaderFromSource(ShaderStage::Pixel, source_code.GetBuffer().c_str(),
                                            source_code.GetBuffer().size());
}

const AbstractShader* ShaderCache::InsertVertexShader(const VertexShaderUid& uid,
                                                      std::unique_ptr<AbstractShader> shader)
{
  auto& entry = m_vs_cache.shader_map[uid];
  entry.pending = false;

  if (shader && !entry.shader)
  {
    if (g_ActiveConfig.bShaderCache && shader->HasBinary())
    {
      auto binary = shader->GetBinary();
      if (!binary.empty())
        m_vs_cache.disk_cache.Append(uid, binary.data(), static_cast<u32>(binary.size()));
    }
    INCSTAT(stats.numVertexShadersCreated);
    INCSTAT(stats.numVertexShadersAlive);
    entry.shader = std::move(shader);
  }

  return entry.shader.get();
}

const AbstractShader* ShaderCache::InsertVertexUberShader(const UberShader::VertexShaderUid& uid,
                                                          std::unique_ptr<AbstractShader> shader)
{
  auto& entry = m_uber_vs_cache.shader_map[uid];
  entry.pending = false;

  if (shader && !entry.shader)
  {
    if (g_ActiveConfig.bShaderCache && shader->HasBinary())
    {
      auto binary = shader->GetBinary();
      if (!binary.empty())
        m_uber_vs_cache.disk_cache.Append(uid, binary.data(), static_cast<u32>(binary.size()));
    }
    INCSTAT(stats.numVertexShadersCreated);
    INCSTAT(stats.numVertexShadersAlive);
    entry.shader = std::move(shader);
  }

  return entry.shader.get();
}

const AbstractShader* ShaderCache::InsertPixelShader(const PixelShaderUid& uid,
                                                     std::unique_ptr<AbstractShader> shader)
{
  auto& entry = m_ps_cache.shader_map[uid];
  entry.pending = false;

  if (shader && !entry.shader)
  {
    if (g_ActiveConfig.bShaderCache && shader->HasBinary())
    {
      auto binary = shader->GetBinary();
      if (!binary.empty())
        m_ps_cache.disk_cache.Append(uid, binary.data(), static_cast<u32>(binary.size()));
    }
    INCSTAT(stats.numPixelShadersCreated);
    INCSTAT(stats.numPixelShadersAlive);
    entry.shader = std::move(shader);
  }

  return entry.shader.get();
}

const AbstractShader* ShaderCache::InsertPixelUberShader(const UberShader::PixelShaderUid& uid,
                                                         std::unique_ptr<AbstractShader> shader)
{
  auto& entry = m_uber_ps_cache.shader_map[uid];
  entry.pending = false;

  if (shader && !entry.shader)
  {
    if (g_ActiveConfig.bShaderCache && shader->HasBinary())
    {
      auto binary = shader->GetBinary();
      if (!binary.empty())
        m_uber_ps_cache.disk_cache.Append(uid, binary.data(), static_cast<u32>(binary.size()));
    }
    INCSTAT(stats.numPixelShadersCreated);
    INCSTAT(stats.numPixelShadersAlive);
    entry.shader = std::move(shader);
  }

  return entry.shader.get();
}

const AbstractShader* ShaderCache::CreateGeometryShader(const GeometryShaderUid& uid)
{
  ShaderCode source_code = GenerateGeometryShaderCode(m_api_type, m_host_config, uid.GetUidData());
  std::unique_ptr<AbstractShader> shader = g_renderer->CreateShaderFromSource(
      ShaderStage::Geometry, source_code.GetBuffer().c_str(), source_code.GetBuffer().size());

  auto& entry = m_gs_cache.shader_map[uid];
  entry.pending = false;

  if (shader && !entry.shader)
  {
    if (g_ActiveConfig.bShaderCache && shader->HasBinary())
    {
      auto binary = shader->GetBinary();
      if (!binary.empty())
        m_gs_cache.disk_cache.Append(uid, binary.data(), static_cast<u32>(binary.size()));
    }
    entry.shader = std::move(shader);
  }

  return entry.shader.get();
}

bool ShaderCache::NeedsGeometryShader(const GeometryShaderUid& uid) const
{
  return m_host_config.backend_geometry_shaders && !uid.GetUidData()->IsPassthrough();
}

AbstractPipelineConfig ShaderCache::GetGXPipelineConfig(
    const NativeVertexFormat* vertex_format, const AbstractShader* vertex_shader,
    const AbstractShader* geometry_shader, const AbstractShader* pixel_shader,
    const RasterizationState& rasterization_state, const DepthState& depth_state,
    const BlendingState& blending_state)
{
  AbstractPipelineConfig config = {};
  config.usage = AbstractPipelineUsage::GX;
  config.vertex_format = vertex_format;
  config.vertex_shader = vertex_shader;
  config.geometry_shader = geometry_shader;
  config.pixel_shader = pixel_shader;
  config.rasterization_state = rasterization_state;
  config.depth_state = depth_state;
  config.blending_state = blending_state;
  config.framebuffer_state.color_texture_format = AbstractTextureFormat::RGBA8;
  config.framebuffer_state.depth_texture_format = AbstractTextureFormat::D32F;
  config.framebuffer_state.per_sample_shading = m_host_config.ssaa;
  config.framebuffer_state.samples = m_efb_multisamples;
  return config;
}

std::optional<AbstractPipelineConfig>
ShaderCache::GetGXPipelineConfig(const GXPipelineConfig& config)
{
  const AbstractShader* vs;
  auto vs_iter = m_vs_cache.shader_map.find(config.vs_uid);
  if (vs_iter != m_vs_cache.shader_map.end() && !vs_iter->second.pending)
    vs = vs_iter->second.shader.get();
  else
    vs = InsertVertexShader(config.vs_uid, CompileVertexShader(config.vs_uid));

  const AbstractShader* ps;
  auto ps_iter = m_ps_cache.shader_map.find(config.ps_uid);
  if (ps_iter != m_ps_cache.shader_map.end() && !ps_iter->second.pending)
    ps = ps_iter->second.shader.get();
  else
    ps = InsertPixelShader(config.ps_uid, CompilePixelShader(config.ps_uid));

  if (!vs || !ps)
    return {};

  const AbstractShader* gs = nullptr;
  if (NeedsGeometryShader(config.gs_uid))
  {
    auto gs_iter = m_gs_cache.shader_map.find(config.gs_uid);
    if (gs_iter != m_gs_cache.shader_map.end() && !gs_iter->second.pending)
      gs = gs_iter->second.shader.get();
    else
      gs = CreateGeometryShader(config.gs_uid);
    if (!gs)
      return {};
  }

  return GetGXPipelineConfig(config.vertex_format, vs, gs, ps, config.rasterization_state,
                             config.depth_state, config.blending_state);
}

std::optional<AbstractPipelineConfig>
ShaderCache::GetGXUberPipelineConfig(const GXUberPipelineConfig& config)
{
  const AbstractShader* vs;
  auto vs_iter = m_uber_vs_cache.shader_map.find(config.vs_uid);
  if (vs_iter != m_uber_vs_cache.shader_map.end() && !vs_iter->second.pending)
    vs = vs_iter->second.shader.get();
  else
    vs = InsertVertexUberShader(config.vs_uid, CompileVertexUberShader(config.vs_uid));

  const AbstractShader* ps;
  auto ps_iter = m_uber_ps_cache.shader_map.find(config.ps_uid);
  if (ps_iter != m_uber_ps_cache.shader_map.end() && !ps_iter->second.pending)
    ps = ps_iter->second.shader.get();
  else
    ps = InsertPixelUberShader(config.ps_uid, CompilePixelUberShader(config.ps_uid));

  if (!vs || !ps)
    return {};

  const AbstractShader* gs = nullptr;
  if (NeedsGeometryShader(config.gs_uid))
  {
    auto gs_iter = m_gs_cache.shader_map.find(config.gs_uid);
    if (gs_iter != m_gs_cache.shader_map.end() && !gs_iter->second.pending)
      gs = gs_iter->second.shader.get();
    else
      gs = CreateGeometryShader(config.gs_uid);
    if (!gs)
      return {};
  }

  return GetGXPipelineConfig(config.vertex_format, vs, gs, ps, config.rasterization_state,
                             config.depth_state, config.blending_state);
}

const AbstractPipeline* ShaderCache::InsertGXPipeline(const GXPipelineConfig& config,
                                                      std::unique_ptr<AbstractPipeline> pipeline)
{
  auto& entry = m_gx_pipeline_cache[config];
  entry.second = false;
  if (!entry.first && pipeline)
    entry.first = std::move(pipeline);

  return entry.first.get();
}

const AbstractPipeline*
ShaderCache::InsertGXUberPipeline(const GXUberPipelineConfig& config,
                                  std::unique_ptr<AbstractPipeline> pipeline)
{
  auto& entry = m_gx_uber_pipeline_cache[config];
  entry.second = false;
  if (!entry.first && pipeline)
    entry.first = std::move(pipeline);

  return entry.first.get();
}

void ShaderCache::AppendGXPipelineUID(const GXPipelineConfig& config)
{
  // Convert to disk format.
  GXPipelineDiskCacheUid disk_uid = {};
  disk_uid.vertex_decl = config.vertex_format->GetVertexDeclaration();
  disk_uid.vs_uid = config.vs_uid;
  disk_uid.gs_uid = config.gs_uid;
  disk_uid.ps_uid = config.ps_uid;
  disk_uid.rasterization_state_bits = config.rasterization_state.hex;
  disk_uid.depth_state_bits = config.depth_state.hex;
  disk_uid.blending_state_bits = config.blending_state.hex;
  m_gx_pipeline_uid_disk_cache.Append(disk_uid, nullptr, 0);
}

void ShaderCache::QueueVertexShaderCompile(const VertexShaderUid& uid)
{
  class VertexShaderWorkItem final : public AsyncShaderCompiler::WorkItem
  {
  public:
    VertexShaderWorkItem(ShaderCache* shader_cache_, const VertexShaderUid& uid_)
        : shader_cache(shader_cache_), uid(uid_)
    {
    }

    bool Compile() override
    {
      shader = shader_cache->CompileVertexShader(uid);
      return true;
    }

    virtual void Retrieve() override { shader_cache->InsertVertexShader(uid, std::move(shader)); }
  private:
    ShaderCache* shader_cache;
    std::unique_ptr<AbstractShader> shader;
    VertexShaderUid uid;
  };

  m_vs_cache.shader_map[uid].pending = true;
  auto wi = m_async_shader_compiler->CreateWorkItem<VertexShaderWorkItem>(this, uid);
  m_async_shader_compiler->QueueWorkItem(std::move(wi));
}

void ShaderCache::QueueVertexUberShaderCompile(const UberShader::VertexShaderUid& uid)
{
  class VertexUberShaderWorkItem final : public AsyncShaderCompiler::WorkItem
  {
  public:
    VertexUberShaderWorkItem(ShaderCache* shader_cache_, const UberShader::VertexShaderUid& uid_)
        : shader_cache(shader_cache_), uid(uid_)
    {
    }

    bool Compile() override
    {
      shader = shader_cache->CompileVertexUberShader(uid);
      return true;
    }

    virtual void Retrieve() override
    {
      shader_cache->InsertVertexUberShader(uid, std::move(shader));
    }

  private:
    ShaderCache* shader_cache;
    std::unique_ptr<AbstractShader> shader;
    UberShader::VertexShaderUid uid;
  };

  m_uber_vs_cache.shader_map[uid].pending = true;
  auto wi = m_async_shader_compiler->CreateWorkItem<VertexUberShaderWorkItem>(this, uid);
  m_async_shader_compiler->QueueWorkItem(std::move(wi));
}

void ShaderCache::QueuePixelShaderCompile(const PixelShaderUid& uid)
{
  class PixelShaderWorkItem final : public AsyncShaderCompiler::WorkItem
  {
  public:
    PixelShaderWorkItem(ShaderCache* shader_cache_, const PixelShaderUid& uid_)
        : shader_cache(shader_cache_), uid(uid_)
    {
    }

    bool Compile() override
    {
      shader = shader_cache->CompilePixelShader(uid);
      return true;
    }

    virtual void Retrieve() override { shader_cache->InsertPixelShader(uid, std::move(shader)); }
  private:
    ShaderCache* shader_cache;
    std::unique_ptr<AbstractShader> shader;
    PixelShaderUid uid;
  };

  m_ps_cache.shader_map[uid].pending = true;
  auto wi = m_async_shader_compiler->CreateWorkItem<PixelShaderWorkItem>(this, uid);
  m_async_shader_compiler->QueueWorkItem(std::move(wi));
}

void ShaderCache::QueuePixelUberShaderCompile(const UberShader::PixelShaderUid& uid)
{
  class PixelUberShaderWorkItem final : public AsyncShaderCompiler::WorkItem
  {
  public:
    PixelUberShaderWorkItem(ShaderCache* shader_cache_, const UberShader::PixelShaderUid& uid_)
        : shader_cache(shader_cache_), uid(uid_)
    {
    }

    bool Compile() override
    {
      shader = shader_cache->CompilePixelUberShader(uid);
      return true;
    }

    virtual void Retrieve() override
    {
      shader_cache->InsertPixelUberShader(uid, std::move(shader));
    }

  private:
    ShaderCache* shader_cache;
    std::unique_ptr<AbstractShader> shader;
    UberShader::PixelShaderUid uid;
  };

  m_uber_ps_cache.shader_map[uid].pending = true;
  auto wi = m_async_shader_compiler->CreateWorkItem<PixelUberShaderWorkItem>(this, uid);
  m_async_shader_compiler->QueueWorkItem(std::move(wi));
}

void ShaderCache::QueuePipelineCompile(const GXPipelineConfig& uid)
{
  class PipelineWorkItem final : public AsyncShaderCompiler::WorkItem
  {
  public:
    PipelineWorkItem(ShaderCache* shader_cache_, const GXPipelineConfig& uid_,
                     const AbstractPipelineConfig& config_)
        : shader_cache(shader_cache_), uid(uid_), config(config_)
    {
    }

    bool Compile() override
    {
      pipeline = g_renderer->CreatePipeline(config);
      return true;
    }

    virtual void Retrieve() override { shader_cache->InsertGXPipeline(uid, std::move(pipeline)); }
  private:
    ShaderCache* shader_cache;
    std::unique_ptr<AbstractPipeline> pipeline;
    GXPipelineConfig uid;
    AbstractPipelineConfig config;
  };

  auto config = GetGXPipelineConfig(uid);
  if (!config)
  {
    // One or more stages failed to compile.
    InsertGXPipeline(uid, nullptr);
    return;
  }

  auto wi = m_async_shader_compiler->CreateWorkItem<PipelineWorkItem>(this, uid, *config);
  m_async_shader_compiler->QueueWorkItem(std::move(wi));
  m_gx_pipeline_cache[uid].second = true;
}

void ShaderCache::QueueUberPipelineCompile(const GXUberPipelineConfig& uid)
{
  class UberPipelineWorkItem final : public AsyncShaderCompiler::WorkItem
  {
  public:
    UberPipelineWorkItem(ShaderCache* shader_cache_, const GXUberPipelineConfig& uid_,
                         const AbstractPipelineConfig& config_)
        : shader_cache(shader_cache_), uid(uid_), config(config_)
    {
    }

    bool Compile() override
    {
      pipeline = g_renderer->CreatePipeline(config);
      return true;
    }

    virtual void Retrieve() override
    {
      shader_cache->InsertGXUberPipeline(uid, std::move(pipeline));
    }

  private:
    ShaderCache* shader_cache;
    std::unique_ptr<AbstractPipeline> pipeline;
    GXUberPipelineConfig uid;
    AbstractPipelineConfig config;
  };

  auto config = GetGXUberPipelineConfig(uid);
  if (!config)
  {
    // One or more stages failed to compile.
    InsertGXUberPipeline(uid, nullptr);
    return;
  }

  auto wi = m_async_shader_compiler->CreateWorkItem<UberPipelineWorkItem>(this, uid, *config);
  m_async_shader_compiler->QueueWorkItem(std::move(wi));
  m_gx_uber_pipeline_cache[uid].second = true;
}

void ShaderCache::PrecompileUberShaders()
{
  // Geometry shaders are required for the pipelines.
  if (m_host_config.backend_geometry_shaders)
  {
    EnumerateGeometryShaderUids([&](const GeometryShaderUid& guid) {
      auto iter = m_gs_cache.shader_map.find(guid);
      if (iter == m_gs_cache.shader_map.end())
        CreateGeometryShader(guid);
    });
  }

  // Queue shader compiling.
  UberShader::EnumerateVertexShaderUids([&](const UberShader::VertexShaderUid& vuid) {
    auto iter = m_uber_vs_cache.shader_map.find(vuid);
    if (iter == m_uber_vs_cache.shader_map.end())
      QueueVertexUberShaderCompile(vuid);
  });
  UberShader::EnumeratePixelShaderUids([&](const UberShader::PixelShaderUid& puid) {
    auto iter = m_uber_ps_cache.shader_map.find(puid);
    if (iter == m_uber_ps_cache.shader_map.end())
      QueuePixelUberShaderCompile(puid);
  });

  // Wait for shaders to finish compiling.
  WaitForAsyncCompiler(GetStringT("Compiling uber shaders..."));

  // Create a dummy vertex format with no attributes.
  // All attributes will be enabled in GetUberVertexFormat.
  PortableVertexDeclaration dummy_vertex_decl = {};
  NativeVertexFormat* dummy_vertex_format =
      VertexLoaderManager::GetUberVertexFormat(dummy_vertex_decl);
  auto QueueDummyPipeline = [&](const UberShader::VertexShaderUid& vs_uid,
                                const GeometryShaderUid& gs_uid,
                                const UberShader::PixelShaderUid& ps_uid) {
    GXUberPipelineConfig config;
    config.vertex_format = dummy_vertex_format;
    config.vs_uid = vs_uid;
    config.gs_uid = gs_uid;
    config.ps_uid = ps_uid;
    config.rasterization_state = RenderState::GetNoCullRasterizationState();
    config.depth_state = RenderState::GetNoDepthTestingDepthStencilState();
    config.blending_state = RenderState::GetNoBlendingBlendState();

    auto iter = m_gx_uber_pipeline_cache.find(config);
    if (iter != m_gx_uber_pipeline_cache.end())
      return;

    auto& entry = m_gx_uber_pipeline_cache[config];
    entry.second = false;
  };

  // Populate the pipeline configs with empty entries, these will be compiled afterwards.
  UberShader::EnumerateVertexShaderUids([&](const UberShader::VertexShaderUid& vuid) {
    UberShader::EnumeratePixelShaderUids([&](const UberShader::PixelShaderUid& puid) {
      // UIDs must have compatible texgens, a mismatching combination will never be queried.
      if (vuid.GetUidData()->num_texgens != puid.GetUidData()->num_texgens)
        return;

      EnumerateGeometryShaderUids([&](const GeometryShaderUid& guid) {
        if (guid.GetUidData()->numTexGens != vuid.GetUidData()->num_texgens)
          return;

        QueueDummyPipeline(vuid, guid, puid);
      });
    });
  });
}

std::string ShaderCache::GetUtilityShaderHeader() const
{
  std::stringstream ss;

  ss << "#define API_D3D " << (m_api_type == APIType::D3D ? 1 : 0) << "\n";
  ss << "#define API_OPENGL " << (m_api_type == APIType::OpenGL ? 1 : 0) << "\n";
  ss << "#define API_VULKAN " << (m_api_type == APIType::Vulkan ? 1 : 0) << "\n";

  if (m_efb_multisamples > 1)
  {
    ss << "#define MSAA_ENABLED 1" << std::endl;
    ss << "#define MSAA_SAMPLES " << m_efb_multisamples << std::endl;
    if (m_host_config.ssaa)
      ss << "#define SSAA_ENABLED 1" << std::endl;
  }

  ss << "#define EFB_LAYERS " << (m_host_config.stereo ? 2 : 1) << std::endl;

  return ss.str();
}
}  // namespace VideoCommon
