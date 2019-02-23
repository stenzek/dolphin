// Copyright 2014 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <sstream>
#include <string>

#include "Common/Assert.h"
#include "Common/CommonFuncs.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/FramebufferShaderGen.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/ShaderCache.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoConfig.h"

#include "imgui.h"

namespace VideoCommon::PostProcessing
{
static std::vector<std::string> GetSearchDirectories()
{
  return {File::GetUserPath(D_SHADERS_IDX), File::GetSysDirectory() + SHADERS_DIR};
}

Shader::Shader() = default;

Shader::~Shader() = default;

std::vector<std::string> Shader::GetAvailableShaderNames(const std::string& sub_dir)
{
  const std::vector<std::string> search_dirs = {File::GetUserPath(D_SHADERS_IDX) + sub_dir,
                                                File::GetSysDirectory() + SHADERS_DIR DIR_SEP +
                                                    sub_dir};
  const std::vector<std::string> search_extensions = {".glsl"};
  std::vector<std::string> result;
  std::vector<std::string> paths;

  // main folder
  paths = Common::DoFileSearch(search_extensions, search_dirs, false);
  for (const std::string& path : paths)
  {
    std::string filename;
    if (SplitPath(path, nullptr, &filename, nullptr))
    {
      if (std::find(result.begin(), result.end(), filename) == result.end())
        result.push_back(filename);
    }
  }

  // folders/sub-shaders
  // paths = Common::DoFileSearch(search_dirs, false);
  for (const std::string& dirname : paths)
  {
    // find sub-shaders in this folder
    size_t pos = dirname.find_last_of(DIR_SEP_CHR);
    if (pos != std::string::npos && (pos != dirname.length() - 1))
    {
      std::string shader_dirname = dirname.substr(pos + 1);
      std::vector<std::string> sub_paths =
          Common::DoFileSearch(search_extensions, {dirname}, false);
      for (const std::string& sub_path : sub_paths)
      {
        std::string filename;
        if (SplitPath(sub_path, nullptr, &filename, nullptr))
        {
          // Remove /main for main shader
          std::string name = (!strcasecmp(filename.c_str(), "main")) ?
                                 (shader_dirname) :
                                 (shader_dirname + DIR_SEP + filename);
          if (std::find(result.begin(), result.end(), filename) == result.end())
            result.push_back(name);
        }
      }
    }
  }

  // sort lexicographically
  std::sort(result.begin(), result.end());
  return result;
}

bool Shader::LoadShader(const char* name)
{
  std::string base_path = name;
  if (base_path.find_last_of(DIR_SEP_CHR) != std::string::npos)
    base_path.erase(base_path.find_last_of(DIR_SEP_CHR));

  std::string path;
  for (const std::string& base_dir : GetSearchDirectories())
  {
    std::string try_filename = base_dir + DIR_SEP + name + ".glsl";
    if (File::Exists(try_filename))
    {
      path = try_filename;
      base_path = base_dir + DIR_SEP + base_path;
      break;
    }
  }
  if (path.empty())
  {
    ERROR_LOG(VIDEO, "Failed to find post-processing shader '%s'", name);
    return false;
  }

  // Read to a single string we can work with
  std::string code;
  if (!File::ReadFileToString(path, code))
    return false;

  m_name = name;
  m_base_path = base_path;
  return ParseShader(code);
}

bool Shader::ParseShader(const std::string& source)
{
  // Find configuration block, if any
  constexpr char config_start_delimiter[] = "[configuration]";
  constexpr char config_end_delimiter[] = "[/configuration]";
  const size_t configuration_start = source.find(config_start_delimiter);
  const size_t configuration_end = source.find(config_end_delimiter);

  if (configuration_start == std::string::npos && configuration_end == std::string::npos)
  {
    // If there is no configuration block. Assume the entire file is code.
    m_source = source;
    CreateDefaultPass();
    return true;
  }

  // Remove the configuration area from the source string, leaving only the GLSL code.
  if (configuration_start > 0)
    m_source = source.substr(0, configuration_start);
  if (configuration_end != source.length())
    m_source += source.substr(configuration_end);

  // Extract configuration string, and parse options/passes
  std::string configuration_string = source.substr(
      configuration_start + ArraySize(config_start_delimiter) - 1,
      configuration_end - configuration_start - ArraySize(config_start_delimiter) - 1);
  return ParseConfiguration(configuration_string);
}

bool Shader::ParseConfiguration(const std::string& source)
{
  std::vector<ConfigBlock> config_blocks = ReadConfigSections(source);
  if (!ParseConfigSections(config_blocks))
    return false;

  // If no render passes are specified, create a default pass.
  if (m_passes.empty())
    CreateDefaultPass();

  return true;
}

std::vector<Shader::ConfigBlock> Shader::ReadConfigSections(const std::string& source)
{
  std::istringstream in(source);

  std::vector<ConfigBlock> config_blocks;
  ConfigBlock* current_block = nullptr;

  while (!in.eof())
  {
    std::string line;

    if (std::getline(in, line))
    {
#ifndef _WIN32
      // Check for CRLF eol and convert it to LF
      if (!line.empty() && line.at(line.size() - 1) == '\r')
        line.erase(line.size() - 1);
#endif

      if (line.size() > 0)
      {
        if (line[0] == '[')
        {
          size_t endpos = line.find("]");

          if (endpos != std::string::npos)
          {
            std::string sub = line.substr(1, endpos - 1);
            ConfigBlock section;
            section.name = sub;
            config_blocks.push_back(std::move(section));
            current_block = &config_blocks.back();
          }
        }
        else
        {
          std::string key, value;
          IniFile::ParseLine(line, &key, &value);
          if (!key.empty() && !value.empty())
          {
            if (current_block)
              current_block->values.emplace_back(key, value);
          }
        }
      }
    }
  }

  return config_blocks;
}

bool Shader::ParseConfigSections(const std::vector<ConfigBlock>& config_blocks)
{
  for (const ConfigBlock& option : config_blocks)
  {
    if (option.name == "Pass")
    {
      if (!ParsePassBlock(option))
        return false;
    }
    else
    {
      if (!ParseOptionBlock(option))
        return false;
    }
  }

  return true;
}

bool Shader::ParseOptionBlock(const ConfigBlock& block)
{
  // Initialize to default values, in case the configuration section is incomplete.
  Option option;
  u32 num_values = 0;

  static constexpr std::array<std::tuple<const char*, OptionType, u32>, 12> types = {{
      {"OptionBool", OptionType::Bool, 1},
      {"OptionFloat", OptionType::Float, 1},
      {"OptionFloat2", OptionType::Float, 2},
      {"OptionFloat3", OptionType::Float, 3},
      {"OptionFloat4", OptionType::Float, 4},
      {"OptionInt", OptionType::Int, 1},
      {"OptionInt2", OptionType::Int2, 2},
      {"OptionInt3", OptionType::Int3, 3},
      {"OptionInt4", OptionType::Int4, 4},
  }};

  for (const auto& it : types)
  {
    if (block.name == std::get<0>(it))
    {
      option.type = std::get<1>(it);
      num_values = std::get<2>(it);
      break;
    }
  }
  if (num_values == 0)
  {
    ERROR_LOG(VIDEO, "Unknown section name in post-processing shader config: '%s'",
              block.name.c_str());
    return false;
  }

  for (const auto& key : block.values)
  {
    if (key.first == "GUIName")
    {
      option.gui_name = key.second;
    }
    else if (key.first == "OptionName")
    {
      option.option_name = key.second;
    }
    else if (key.first == "DependentOption")
    {
      option.dependent_option = key.second;
    }
    else if (key.first == "ResolveAtCompilation")
    {
      TryParse(key.second, &option.compile_time_constant);
    }
    else if (key.first == "MinValue" || key.first == "MaxValue" || key.first == "DefaultValue" ||
             key.first == "StepAmount")
    {
      OptionValue* value_ptr;

      if (key.first == "MinValue")
        value_ptr = &option.min_value;
      else if (key.first == "MaxValue")
        value_ptr = &option.max_value;
      else if (key.first == "DefaultValue")
        value_ptr = &option.default_value;
      else if (key.first == "StepAmount")
        value_ptr = &option.step_amount;
      else
        continue;

      bool result = false;
      switch (option.type)
      {
      case OptionType::Bool:
        result = TryParse(key.second, &value_ptr->bool_value);
        break;

      case OptionType::Float:
      case OptionType::Float2:
      case OptionType::Float3:
      case OptionType::Float4:
      {
        std::vector<float> temp;
        result = TryParseVector(key.second, &temp);
        if (result && !temp.empty())
        {
          std::copy_n(temp.begin(), std::min(temp.size(), ArraySize(value_ptr->float_values)),
                      value_ptr->float_values);
        }
      }
      break;

      case OptionType::Int:
      case OptionType::Int2:
      case OptionType::Int3:
      case OptionType::Int4:
      {
        std::vector<s32> temp;
        result = TryParseVector(key.second, &temp);
        if (result && !temp.empty())
        {
          std::copy_n(temp.begin(), std::min(temp.size(), ArraySize(value_ptr->int_values)),
                      value_ptr->int_values);
        }
      }
      break;
      }

      if (!result)
      {
        ERROR_LOG(VIDEO, "Value parse fail at section '%s' key '%s' value '%s'", block.name.c_str(),
                  key.first.c_str(), key.second.c_str());
        return false;
      }
    }
    else
    {
      ERROR_LOG(VIDEO, "Unknown key '%s' in section '%s'", block.name.c_str(), key.first.c_str());
      return false;
    }
  }

  option.value = option.default_value;
  m_options[option.option_name] = option;
  return true;
}

bool Shader::ParsePassBlock(const ConfigBlock& block)
{
  Pass pass;
  for (const auto& option : block.values)
  {
    const std::string& key = option.first;
    const std::string& value = option.second;
    if (key == "EntryPoint")
    {
      pass.entry_point = value;
    }
    else if (key == "OutputScale")
    {
      TryParse(value, &pass.output_scale);
      if (pass.output_scale <= 0.0f)
        return false;
    }
    else if (key == "DependantOption")
    {
      const auto& dependant_option = m_options.find(value);
      if (dependant_option == m_options.end())
      {
        ERROR_LOG(VIDEO, "Unknown dependant option: %s", value.c_str());
        return false;
      }

      pass.dependent_option = value;
      dependant_option->second.is_pass_dependent_option = true;
    }
    else if (key.compare(0, 5, "Input") == 0 && key.length() > 5)
    {
      u32 texture_unit = key[5] - '0';
      if (texture_unit > POST_PROCESSING_MAX_TEXTURE_INPUTS)
      {
        ERROR_LOG(VIDEO, "Post processing configuration error: Out-of-range texture unit: %u",
                  texture_unit);
        return false;
      }

      // Input declared yet?
      Input* input = nullptr;
      for (Input& input_it : pass.inputs)
      {
        if (input_it.texture_unit == texture_unit)
        {
          input = &input_it;
          break;
        }
      }
      if (!input)
      {
        Input new_input;
        new_input.texture_unit = texture_unit;
        pass.inputs.push_back(std::move(new_input));
        input = &pass.inputs.back();
      }

      // Input#(Filter|Mode|Source)
      std::string extra = (key.length() > 6) ? key.substr(6) : "";
      if (extra.empty())
      {
        // Type
        if (value == "ColorBuffer")
        {
          input->type = InputType::ColorBuffer;
        }
        else if (value == "DepthBuffer")
        {
          input->type = InputType::DepthBuffer;
          m_requires_depth_buffer = true;
        }
        else if (value == "PreviousPass")
        {
          input->type = InputType::PreviousPassOutput;
        }
        else if (value.compare(0, 4, "Pass") == 0)
        {
          input->type = InputType::PassOutput;
          if (!TryParse(value.substr(4), &input->pass_output_index) ||
              input->pass_output_index >= m_passes.size())
          {
            ERROR_LOG(VIDEO, "Out-of-range render pass reference: %u", input->pass_output_index);
            return false;
          }
        }
        else
        {
          ERROR_LOG(VIDEO, "Invalid input type: %s", value.c_str());
          return false;
        }
      }
      else if (extra == "Filter")
      {
        if (value == "Nearest")
        {
          input->sampler_state.min_filter = SamplerState::Filter::Linear;
          input->sampler_state.mag_filter = SamplerState::Filter::Linear;
          input->sampler_state.mipmap_filter = SamplerState::Filter::Linear;
        }
        else if (value == "Linear")
        {
          input->sampler_state.min_filter = SamplerState::Filter::Point;
          input->sampler_state.mag_filter = SamplerState::Filter::Point;
          input->sampler_state.mipmap_filter = SamplerState::Filter::Point;
        }
        else
        {
          ERROR_LOG(VIDEO, "Invalid input filter: %s", value.c_str());
          return false;
        }
      }
      else if (extra == "Mode")
      {
        if (value == "Clamp")
        {
          input->sampler_state.wrap_u = SamplerState::AddressMode::Clamp;
          input->sampler_state.wrap_v = SamplerState::AddressMode::Clamp;
        }
        else if (value == "Wrap")
        {
          input->sampler_state.wrap_u = SamplerState::AddressMode::Repeat;
          input->sampler_state.wrap_v = SamplerState::AddressMode::Repeat;
        }
        else if (value == "WrapMirror")
        {
          input->sampler_state.wrap_u = SamplerState::AddressMode::MirroredRepeat;
          input->sampler_state.wrap_v = SamplerState::AddressMode::MirroredRepeat;
        }
        else if (value == "Border")
        {
          input->sampler_state.wrap_u = SamplerState::AddressMode::Border;
          input->sampler_state.wrap_v = SamplerState::AddressMode::Border;
        }
        else
        {
          ERROR_LOG(VIDEO, "Invalid input mode: %s", value.c_str());
          return false;
        }
      }
      else if (extra == "Source")
      {
        // Load external image
        std::string path = m_base_path + value;
        input->type = InputType::ExternalImage;
        input->external_image = LoadExternalImage(path);
        if (!input->external_image)
        {
          ERROR_LOG(VIDEO, "Unable to load external image at '%s'", value.c_str());
          return false;
        }
      }
      else
      {
        ERROR_LOG(VIDEO, "Unknown input key: %s", key.c_str());
        return false;
      }
    }
  }

  m_passes.push_back(std::move(pass));
  return true;
}

std::unique_ptr<ExternalImage> Shader::LoadExternalImage(const std::string& path)
{
  File::IOFile file(path, "rb");
  std::vector<u8> buffer(file.GetSize());
  if (!file.IsOpen() || !file.ReadBytes(buffer.data(), file.GetSize()))
    return false;

  return false;
}

void Shader::CreateDefaultPass()
{
  Input input;
  input.type = InputType::PreviousPassOutput;
  input.sampler_state = RenderState::GetLinearSamplerState();

  Pass pass;
  pass.entry_point = "main";
  pass.inputs.push_back(std::move(input));
  pass.output_scale = 1.0f;
  m_passes.push_back(std::move(pass));
}

void Shader::SetOptionf(const char* option, int index, float value)
{
  auto it = m_options.find(option);
  if (it->second.value.float_values[index] == value)
    return;

  it->second.value.float_values[index] = value;
  if (it->second.compile_time_constant || it->second.is_pass_dependent_option)
    m_requires_recompile.Set();
}

void Shader::SetOptioni(const char* option, int index, s32 value)
{
  auto it = m_options.find(option);
  if (it->second.value.int_values[index] == value)
    return;

  it->second.value.int_values[index] = value;
  if (it->second.compile_time_constant || it->second.is_pass_dependent_option)
    m_requires_recompile.Set();
}

void Shader::SetOptionb(const char* option, bool value)
{
  auto it = m_options.find(option);
  if (it->second.value.bool_value == value)
    return;

  it->second.value.bool_value = value;
  if (it->second.compile_time_constant || it->second.is_pass_dependent_option)
    m_requires_recompile.Set();
}

const Option* Shader::FindOption(const char* option) const
{
  auto iter = m_options.find(option);
  return iter != m_options.end() ? &iter->second : nullptr;
}

Config::Config() = default;

Config::~Config() = default;

bool Config::LoadConfigString(const std::string& config)
{
  m_valid = false;
  m_requires_depth_buffer = false;

  std::vector<std::string> shader_names;
  shader_names.push_back(config);

  // If the number of shaders hasn't changed, we don't need to recompile them.
  bool recompile_shaders = m_shaders.size() != shader_names.size();
  m_shaders.resize(shader_names.size());
  for (size_t i = 0; i < shader_names.size(); i++)
  {
    std::unique_ptr<Shader>& shader = m_shaders[i];
    if (!shader || shader->GetShaderName() != shader_names[i])
    {
      shader = std::make_unique<Shader>();
      if (!shader->LoadShader(shader_names[i].c_str()))
        return false;

      // TODO: Load options.
      recompile_shaders = true;
    }

    recompile_shaders |= shader->RequiresRecompile();
    m_requires_depth_buffer |= shader->RequiresDepthBuffer();
  }

  m_valid = true;
  return true;
}

Instance::Instance()
{
  m_timer.Start();
}

Instance::~Instance()
{
  m_timer.Stop();
}

static std::vector<std::string> GetShaders(const std::string& sub_dir = "")
{
  std::vector<std::string> paths =
      Common::DoFileSearch({File::GetUserPath(D_SHADERS_IDX) + sub_dir,
                            File::GetSysDirectory() + SHADERS_DIR DIR_SEP + sub_dir},
                           {".glsl"});
  std::vector<std::string> result;
  for (std::string path : paths)
  {
    std::string name;
    SplitPath(path, nullptr, &name, nullptr);
    result.push_back(name);
  }
  return result;
}

std::vector<std::string> Instance::GetShaderList()
{
  return GetShaders();
}

std::vector<std::string> Instance::GetAnaglyphShaderList()
{
  return GetShaders(ANAGLYPH_DIR DIR_SEP);
}

bool Instance::LoadConfig(const std::string& config)
{
  m_valid = false;
  m_render_passes.clear();
  m_target_width = 0;
  m_target_height = 0;
  m_target_format = AbstractTextureFormat::Undefined;

  if (!m_config.LoadConfigString(config))
    return false;

  return CompilePasses();
}

bool Instance::CompilePasses()
{
  m_valid = false;
  if (g_ActiveConfig.backend_info.bSupportsGeometryShaders && !m_geometry_shader)
  {
    m_geometry_shader = CompileGeometryShader();
    if (!m_geometry_shader)
      return false;
  }

  m_render_passes.clear();
  for (u32 shader_index = 0; shader_index < m_config.GetShaderCount(); shader_index++)
  {
    const Shader& shader = m_config.GetShader(shader_index);
    std::shared_ptr<AbstractShader> vertex_shader = CompileVertexShader(shader);
    if (!vertex_shader)
      return false;

    for (u32 pass_index = 0; pass_index < shader.GetPassCount(); pass_index++)
    {
      const Pass& pass = shader.GetPass(pass_index);

      // don't add passes which aren't enabled
      if (!pass.dependent_option.empty())
      {
        const Option* option =
            m_config.GetShader(shader_index).FindOption(pass.dependent_option.c_str());
        if (!option || option->type != OptionType::Bool || !option->value.bool_value)
          continue;
      }

      RenderPass render_pass;
      render_pass.output_scale = pass.output_scale;
      render_pass.shader_index = shader_index;
      render_pass.shader_pass_index = pass_index;
      render_pass.vertex_shader = vertex_shader;
      render_pass.pixel_shader = CompilePixelShader(shader, pass);
      if (!render_pass.pixel_shader)
        return false;

      render_pass.inputs.reserve(pass.inputs.size());
      for (const auto& input : pass.inputs)
      {
        InputBinding binding;
        if (!CreateInputBinding(shader_index, input, &binding))
          return false;

        render_pass.inputs.push_back(std::move(binding));
      }

      m_render_passes.push_back(std::move(render_pass));
    }
  }

  m_valid = true;
  return true;
}

bool Instance::CreateInputBinding(u32 shader_index, const Input& input, InputBinding* binding)
{
  binding->type = input.type;
  binding->texture_unit = input.texture_unit;
  binding->sampler_state = input.sampler_state;
  binding->texture_ptr = nullptr;
  binding->source_pass_index = 0;

  if (binding->type == InputType::ExternalImage)
  {
    binding->owned_texture_ptr = g_renderer->CreateTexture(
        TextureConfig(input.external_image->width, input.external_image->height, 1, 1, 1,
                      AbstractTextureFormat::RGBA8, 0));
    if (!binding->owned_texture_ptr)
      return false;

    binding->owned_texture_ptr->Load(0, input.external_image->width, input.external_image->height,
                                     input.external_image->width, input.external_image->data.data(),
                                     input.external_image->width * sizeof(u32) *
                                         input.external_image->height);
  }
  else if (binding->type == InputType::PassOutput)
  {
    binding->source_pass_index = GetRenderPassIndex(shader_index, input.pass_output_index);
    if (binding->source_pass_index >= m_render_passes.size())
    {
      // This should have been checked as part of the configuration parsing.
      return false;
    }
  }

  return true;
}

bool Instance::CreateOutputTextures(u32 width, u32 height, u32 layers, AbstractTextureFormat format)
{
  // Pipelines must be recompiled if format or layers(->GS) changes.
  const bool recompile_pipelines = m_target_format != format || m_target_layers != layers;

  m_target_width = width;
  m_target_height = height;
  m_target_layers = layers;
  m_target_format = format;
  m_valid = false;

  for (RenderPass& pass : m_render_passes)
  {
    const u32 output_width = std::max(1u, static_cast<u32>(width * pass.output_scale));
    const u32 output_height = std::max(1u, static_cast<u32>(height * pass.output_scale));

    pass.output_framebuffer.reset();
    pass.output_texture.reset();
    pass.output_texture = g_renderer->CreateTexture(TextureConfig(
        output_width, output_height, 1, layers, 1, format, AbstractTextureFlag_RenderTarget));
    if (!pass.output_texture)
      return false;
    pass.output_framebuffer = g_renderer->CreateFramebuffer(pass.output_texture.get(), nullptr);
    if (!pass.output_framebuffer)
      return false;
  }

  LinkPassOutputs();
  m_valid = true;

  if (recompile_pipelines && !CompilePipelines())
    return false;

  return true;
}

void Instance::LinkPassOutputs()
{
  m_last_pass_uses_color_buffer = false;
  for (RenderPass& render_pass : m_render_passes)
  {
    m_last_pass_uses_color_buffer = false;
    m_last_pass_scaled = render_pass.output_scale != 1.0f;
    for (InputBinding& binding : render_pass.inputs)
    {
      switch (binding.type)
      {
      case InputType::ColorBuffer:
        m_last_pass_uses_color_buffer = true;
        break;

      case InputType::PassOutput:
        binding.texture_ptr = m_render_passes.at(binding.source_pass_index).output_texture.get();
        break;

      default:
        break;
      }
    }
  }
}

bool Instance::Apply(AbstractFramebuffer* dest_fb, const MathUtil::Rectangle<int>& dest_rect,
                     const AbstractTexture* source_color_tex,
                     const AbstractTexture* source_depth_tex,
                     const MathUtil::Rectangle<int>& source_rect, int source_layer)
{
  ASSERT(m_valid);
  if (m_config.RequiresRecompile() && !CompilePasses())
    return false;

  const u32 dest_rect_width = static_cast<u32>(dest_rect.GetWidth());
  const u32 dest_rect_height = static_cast<u32>(dest_rect.GetHeight());
  if (m_target_width != dest_rect_width || m_target_height != dest_rect_height ||
      m_target_layers != dest_fb->GetLayers() || m_target_format != dest_fb->GetColorFormat())
  {
    if (!CreateOutputTextures(dest_rect_width, dest_rect_height, dest_fb->GetLayers(),
                              dest_fb->GetColorFormat()))
    {
      return false;
    }
  }

  // Determine whether we can skip the final copy by writing directly to the output texture, if the
  // last pass is not scaled, and the target isn't multisampled.
  const bool skip_final_copy = !m_last_pass_scaled && !m_last_pass_uses_color_buffer &&
                               dest_fb->GetColorAttachment() != source_color_tex &&
                               dest_fb->GetSamples() == 1;

  // Draw each pass.
  const AbstractTexture* last_pass_tex = source_color_tex;
  MathUtil::Rectangle<int> last_pass_rect = source_rect;
  u32 last_shader_index = m_config.GetShaderCount();
  bool uniforms_changed = true;
  for (size_t pass_index = 0; pass_index < m_render_passes.size(); pass_index++)
  {
    const RenderPass& pass = m_render_passes[pass_index];

    // If this is the last pass and we can skip the final copy, write directly to output texture.
    AbstractFramebuffer* output_fb;
    MathUtil::Rectangle<int> output_rect;
    if (pass_index == m_render_passes.size() - 1 && skip_final_copy)
    {
      output_fb = dest_fb;
      output_rect = dest_rect;
      g_renderer->SetFramebuffer(dest_fb);
    }
    else
    {
      output_fb = pass.output_framebuffer.get();
      output_rect = pass.output_framebuffer->GetRect();
      g_renderer->SetAndDiscardFramebuffer(output_fb);
    }

    g_renderer->SetPipeline(pass.pipeline.get());
    g_renderer->SetViewportAndScissor(
        g_renderer->ConvertFramebufferRectangle(output_rect, output_fb));

    for (const InputBinding& input : pass.inputs)
    {
      switch (input.type)
      {
      case InputType::ColorBuffer:
        g_renderer->SetTexture(input.texture_unit, source_color_tex);
        break;

      case InputType::DepthBuffer:
        g_renderer->SetTexture(input.texture_unit, source_depth_tex);
        break;

      case InputType::PreviousPassOutput:
        g_renderer->SetTexture(input.texture_unit, last_pass_tex);
        break;

      case InputType::ExternalImage:
      case InputType::PassOutput:
        g_renderer->SetTexture(input.texture_unit, input.texture_ptr);
        break;
      }

      g_renderer->SetSamplerState(input.texture_unit, input.sampler_state);
    }

    // Skip uniform update where possible to save bandwidth.
    uniforms_changed |= pass.shader_index != last_shader_index;
    if (uniforms_changed)
    {
      UploadUniformBuffer(m_config.GetShader(pass.shader_index), last_pass_tex, last_pass_rect,
                          source_color_tex, source_rect, source_layer);
    }

    g_renderer->Draw(0, 3);

    if (output_fb != dest_fb)
      output_fb->GetColorAttachment()->FinishedRendering();

    last_shader_index = pass.shader_index;
    last_pass_tex = pass.output_texture.get();
    last_pass_rect = pass.output_texture->GetRect();
  }

  // Copy the last pass output to the target if not done already
  if (!skip_final_copy)
    g_renderer->ScaleTexture(dest_fb, dest_rect, last_pass_tex, last_pass_rect);

  return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Uniform buffer management
////////////////////////////////////////////////////////////////////////////////////////////////////
constexpr u32 MAX_UBO_PACK_OFFSET = 4;

static u32 GetOptionComponents(OptionType type)
{
  switch (type)
  {
  case OptionType::Bool:
  case OptionType::Float:
  case OptionType::Int:
    return 1;

  case OptionType::Float2:
  case OptionType::Int2:
    return 2;

  case OptionType::Float3:
  case OptionType::Int3:
    return 3;

  case OptionType::Float4:
  case OptionType::Int4:
    return 4;
  }

  return 0;
}

static const char* GetOptionShaderType(OptionType type)
{
  switch (type)
  {
  case OptionType::Bool:
    return "bool";

  case OptionType::Float:
    return "float";

  case OptionType::Int:
    return "int";

  case OptionType::Float2:
    return "float2";

  case OptionType::Int2:
    return "int2";

  case OptionType::Float3:
    return "float3";

  case OptionType::Int3:
    return "int3";

  case OptionType::Float4:
    return "float4";

  case OptionType::Int4:
    return "int4";
  }

  return "";
}

static u32 GetOptionPadding(OptionType type, u32& pack_offset)
{
  // NOTE: Assumes std140 packing.
  const u32 components = GetOptionComponents(type);

  u32 padding = 0;
  if ((pack_offset + components) > MAX_UBO_PACK_OFFSET)
    padding = MAX_UBO_PACK_OFFSET - pack_offset;

  pack_offset += components;
  return padding;
}

struct BuiltinUniforms
{
  float prev_resolution[4];
  float prev_rect[4];
  float src_resolution[4];
  float src_rect[4];
  s32 u_time;
  u32 u_layer;

  static constexpr u32 pack_offset = 2;
};

size_t Instance::CalculateUniformsSize(const Shader& shader) const
{
  u32 pack_offset = BuiltinUniforms::pack_offset;
  u32 total_components = 0;
  for (const auto& it : shader.GetOptions())
  {
    const Option& option = it.second;
    if (option.compile_time_constant)
      continue;

    const u32 padding = GetOptionPadding(option.type, pack_offset);
    total_components += padding + GetOptionComponents(option.type);
  }

  if (pack_offset > 0)
    total_components += MAX_UBO_PACK_OFFSET - pack_offset;

  return sizeof(BuiltinUniforms) + total_components * sizeof(u32);
}

void Instance::UploadUniformBuffer(const Shader& shader, const AbstractTexture* prev_texture,
                                   const MathUtil::Rectangle<int>& prev_rect,
                                   const AbstractTexture* source_color_texture,
                                   const MathUtil::Rectangle<int>& source_rect, int source_layer)
{
  BuiltinUniforms builtin_uniforms;

  // prev_resolution
  const float prev_width_f = static_cast<float>(prev_texture->GetWidth());
  const float prev_height_f = static_cast<float>(prev_texture->GetHeight());
  const float rcp_prev_width = 1.0f / prev_width_f;
  const float rcp_prev_height = 1.0f / prev_height_f;
  builtin_uniforms.prev_resolution[0] = prev_width_f;
  builtin_uniforms.prev_resolution[1] = prev_height_f;
  builtin_uniforms.prev_resolution[2] = rcp_prev_width;
  builtin_uniforms.prev_resolution[3] = rcp_prev_height;

  // prev_rect
  builtin_uniforms.prev_rect[0] = static_cast<float>(source_rect.left) * rcp_prev_width;
  builtin_uniforms.prev_rect[1] = static_cast<float>(source_rect.top) * rcp_prev_height;
  builtin_uniforms.prev_rect[2] = static_cast<float>(source_rect.GetWidth()) * rcp_prev_width;
  builtin_uniforms.prev_rect[3] = static_cast<float>(source_rect.GetHeight()) * rcp_prev_height;

  // src_resolution
  const float src_width_f = static_cast<float>(source_color_texture->GetWidth());
  const float src_height_f = static_cast<float>(source_color_texture->GetHeight());
  const float rcp_src_width = 1.0f / src_width_f;
  const float rcp_src_height = 1.0f / src_height_f;
  builtin_uniforms.src_resolution[0] = src_width_f;
  builtin_uniforms.src_resolution[1] = src_height_f;
  builtin_uniforms.src_resolution[2] = rcp_src_width;
  builtin_uniforms.src_resolution[3] = rcp_src_height;

  // src_rect
  builtin_uniforms.src_rect[0] = static_cast<float>(source_rect.left) * rcp_src_width;
  builtin_uniforms.src_rect[1] = static_cast<float>(source_rect.top) * rcp_src_height;
  builtin_uniforms.src_rect[2] = static_cast<float>(source_rect.GetWidth()) * rcp_src_width;
  builtin_uniforms.src_rect[3] = static_cast<float>(source_rect.GetHeight()) * rcp_src_height;

  builtin_uniforms.u_time = static_cast<s32>(m_timer.GetTimeElapsed());
  builtin_uniforms.u_layer = static_cast<u32>(source_layer);

  u8* buf = m_uniform_staging_buffer.data();
  std::memcpy(buf, &builtin_uniforms, sizeof(builtin_uniforms));
  buf += sizeof(builtin_uniforms);

  u32 pack_offset = BuiltinUniforms::pack_offset;
  for (const auto& it : shader.GetOptions())
  {
    const Option& option = it.second;
    if (option.compile_time_constant)
      continue;

    const u32 components = GetOptionComponents(option.type);
    const u32 padding = GetOptionPadding(option.type, pack_offset);
    buf += padding * sizeof(u32);
    std::memcpy(buf, &option.value, sizeof(u32) * components);
    buf += sizeof(u32) * components;
  }

  g_vertex_manager->UploadUtilityUniforms(m_uniform_staging_buffer.data(),
                                          static_cast<u32>(buf - m_uniform_staging_buffer.data()));
}

std::string Instance::GetUniformBufferHeader(const Shader& shader) const
{
  std::stringstream ss;
  if (g_ActiveConfig.backend_info.api_type == APIType::D3D)
    ss << "cbuffer PSBlock : register(b0) {\n";
  else
    ss << "UBO_BINDING(std140, 1) uniform PSBlock {\n";

  // Builtin uniforms
  ss << "  float4 prev_resolution;\n";
  ss << "  float4 prev_rect;\n";
  ss << "  float4 src_resolution;\n";
  ss << "  float4 src_rect;\n";
  ss << "  uint u_time;\n";
  ss << "  int u_layer;\n";
  ss << "\n";

  // Custom options/uniforms - pack with std140 layout.
  u32 pack_offset = BuiltinUniforms::pack_offset;
  u32 padding_var_counter = 0;
  for (const auto& it : shader.GetOptions())
  {
    const Option& option = it.second;
    if (option.compile_time_constant)
      continue;

    const u32 components = GetOptionComponents(option.type);
    const u32 padding = GetOptionPadding(option.type, pack_offset);
    for (u32 i = 0; i < 3; i++)
      ss << "  float ubo_align_" << padding_var_counter++ << "_;\n";

    ss << "  " << GetOptionShaderType(option.type) << " " << option.option_name << ";\n";
  }

  ss << "};\n\n";
  return ss.str();
}

std::string Instance::GetPixelShaderHeader(const Shader& shader, const Pass& pass) const
{
  // NOTE: uv0 contains the texture coordinates in previous pass space.
  // uv1 contains the texture coordinates in color/depth buffer space.

  std::stringstream ss;
  ss << GetUniformBufferHeader(shader);

  if (g_ActiveConfig.backend_info.api_type == APIType::D3D)
  {
    // Rename main, since we need to set up globals
    ss << R"(
#define HLSL 1
#define main real_main
#define gl_FragCoord v_fragcoord

Texture2DArray samp[4] : register(t0);
SamplerState samp_ss[4] : register(s0);

static float3 v_tex0;
static float3 v_tex1;
static float4 v_fragcoord;
static float4 ocol0;

// Wrappers for sampling functions.
float4 SampleInput(int input) { return samp[input].Sample(samp_ss[input], float3(v_tex0.xy, float(u_layer))); }
float4 SampleInputLocation(int input, float2 location) { return samp[input].Sample(samp_ss[input], float3(location, float(u_layer))); }
float4 SampleInputLayer(int input, int layer) { return samp[input].Sample(samp_ss[input], float3(v_tex0.xy, float(layer))); }
float4 SampleInputLocationLayer(int input, float2 location, int layer) { return samp[input].Sample(samp_ss[input], float3(location, float(layer))); }
#define SampleInputOffset(input, offset) samp[input].Sample(samp_ss[input], float3(v_tex0.xy, float(u_layer)), offset)
#define SampleInputLocationOffset(input, location, offset) samp[input].Sample(samp_ss[input], float3(location.xy, float(u_layer)), offset)
#define SampleInputLayerOffset(input, layer, offset) samp[input].Sample(samp_ss[input], float3(v_tex0.xy, float(layer)), offset)
#define SampleInputLocationLayerOffset(input, location, layer, offset) samp[input].Sample(samp_ss[input], float3(location.xy, float(layer)), offset)

)";
  }
  else
  {
    ss << R"(
#define GLSL 1

// Type aliases.
#define float2x2 mat2
#define float3x3 mat3
#define float4x4 mat4
#define float4x3 mat4x3

// Utility functions.
float saturate(float x) { return clamp(x, 0.0f, 1.0f); }
float2 saturate(float2 x) { return clamp(x, float2(0.0f, 0.0f), float2(1.0f, 1.0f)); }
float3 saturate(float3 x) { return clamp(x, float3(0.0f, 0.0f, 0.0f), float3(1.0f, 1.0f, 1.0f)); }
float4 saturate(float4 x) { return clamp(x, float4(0.0f, 0.0f, 0.0f, 0.0f), float4(1.0f, 1.0f, 1.0f, 1.0f)); }

// Flipped multiplication order because GLSL matrices use column vectors.
float2 mul(float2x2 m, float2 v) { return (v * m); }
float3 mul(float3x3 m, float3 v) { return (v * m); }
float4 mul(float4x3 m, float3 v) { return (v * m); }
float4 mul(float4x4 m, float4 v) { return (v * m); }
float2 mul(float2 v, float2x2 m) { return (m * v); }
float3 mul(float3 v, float3x3 m) { return (m * v); }
float3 mul(float4 v, float4x3 m) { return (m * v); }
float4 mul(float4 v, float4x4 m) { return (m * v); }

SAMPLER_BINDING(0) uniform sampler2DArray samp[4];
VARYING_LOCATION(0) in float3 v_tex0;
FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;

// Wrappers for sampling functions.
float4 SampleInput(int input) { return texture(samp[input], float3(v_tex0.xy, float(u_layer))); }
float4 SampleInputLocation(int input, float2 location) { return texture(samp[input], float3(location, float(u_layer))); }
float4 SampleInputLayer(int input, int layer) { return texture(samp[input], float3(v_tex0.xy, float(layer))); }
float4 SampleInputLocationLayer(int input, float2 location, int layer) { return texture(samp[input], float3(location, float(layer))); }
#define SampleInputOffset(input, offset) textureOffset(samp[input], float3(v_tex0.xy, float(u_layer)), offset)
#define SampleInputLocationOffset(input, location, offset) textureOffset(samp[input], float3(location.xy, float(u_layer)), offset)
#define SampleInputLayerOffset(input, layer, offset) textureOffset(samp[input], float3(v_tex0.xy, float(layer)), offset)
#define SampleInputLocationLayerOffset(input, location, layer, offset) textureOffset(samp[input], float3(location.xy, float(layer)), offset)
)";
  }

  if (g_ActiveConfig.backend_info.bSupportsReversedDepthRange)
    ss << "#define DEPTH_VALUE(val) (val)\n";
  else
    ss << "#define DEPTH_VALUE(val) (1.0 - (val))\n";

  // Figure out which input indices map to color/depth/previous buffers.
  // If any of these buffers is not bound, defaults of zero are fine here,
  // since that buffer may not even be used by the shader.
  int color_buffer_index = 0;
  int depth_buffer_index = 0;
  int prev_output_index = 0;
  for (const Input& input : pass.inputs)
  {
    switch (input.type)
    {
    case InputType::ColorBuffer:
      color_buffer_index = input.texture_unit;
      break;

    case InputType::DepthBuffer:
      depth_buffer_index = input.texture_unit;
      break;

    case InputType::PreviousPassOutput:
      prev_output_index = input.texture_unit;
      break;

    default:
      break;
    }
  }

  // Hook the discovered indices up to macros.
  // This is to support the SampleDepth, SamplePrev, etc. macros.
  ss << "#define COLOR_BUFFER_INPUT_INDEX " << color_buffer_index << "\n";
  ss << "#define DEPTH_BUFFER_INPUT_INDEX " << depth_buffer_index << "\n";
  ss << "#define PREV_OUTPUT_INPUT_INDEX " << prev_output_index << "\n";

  // Add compile-time constants
  for (const auto& it : shader.GetOptions())
  {
    const Option& option = it.second;
    if (!option.compile_time_constant)
      continue;

    const u32 components = GetOptionComponents(option.type);
    ss << "#define " << option.option_name << " " << GetOptionShaderType(option.type) << "(";
    if (option.type == OptionType::Bool)
    {
      ss << option.value.bool_value ? "true" : "false";
    }
    else
    {
      for (u32 i = 0; i < components; i++)
      {
        if (i > 0)
          ss << ", ";
        switch (option.type)
        {
        case OptionType::Float:
        case OptionType::Float2:
        case OptionType::Float3:
        case OptionType::Float4:
          ss << option.value.float_values[i];
          break;
        case OptionType::Int:
        case OptionType::Int2:
        case OptionType::Int3:
        case OptionType::Int4:
          ss << option.value.int_values[i];
          break;
        }
      }
    }
    ss << ")\n";
  }

  ss << R"(

// Convert z/w -> linear depth
float ToLinearDepth(float depth)
{
  // TODO: Look at where we can pull better values for this from.
  const float NearZ = 0.001f;
  const float FarZ = 1.0f;
  const float A = (1.0f - (FarZ / NearZ)) / 2.0f;
  const float B = (1.0f + (FarZ / NearZ)) / 2.0f;
  return 1.0f / (A * depth + B);
}

// For backwards compatibility.
float4 Sample() { return SampleInput(PREV_OUTPUT_INPUT_INDEX); }
float4 SampleLocation(float2 location) { return SampleInputLocation(PREV_OUTPUT_INPUT_INDEX, location); }
float4 SampleLayer(int layer) { return SampleInputLayer(PREV_OUTPUT_INPUT_INDEX, layer); }
float4 SamplePrev() { return SampleInput(PREV_OUTPUT_INPUT_INDEX); }
float4 SamplePrevLocation(float2 location) { return SampleInputLocation(PREV_OUTPUT_INPUT_INDEX, location); }
float SampleRawDepth() { return DEPTH_VALUE(SampleInput(DEPTH_BUFFER_INPUT_INDEX).x); }
float SampleRawDepthLocation(float2 location) { return DEPTH_VALUE(SampleInputLocation(DEPTH_BUFFER_INPUT_INDEX, location).x); }
float SampleDepth() { return ToLinearDepth(SampleRawDepth()); }
float SampleDepthLocation(float2 location) { return ToLinearDepth(SampleRawDepthLocation(location)); }
#define SampleOffset(offset) (SampleInputOffset(COLOR_BUFFER_INPUT_INDEX, offset))
#define SampleLayerOffset(offset, layer) (SampleInputLayerOffset(COLOR_BUFFER_INPUT_INDEX, layer, offset))
#define SamplePrevOffset(offset) (SampleInputOffset(PREV_OUTPUT_INPUT_INDEX, offset))
#define SampleRawDepthOffset(offset) (DEPTH_VALUE(SampleInputOffset(DEPTH_BUFFER_INPUT_INDEX, offset).x))
#define SampleDepthOffset(offset) (ToLinearDepth(SampleRawDepthOffset(offset)))

float2 GetResolution() { return prev_resolution.xy; }
float2 GetInvResolution() { return prev_resolution.zw; }
float2 GetCoordinates() { return v_tex0.xy; }

float2 GetPrevResolution() { return prev_resolution.xy; }
float2 GetInvPrevResolution() { return prev_resolution.zw; }
float2 GetPrevRectOrigin() { return prev_rect.xy; }
float2 GetPrevRectSize() { return prev_rect.zw; }
float2 GetPrevCoordinates() { return v_tex0.xy; }

float2 GetSrcResolution() { return src_resolution.xy; }
float2 GetInvSrcResolution() { return src_resolution.zw; }
float2 GetSrcRectOrigin() { return src_rect.xy; }
float2 GetSrcRectSize() { return src_rect.zw; }
float2 GetSrcCoordinates() { return v_tex1.xy; }

float4 GetFragmentCoord() { return gl_FragCoord; }

float GetLayer() { return u_layer; }
float GetTime() { return u_time; }

void SetOutput(float4 color) { ocol0 = color; }

#define GetOption(x) (x)
#define OptionEnabled(x) (x)

)";
  return ss.str();
}

std::string Instance::GetPixelShaderFooter(const Shader& shader, const Pass& pass) const
{
  std::stringstream ss;
  if (g_ActiveConfig.backend_info.api_type == APIType::D3D)
  {
    ss << R"(

#undef main
void main(in float3 v_tex0_ : TEXCOORD0,
          in float3 v_tex1_ : TEXCOORD1,
          in float4 pos : SV_Position,
          out float4 ocol0_ : SV_Target)
{
  v_tex0 = v_tex0_;
  v_tex1 = v_tex1_;
  v_fragcoord = pos;
)";
    if (pass.entry_point == "main")
      ss << "  real_main();\n";
    else
      ss << "  " << pass.entry_point << "();\n";
    ss << R"(
  ocol0_ = ocol0;
})";
  }
  else
  {
    if (pass.entry_point != "main")
      ss << "void main() { " << pass.entry_point << "();\n }\n";
  }

  return ss.str();
}

bool Instance::CompilePipelines()
{
  AbstractPipelineConfig config = {};
  config.geometry_shader = m_target_layers > 1 ? m_geometry_shader.get() : nullptr;
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = RenderState::GetColorFramebufferState(m_target_format);
  config.usage = AbstractPipelineUsage::Utility;

  size_t uniforms_size = 0;
  for (RenderPass& render_pass : m_render_passes)
  {
    config.vertex_shader = render_pass.vertex_shader.get();
    config.pixel_shader = render_pass.pixel_shader.get();
    render_pass.pipeline = g_renderer->CreatePipeline(config);
    if (!render_pass.pipeline)
    {
      PanicAlert("Failed to compile post-processing pipeline");
      return false;
    }

    uniforms_size = std::max(uniforms_size,
                             CalculateUniformsSize(m_config.GetShader(render_pass.shader_index)));
  }

  m_uniform_staging_buffer.resize(uniforms_size);
  return true;
}

std::shared_ptr<AbstractShader> Instance::CompileVertexShader(const Shader& shader) const
{
  std::stringstream ss;
  ss << GetUniformBufferHeader(shader);

  if (g_ActiveConfig.backend_info.api_type == APIType::D3D)
  {
    ss << "void main(in uint id : SV_VertexID, out float3 v_tex0 : TEXCOORD0,\n";
    ss << "          out float3 v_tex1 : TEXCOORD1, out float4 opos : SV_Position) {\n";
  }
  else
  {
    ss << "VARYING_LOCATION(0) out float3 v_tex0;\n";
    ss << "VARYING_LOCATION(0) out float3 v_tex1;\n";
    ss << "#define id gl_VertexID\n";
    ss << "#define opos gl_Position\n";
    ss << "void main() {\n";
  }
  ss << "  v_tex0 = float3(float((id << 1) & 2), float(id & 2), 0.0f);\n";
  ss << "  opos = float4(v_tex0.xy * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n";
  ss << "  v_tex1 = float3(src_rect.xy + (src_rect.zw * v_tex0.xy), 0.0f);\n";

  if (g_ActiveConfig.backend_info.api_type == APIType::Vulkan)
    ss << "  opos.y = -opos.y;\n";

  ss << "}\n";

  std::unique_ptr<AbstractShader> vs =
      g_renderer->CreateShaderFromSource(ShaderStage::Vertex, ss.str());
  if (!vs)
  {
    PanicAlert("Failed to compile post-processing vertex shader");
    return nullptr;
  }

  // convert from unique_ptr to shared_ptr, as many passes share one VS.
  return std::shared_ptr<AbstractShader>(vs.release());
}

std::unique_ptr<AbstractShader> Instance::CompileGeometryShader() const
{
  std::string source = FramebufferShaderGen::GeneratePassthroughGeometryShader(2, 0);
  auto gs = g_renderer->CreateShaderFromSource(ShaderStage::Geometry, source);
  if (!gs)
  {
    PanicAlert("Failed to compile post-processing geometry shader");
    return nullptr;
  }

  return gs;
}

std::unique_ptr<AbstractShader> Instance::CompilePixelShader(const Shader& shader,
                                                             const Pass& pass) const
{
  // Generate GLSL and compile the new shader.
  std::stringstream ss;
  ss << GetPixelShaderHeader(shader, pass);
  ss << shader.GetShaderSource();
  ss << GetPixelShaderFooter(shader, pass);

  auto ps = g_renderer->CreateShaderFromSource(ShaderStage::Pixel, ss.str());
  if (!ps)
  {
    PanicAlert("Failed to compile post-processing pixel shader");
    return nullptr;
  }

  return ps;
}

const Instance::RenderPass* Instance::GetRenderPass(u32 shader_index, u32 pass_index) const
{
  for (size_t i = 0; i < m_render_passes.size(); i++)
  {
    const RenderPass& rp = m_render_passes[i];
    if (rp.shader_index == shader_index && rp.shader_pass_index == pass_index)
      return &rp;
  }

  return nullptr;
}

u32 Instance::GetRenderPassIndex(u32 shader_index, u32 pass_index) const
{
  for (size_t i = 0; i < m_render_passes.size(); i++)
  {
    const RenderPass& rp = m_render_passes[i];
    if (rp.shader_index == shader_index && rp.shader_pass_index == pass_index)
      return static_cast<u32>(i);
  }

  return static_cast<u32>(m_render_passes.size());
}

void Instance::RenderConfigUI()
{
  ImGui::SetNextWindowSize(ImVec2(512, 300), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Post-Processing Shader Configuration"))
  {
    ImGui::End();
    return;
  }

  static int current_shader_index = 0;
  if (ImGui::BeginChild("ShaderList", ImVec2(200, 0)))
  {
    ImGui::Text("Shaders:");

    ImGui::ListBox(
        "", &current_shader_index,
        [](void* data, int idx, const char** out_text) {
          *out_text = static_cast<Instance*>(data)->m_config.GetShader(idx).GetShaderName().c_str();
          return true;
        },
        this, m_config.GetShaderCount());

    ImGui::Button("Add");
    ImGui::SameLine();
    ImGui::Button("Remove");
  }
  ImGui::EndChild();

  ImGui::SameLine();
  if (ImGui::BeginChild("ShaderConfig"))
  {
    Shader& shader = m_config.GetShader(current_shader_index);
    ImGui::Text("Shader Name: %s", shader.GetShaderName().c_str());
    ImGui::Text("%u passes", shader.GetPassCount());

    for (const auto& it : shader.GetOptions())
    {
      ImGui::BeginGroup();
      ImGui::Text("%s (%s)", it.second.gui_name.c_str(), it.second.option_name.c_str());
      ImGui::EndGroup();
    }
  }
  ImGui::EndChild();

  ImGui::End();
}

}  // namespace VideoCommon::PostProcessing
