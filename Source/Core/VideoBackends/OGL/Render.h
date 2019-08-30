// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <string>

#include "Common/GL/GLContext.h"
#include "Common/GL/GLExtensions/GLExtensions.h"
#include "VideoCommon/RenderBase.h"

namespace OGL
{
class OGLFramebuffer;
class OGLPipeline;
class OGLTexture;

enum GlslVersion
{
  Glsl130,
  Glsl140,
  Glsl150,
  Glsl330,
  Glsl400,  // and above
  Glsl430,
  GlslEs300,  // GLES 3.0
  GlslEs310,  // GLES 3.1
  GlslEs320,  // GLES 3.2
};
enum class EsTexbufType
{
  TexbufNone,
  TexbufCore,
  TexbufOes,
  TexbufExt
};

enum class EsFbFetchType
{
  FbFetchNone,
  FbFetchExt,
  FbFetchArm,
};

// ogl-only config, so not in VideoConfig.h
struct VideoConfig
{
  bool bIsES;
  bool bSupportsGLPinnedMemory;
  bool bSupportsGLSync;
  bool bSupportsGLBaseVertex;
  bool bSupportsGLBufferStorage;
  bool bSupportsMSAA;
  GlslVersion eSupportedGLSLVersion;
  bool bSupportViewportFloat;
  bool bSupportsAEP;
  bool bSupportsDebug;
  bool bSupportsCopySubImage;
  u8 SupportedESPointSize;
  EsTexbufType SupportedESTextureBuffer;
  bool bSupportsTextureStorage;
  bool bSupports2DTextureStorageMultisample;
  bool bSupports3DTextureStorageMultisample;
  bool bSupportsConservativeDepth;
  bool bSupportsImageLoadStore;
  bool bSupportsAniso;
  bool bSupportsBitfield;
  bool bSupportsTextureSubImage;
  EsFbFetchType SupportedFramebufferFetch;
  bool bSupportsShaderThreadShuffleNV;

  const char* gl_vendor;
  const char* gl_renderer;
  const char* gl_version;

  s32 max_samples;
};
extern VideoConfig g_ogl_config;

class Renderer : public ::Renderer
{
public:
  Renderer(std::unique_ptr<GLContext> main_gl_context, float backbuffer_scale);
  ~Renderer() override;

  static Renderer* GetInstance() { return static_cast<Renderer*>(g_renderer.get()); }

  bool IsHeadless() const override;

  bool Initialize() override;
  void Shutdown() override;

  std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config) override;
  std::unique_ptr<AbstractStagingTexture>
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config) override;
  std::unique_ptr<AbstractShader> CreateShaderFromSource(ShaderStage stage,
                                                         std::string_view source) override;
  std::unique_ptr<AbstractShader> CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                         size_t length) override;
  std::unique_ptr<NativeVertexFormat>
  CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl) override;
  std::unique_ptr<AbstractPipeline> CreatePipeline(const AbstractPipelineConfig& config,
                                                   const void* cache_data = nullptr,
                                                   size_t cache_data_length = 0) override;
  std::unique_ptr<AbstractFramebuffer>
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment) override;

  void SetPipeline(const AbstractPipeline* pipeline) override;
  void SetFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value = {},
                              float depth_value = 0.0f) override;
  void SetScissorRect(const MathUtil::Rectangle<int>& rc) override;
  void SetTexture(u32 index, const AbstractTexture* texture) override;
  void SetSamplerState(u32 index, const SamplerState& state) override;
  void SetComputeImageTexture(AbstractTexture* texture, bool read, bool write) override;
  void UnbindTexture(const AbstractTexture* texture) override;
  void SetViewport(float x, float y, float width, float height, float near_depth,
                   float far_depth) override;
  void Draw(u32 base_vertex, u32 num_vertices) override;
  void DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex) override;
  void DispatchComputeShader(const AbstractShader* shader, u32 groups_x, u32 groups_y,
                             u32 groups_z) override;
  void BindBackbuffer(const ClearColor& clear_color = {}) override;
  void PresentBackbuffer() override;

  u16 BBoxRead(int index) override;
  void BBoxWrite(int index, u16 value) override;

  void BeginUtilityDrawing() override;
  void EndUtilityDrawing() override;

  void Flush() override;
  void WaitForGPUIdle() override;
  void RenderXFBToScreen(const MathUtil::Rectangle<int>& target_rc,
                         const AbstractTexture* source_texture,
                         const MathUtil::Rectangle<int>& source_rc) override;
  void OnConfigChanged(u32 bits) override;

  void ClearScreen(const MathUtil::Rectangle<int>& rc, bool colorEnable, bool alphaEnable,
                   bool zEnable, u32 color, u32 z) override;

  std::unique_ptr<VideoCommon::AsyncShaderCompiler> CreateAsyncShaderCompiler() override;

  // Only call methods from this on the GPU thread.
  GLContext* GetMainGLContext() const { return m_main_gl_context.get(); }
  bool IsGLES() const { return m_main_gl_context->IsGLES(); }

  // Invalidates a cached texture binding. Required for texel buffers when they borrow the units.
  void InvalidateTextureBinding(u32 index)
  {
    m_current_state.textures[index] = nullptr;
    m_pending_state.textures[index] = nullptr;
    m_dirty_state &= ~(GLState::DirtyTexture0 << index);
  }

  // Overwrites the current program, use when compiling shaders.
  void OverrideCurrentProgram(GLuint id)
  {
    m_current_state.program = id;
    m_dirty_state |= GLState::DirtyProgram;
  }

  // The shared framebuffer exists for copying textures when extensions are not available. It is
  // slower, but the only way to do these things otherwise.
  GLuint GetSharedReadFramebuffer() const { return m_shared_read_framebuffer; }
  GLuint GetSharedDrawFramebuffer() const { return m_shared_draw_framebuffer; }
  void BindSharedReadFramebuffer();
  void BindSharedDrawFramebuffer();

  // Restores FBO binding after it's been changed.
  void RestoreFramebufferBinding();

private:
  static constexpr u32 NUM_TEXTURE_UNITS = 8;

  void CheckForSurfaceChange();
  void CheckForSurfaceResize();

  void SetRasterizationState(const RasterizationState state);
  void SetDepthState(const DepthState state);
  void SetBlendingState(const BlendingState state);
  void ApplyState();

  std::unique_ptr<GLContext> m_main_gl_context;
  std::unique_ptr<OGLFramebuffer> m_system_framebuffer;
  AbstractTexture* m_bound_image_texture = nullptr;
  GLuint m_shared_read_framebuffer = 0;
  GLuint m_shared_draw_framebuffer = 0;

  // TODO: VAO, framebuffer, move state to pipeline object
  struct GLState
  {
    enum DirtyBit : u64
    {
      DirtyCullFaceEnabled = UINT64_C(1) << 0,
      DirtyDepthTestEnabled = UINT64_C(1) << 1,
      DirtyDepthFunc = UINT64_C(1) << 2,
      DirtyDepthMask = UINT64_C(1) << 3,
      DirtyColorAlphaMask = UINT64_C(1) << 4,
      DirtyClipDistanceEnabled = UINT64_C(1) << 5,
      DirtyCullFace = UINT64_C(1) << 6,
      DirtyBlendEnabled = UINT64_C(1) << 7,
      DirtyBlendFunc = UINT64_C(1) << 8,
      DirtyBlendFactor = UINT64_C(1) << 9,
      DirtyLogicOpEnabled = UINT64_C(1) << 10,
      DirtyLogicOp = UINT64_C(1) << 11,
      DirtyFramebuffer = UINT64_C(1) << 12,
      DirtyViewport = UINT64_C(1) << 13,
      DirtyDepthRange = UINT64_C(1) << 14,
      DirtyScissor = UINT64_C(1) << 15,
      DirtyTexture0 = UINT64_C(1) << 16,
      DirtySampler0 = UINT64_C(1) << 24,
      DirtyProgram = UINT64_C(1) << 32
    };

    bool cull_face_enabled = false;
    bool depth_test_enabled = false;
    bool depth_mask = false;
    bool blend_enabled = false;
    bool logic_op_enabled = false;
    bool color_mask = true;
    bool alpha_mask = true;
    bool clip_distance_enabled = false;

    GLenum cull_face = GL_BACK;
    GLenum depth_func = GL_LESS;
    GLenum blend_func = GL_FUNC_ADD;
    GLenum blend_func_alpha = GL_FUNC_ADD;
    GLenum blend_src_factor_rgb = GL_ONE;
    GLenum blend_dst_factor_rgb = GL_ONE;
    GLenum blend_src_factor_alpha = GL_ONE;
    GLenum blend_dst_factor_alpha = GL_ONE;
    GLenum logic_op = GL_COPY;

    GLuint framebuffer = 0;
    GLuint program = 0;

    struct Viewport
    {
      float x;
      float y;
      float width;
      float height;

      bool operator==(const Viewport& rhs) const
      {
        return std::memcmp(this, &rhs, sizeof(*this)) == 0;
      }
      bool operator!=(const Viewport& rhs) const
      {
        return std::memcmp(this, &rhs, sizeof(*this)) != 0;
      }
    } viewport = {0.0f, 0.0f, 1.0f, 1.0f};
    static_assert(std::is_pod_v<Viewport>);

    float near_depth = 0.0f;
    float far_depth = 1.0f;

    struct Scissor
    {
      int x;
      int y;
      int width;
      int height;

      bool operator==(const Scissor& rhs) const
      {
        return std::memcmp(this, &rhs, sizeof(*this)) == 0;
      }
      bool operator!=(const Scissor& rhs) const
      {
        return std::memcmp(this, &rhs, sizeof(*this)) != 0;
      }
    } scissor = {0, 0, 1, 1};
    static_assert(std::is_pod_v<Scissor>);

    std::array<const OGLTexture*, NUM_TEXTURE_UNITS> textures{};
    std::array<GLuint, NUM_TEXTURE_UNITS> samplers{};
  };

  // Current state.
  GLState m_current_state;
  GLState m_pending_state;
  u64 m_dirty_state = 0;

  // Debug output enabled?
  bool m_debug_output_enabled = false;

  // Broken dual-source blending, see DriverDetails.cpp.
  bool m_has_broken_dual_source_blending = false;
};
}  // namespace OGL
