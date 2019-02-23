// Copyright 2014 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Flag.h"
#include "Common/Timer.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoCommon.h"

class AbstractFramebuffer;
class AbstractTexture;
class AbstractPipeline;
class AbstractShader;

namespace VideoCommon::PostProcessing
{
enum class OptionType : u32
{
  Bool,
  Float,
  Float2,
  Float3,
  Float4,
  Int,
  Int2,
  Int3,
  Int4
};

enum class InputType : u32
{
  ExternalImage,       // external image loaded from file
  ColorBuffer,         // colorbuffer at internal resolution
  DepthBuffer,         // depthbuffer at internal resolution
  PreviousPassOutput,  // output of the previous pass
  PassOutput           // output of a previous pass
};

// Maximum number of texture inputs to a post-processing shader.
static const size_t POST_PROCESSING_MAX_TEXTURE_INPUTS = 4;

union OptionValue
{
  bool bool_value;
  float float_values[4];
  s32 int_values[4];
};

struct Option final
{
  OptionType type = OptionType::Float;

  OptionValue default_value = {};
  OptionValue min_value = {};
  OptionValue max_value = {};
  OptionValue step_amount = {};

  OptionValue value = {};

  std::string gui_name;
  std::string option_name;
  std::string dependent_option;

  bool compile_time_constant = false;
  bool is_pass_dependent_option = false;
};

struct ExternalImage final
{
  std::vector<u8> data;
  u32 width;
  u32 height;
};

struct Input final
{
  InputType type = InputType::PreviousPassOutput;
  SamplerState sampler_state = RenderState::GetLinearSamplerState();
  u32 texture_unit = 0;
  u32 pass_output_index = 0;

  std::unique_ptr<ExternalImage> external_image;
};

struct Pass final
{
  std::vector<Input> inputs;
  std::string entry_point;
  std::string dependent_option;
  float output_scale = 1.0f;
};

class Shader
{
public:
  using OptionMap = std::map<std::string, Option>;
  using RenderPassList = std::vector<Pass>;

  Shader();
  ~Shader();

  // Loads the configuration with a shader
  bool LoadShader(const char* name);

  const std::string& GetShaderName() const { return m_name; }
  const std::string& GetShaderSource() const { return m_source; }

  bool RequiresRecompile() { return m_requires_recompile.TestAndClear(); }
  bool RequiresDepthBuffer() const { return m_requires_depth_buffer; }

  bool HasOptions() const { return !m_options.empty(); }
  OptionMap& GetOptions() { return m_options; }
  const OptionMap& GetOptions() const { return m_options; }
  const Option* FindOption(const char* option) const;

  const RenderPassList& GetPasses() const { return m_passes; }
  const Pass& GetPass(u32 index) const { return m_passes.at(index); }
  const u32 GetPassCount() const { return static_cast<u32>(m_passes.size()); }

  // For updating option's values
  void SetOptionf(const char* option, int index, float value);
  void SetOptioni(const char* option, int index, s32 value);
  void SetOptionb(const char* option, bool value);

  // Get a list of available post-processing shaders.
  static std::vector<std::string> GetAvailableShaderNames(const std::string& sub_dir);

private:
  // Intermediate block used while parsing.
  struct ConfigBlock final
  {
    std::string name;
    std::vector<std::pair<std::string, std::string>> values;
  };

  static std::unique_ptr<ExternalImage> LoadExternalImage(const std::string& path);

  bool ParseShader(const std::string& source);

  void CreateDefaultPass();
  bool ParseConfiguration(const std::string& source);
  std::vector<ConfigBlock> ReadConfigSections(const std::string& source);
  bool ParseConfigSections(const std::vector<ConfigBlock>& blocks);
  bool ParseOptionBlock(const ConfigBlock& block);
  bool ParsePassBlock(const ConfigBlock& block);

  std::string m_name;
  std::string m_source;
  std::string m_base_path;
  OptionMap m_options;
  RenderPassList m_passes;
  Common::Flag m_requires_recompile{false};
  bool m_requires_depth_buffer = false;
};

class Config final
{
public:
  Config();
  ~Config();

  bool IsValid() const { return m_valid; }
  bool RequiresDepthBuffer() const { return m_requires_depth_buffer; }
  bool RequiresRecompile() { return m_recompile_flag.TestAndClear(); }

  const Shader& GetShader(u32 index) const { return *m_shaders.at(index).get(); }
  Shader& GetShader(u32 index) { return *m_shaders.at(index).get(); }
  const u32 GetShaderCount() const { return static_cast<u32>(m_shaders.size()); }
  const std::vector<std::unique_ptr<Shader>>& GetShaders() const { return m_shaders; }

  bool LoadConfigString(const std::string& config);

private:
  Common::Flag m_recompile_flag;
  std::vector<std::unique_ptr<Shader>> m_shaders;

  bool m_valid = false;
  bool m_requires_depth_buffer = false;
};

class Instance final
{
public:
  Instance();
  ~Instance();

  static std::vector<std::string> GetShaderList();
  static std::vector<std::string> GetAnaglyphShaderList();

  const Config* GetConfig() { return &m_config; }
  bool RequiresDepthBuffer() const { return m_config.RequiresDepthBuffer(); }
  bool IsValid() const { return m_valid; }

  bool LoadConfig(const std::string& config);

  bool Apply(AbstractFramebuffer* dest_fb, const MathUtil::Rectangle<int>& dest_rect,
             const AbstractTexture* source_color_tex, const AbstractTexture* source_depth_tex,
             const MathUtil::Rectangle<int>& source_rect, int source_layer);

  void RenderConfigUI();

protected:
  struct InputBinding final
  {
    InputType type;
    u32 texture_unit;
    SamplerState sampler_state;
    const AbstractTexture* texture_ptr;
    std::unique_ptr<AbstractTexture> owned_texture_ptr;
    u32 source_pass_index;
  };

  struct RenderPass final
  {
    std::shared_ptr<AbstractShader> vertex_shader;
    std::unique_ptr<AbstractShader> pixel_shader;
    std::unique_ptr<AbstractPipeline> pipeline;
    std::vector<InputBinding> inputs;

    std::unique_ptr<AbstractTexture> output_texture;
    std::unique_ptr<AbstractFramebuffer> output_framebuffer;
    float output_scale;

    u32 shader_index;
    u32 shader_pass_index;
  };

  std::string GetUniformBufferHeader(const Shader& shader) const;
  std::string GetPixelShaderHeader(const Shader& shader, const Pass& pass) const;
  std::string GetPixelShaderFooter(const Shader& shader, const Pass& pass) const;

  std::shared_ptr<AbstractShader> CompileVertexShader(const Shader& shader) const;
  std::unique_ptr<AbstractShader> CompilePixelShader(const Shader& shader, const Pass& pass) const;
  std::unique_ptr<AbstractShader> CompileGeometryShader() const;

  size_t CalculateUniformsSize(const Shader& shader) const;
  void UploadUniformBuffer(const Shader& shader, const AbstractTexture* prev_texture,
                           const MathUtil::Rectangle<int>& prev_rect,
                           const AbstractTexture* source_color_texture,
                           const MathUtil::Rectangle<int>& source_rect, int source_layer);

  bool CompilePasses();
  bool CreateInputBinding(u32 shader_index, const Input& input, InputBinding* binding);
  const RenderPass* GetRenderPass(u32 shader_index, u32 pass_index) const;
  u32 GetRenderPassIndex(u32 shader_index, u32 pass_index) const;

  bool CreateOutputTextures(u32 new_width, u32 new_height, u32 new_layers,
                            AbstractTextureFormat new_format);
  void LinkPassOutputs();
  bool CompilePipelines();

  // Timer for determining our time value
  Common::Timer m_timer;
  Config m_config;

  std::unique_ptr<AbstractShader> m_vertex_shader;
  std::unique_ptr<AbstractShader> m_geometry_shader;

  std::vector<u8> m_uniform_staging_buffer;

  // intermediate buffer sizes
  u32 m_target_width = 0;
  u32 m_target_height = 0;
  u32 m_target_layers = 0;
  AbstractTextureFormat m_target_format = AbstractTextureFormat::Undefined;

  std::vector<RenderPass> m_render_passes;
  bool m_last_pass_uses_color_buffer = false;
  bool m_last_pass_scaled = false;
  bool m_valid = false;
};

}  // namespace VideoCommon::PostProcessing
