// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Common/GL/GLInterface/EGLWayland.h"
#include "VideoCommon/RenderBase.h"

#include <wayland-egl.h>

GLContextEGLWayland::~GLContextEGLWayland()
{
  if (m_native_window)
    wl_egl_window_destroy(reinterpret_cast<wl_egl_window*>(m_native_window));
}

EGLDisplay GLContextEGLWayland::OpenEGLDisplay()
{
  return eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR,
                               reinterpret_cast<EGLNativeDisplayType>(m_wsi.display_connection),
                               nullptr);
}

void GLContextEGLWayland::UpdateDimensions(int window_width, int window_height)
{
  wl_egl_window_resize(reinterpret_cast<wl_egl_window*>(m_native_window), window_width,
                       window_height, 0, 0);
  m_backbuffer_width = window_width;
  m_backbuffer_height = window_height;
}

EGLNativeWindowType GLContextEGLWayland::GetEGLNativeWindow(EGLConfig config)
{
  if (m_native_window)
    wl_egl_window_destroy(reinterpret_cast<wl_egl_window*>(m_native_window));

  // If passed handle is null (Wayland), use the interlock to mutually synchronize host and
  // renderer.
  if (m_wsi.render_surface == nullptr)
    std::tie(m_wsi.render_surface, m_wsi.render_surface_width, m_wsi.render_surface_height) =
        g_renderer->WaitForNewSurface();

  wl_egl_window* window =
      wl_egl_window_create(static_cast<wl_surface*>(m_wsi.render_surface),
                           m_wsi.render_surface_width, m_wsi.render_surface_height);
  if (!window)
    return {};

  return reinterpret_cast<EGLNativeWindowType>(window);
}
