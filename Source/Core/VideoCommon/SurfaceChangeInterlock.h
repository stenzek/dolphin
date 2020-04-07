// Copyright 2020 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// ---------------------------------------------------------------------------------------------
// Stateful interlock for synchronizing surface changes between host and renderer threads.
// It first blocks the host thread until the rendering surface can be safely destroyed.
// Then it blocks the rendering thread until the host comes back with a new surface.
//
// Designed to work with Wayland platform, but can be used for any platform that depends
// on synchronous surface changes.
// ---------------------------------------------------------------------------------------------

#include <condition_variable>
#include <mutex>

class SurfaceChangeInterlock
{
  enum class State
  {
    NotBlocking,
    BlockingHost,
    BlockingRenderer
  };

public:
  void BlockHostForSurfaceDestroy();
  void UnblockRendererWithNewSurface(void* native_handle, int width, int height);
  std::tuple<void*, int, int> WaitForNewSurface();

private:
  std::mutex m_mutex;
  std::condition_variable m_host_cv;
  std::condition_variable m_renderer_cv;
  void* m_native_handle = nullptr;
  int m_width = 0, m_height = 0;
  State m_state = State::NotBlocking;
};
