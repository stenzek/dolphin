// Copyright 2020 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/SurfaceChangeInterlock.h"

#include "Common/Assert.h"

void SurfaceChangeInterlock::BlockHostForSurfaceDestroy()
{
  std::unique_lock<std::mutex> lock(m_mutex);
  DEBUG_ASSERT(m_state == State::NotBlocking);
  m_state = State::BlockingHost;
  m_host_cv.wait(lock);
}

void SurfaceChangeInterlock::UnblockRendererWithNewSurface(void* native_handle, int width,
                                                           int height)
{
  std::unique_lock<std::mutex> lock(m_mutex);
  DEBUG_ASSERT(m_state == State::BlockingRenderer);
  m_native_handle = native_handle;
  m_width = width;
  m_height = height;
  m_renderer_cv.notify_one();
}

std::tuple<void*, int, int> SurfaceChangeInterlock::WaitForNewSurface()
{
  std::unique_lock<std::mutex> lock(m_mutex);
  DEBUG_ASSERT(m_state == State::BlockingHost);
  m_host_cv.notify_one();
  m_state = State::BlockingRenderer;
  m_renderer_cv.wait(lock);
  m_state = State::NotBlocking;
  return {m_native_handle, m_width, m_height};
}
