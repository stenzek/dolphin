// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <glad/glad.h>
#include <memory>

#include "Common/GL/GLContext.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Software/SWOGLWindow.h"
#include "VideoBackends/Software/SWTexture.h"

SWOGLWindow::SWOGLWindow() = default;
SWOGLWindow::~SWOGLWindow() = default;

std::unique_ptr<SWOGLWindow> SWOGLWindow::Create(const WindowSystemInfo& wsi)
{
  std::unique_ptr<SWOGLWindow> window = std::unique_ptr<SWOGLWindow>(new SWOGLWindow());
  if (!window->Initialize(wsi))
  {
    PanicAlert("Failed to create OpenGL window");
    return nullptr;
  }

  return window;
}

bool SWOGLWindow::IsHeadless() const
{
  return m_gl_context->IsHeadless();
}

static GLuint CompileProgram(const std::string& vertexShader, const std::string& fragmentShader)
{
  // generate objects
  GLuint vertexShaderID = glCreateShader(GL_VERTEX_SHADER);
  GLuint fragmentShaderID = glCreateShader(GL_FRAGMENT_SHADER);
  GLuint programID = glCreateProgram();

  // compile vertex shader
  const char* shader = vertexShader.c_str();
  glShaderSource(vertexShaderID, 1, &shader, nullptr);
  glCompileShader(vertexShaderID);
#if defined(_DEBUG) || defined(DEBUGFAST)
  GLint Result = GL_FALSE;
  char stringBuffer[1024];
  GLsizei stringBufferUsage = 0;
  glGetShaderiv(vertexShaderID, GL_COMPILE_STATUS, &Result);
  glGetShaderInfoLog(vertexShaderID, 1024, &stringBufferUsage, stringBuffer);

  if (Result && stringBufferUsage)
  {
    ERROR_LOG(VIDEO, "GLSL vertex shader warnings:\n%s%s", stringBuffer, vertexShader.c_str());
  }
  else if (!Result)
  {
    ERROR_LOG(VIDEO, "GLSL vertex shader error:\n%s%s", stringBuffer, vertexShader.c_str());
  }
  else
  {
    INFO_LOG(VIDEO, "GLSL vertex shader compiled:\n%s", vertexShader.c_str());
  }

  bool shader_errors = !Result;
#endif

  // compile fragment shader
  shader = fragmentShader.c_str();
  glShaderSource(fragmentShaderID, 1, &shader, nullptr);
  glCompileShader(fragmentShaderID);
#if defined(_DEBUG) || defined(DEBUGFAST)
  glGetShaderiv(fragmentShaderID, GL_COMPILE_STATUS, &Result);
  glGetShaderInfoLog(fragmentShaderID, 1024, &stringBufferUsage, stringBuffer);

  if (Result && stringBufferUsage)
  {
    ERROR_LOG(VIDEO, "GLSL fragment shader warnings:\n%s%s", stringBuffer, fragmentShader.c_str());
  }
  else if (!Result)
  {
    ERROR_LOG(VIDEO, "GLSL fragment shader error:\n%s%s", stringBuffer, fragmentShader.c_str());
  }
  else
  {
    INFO_LOG(VIDEO, "GLSL fragment shader compiled:\n%s", fragmentShader.c_str());
  }

  shader_errors |= !Result;
#endif

  // link them
  glAttachShader(programID, vertexShaderID);
  glAttachShader(programID, fragmentShaderID);
  glLinkProgram(programID);
#if defined(_DEBUG) || defined(DEBUGFAST)
  glGetProgramiv(programID, GL_LINK_STATUS, &Result);
  glGetProgramInfoLog(programID, 1024, &stringBufferUsage, stringBuffer);

  if (Result && stringBufferUsage)
  {
    ERROR_LOG(VIDEO, "GLSL linker warnings:\n%s%s%s", stringBuffer, vertexShader.c_str(),
              fragmentShader.c_str());
  }
  else if (!Result && !shader_errors)
  {
    ERROR_LOG(VIDEO, "GLSL linker error:\n%s%s%s", stringBuffer, vertexShader.c_str(),
              fragmentShader.c_str());
  }
#endif

  // cleanup
  glDeleteShader(vertexShaderID);
  glDeleteShader(fragmentShaderID);

  return programID;
}

bool SWOGLWindow::Initialize(const WindowSystemInfo& wsi)
{
  m_gl_context = GLContext::Create(wsi);
  if (!m_gl_context)
    return false;

  // Init extension support.
  if (!gladLoadGL() || !GLAD_GL_VERSION_3_1 && !GLAD_GL_ES_VERSION_3_0)
  {
    ERROR_LOG(VIDEO,
              "GL extension initialization failed. Does your video card support OpenGL 3.1?");
    return false;
  }

  std::string frag_shader = "in vec2 TexCoord;\n"
                            "out vec4 ColorOut;\n"
                            "uniform sampler2D samp;\n"
                            "void main() {\n"
                            "	ColorOut = texture(samp, TexCoord);\n"
                            "}\n";

  std::string vertex_shader = "out vec2 TexCoord;\n"
                              "void main() {\n"
                              "	vec2 rawpos = vec2(gl_VertexID & 1, (gl_VertexID & 2) >> 1);\n"
                              "	gl_Position = vec4(rawpos * 2.0 - 1.0, 0.0, 1.0);\n"
                              "	TexCoord = vec2(rawpos.x, -rawpos.y);\n"
                              "}\n";

  std::string header = m_gl_context->IsGLES() ? "#version 300 es\n"
                                                "precision highp float;\n" :
                                                "#version 140\n";

  m_image_program = CompileProgram(header + vertex_shader, header + frag_shader);

  glUseProgram(m_image_program);

  glUniform1i(glGetUniformLocation(m_image_program, "samp"), 0);
  glGenTextures(1, &m_image_texture);
  glBindTexture(GL_TEXTURE_2D, m_image_texture);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  glGenVertexArrays(1, &m_image_vao);
  return true;
}

void SWOGLWindow::ShowImage(const AbstractTexture* image,
                            const MathUtil::Rectangle<int>& xfb_region)
{
  const SW::SWTexture* sw_image = static_cast<const SW::SWTexture*>(image);
  m_gl_context->Update();  // just updates the render window position and the backbuffer size

  GLsizei glWidth = (GLsizei)m_gl_context->GetBackBufferWidth();
  GLsizei glHeight = (GLsizei)m_gl_context->GetBackBufferHeight();

  glViewport(0, 0, glWidth, glHeight);

  glActiveTexture(GL_TEXTURE9);
  glBindTexture(GL_TEXTURE_2D, m_image_texture);

  // TODO: Apply xfb_region

  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);  // 4-byte pixel alignment
  glPixelStorei(GL_UNPACK_ROW_LENGTH, sw_image->GetConfig().width);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(sw_image->GetConfig().width),
               static_cast<GLsizei>(sw_image->GetConfig().height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
               sw_image->GetData());

  glUseProgram(m_image_program);

  glBindVertexArray(m_image_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  m_gl_context->Swap();
}
