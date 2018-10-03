// Copyright 2014 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Common/GL/GLInterface/EGLWayland.h"
#include <wayland-egl.h>

GLContextEGLWayland::~GLContextEGLWayland()
{
  if (m_native_window)
    wl_egl_window_destroy(reinterpret_cast<wl_egl_window*>(m_native_window));
}

EGLDisplay GLContextEGLWayland::OpenEGLDisplay()
{
  return eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(m_wsi.display_connection));
}

EGLNativeWindowType GLContextEGLWayland::GetEGLNativeWindow(EGLConfig config)
{
  wl_egl_window* window =
      wl_egl_window_create(static_cast<wl_surface*>(m_wsi.render_surface),
                           m_wsi.render_surface_width, m_wsi.render_surface_height);
  if (!window)
    return {};

  return reinterpret_cast<EGLNativeWindowType>(window);
}
