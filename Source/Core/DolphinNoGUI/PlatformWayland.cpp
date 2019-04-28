// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <unistd.h>

#include "DolphinNoGUI/Platform.h"

#include "Common/MsgHandler.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/State.h"

#include <climits>
#include <cstdio>
#include <cstring>

#include <wayland-client-protocol.h>

#include "UICommon/X11Utils.h"
#include "VideoCommon/RenderBase.h"

namespace
{
class PlatformWayland : public Platform
{
public:
  ~PlatformWayland() override;

  bool Init() override;
  void SetTitle(const std::string& string) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const;

private:
  void ProcessEvents();

  static void ShellSurfacePing(void* data, wl_shell_surface* surface, uint32_t serial);
  static void ShellSurfaceConfigure(void* data, wl_shell_surface* surface, uint32_t edges,
                                    int32_t width, int32_t height);
  static void ShellSurfacePopupDone(void* data, wl_shell_surface* surface);
  static void GlobalRegistryHandler(void* data, wl_registry* registry, uint32_t id,
                                    const char* interface, uint32_t version);
  static void GlobalRegistryRemover(void* data, wl_registry* registry, uint32_t id);

  wl_display* m_display = nullptr;
  wl_registry* m_registry = nullptr;
  wl_compositor* m_compositor = nullptr;
  wl_surface* m_surface = nullptr;
  wl_region* m_region = nullptr;
  wl_shell* m_shell = nullptr;
  wl_shell_surface* m_shell_surface = nullptr;

  u32 m_surface_width = 0;
  u32 m_surface_height = 0;
};

PlatformWayland::~PlatformWayland()
{
  if (m_shell_surface)
    wl_shell_surface_destroy(m_shell_surface);
  if (m_surface)
    wl_surface_destroy(m_surface);
  if (m_region)
    wl_region_destroy(m_region);
  if (m_shell)
    wl_shell_destroy(m_shell);
  if (m_compositor)
    wl_compositor_destroy(m_compositor);
  if (m_registry)
    wl_registry_destroy(m_registry);
  if (m_display)
    wl_display_disconnect(m_display);
}

void PlatformWayland::ShellSurfacePing(void* data, wl_shell_surface* surface, uint32_t serial)
{
  wl_shell_surface_pong(surface, serial);
}

void PlatformWayland::ShellSurfaceConfigure(void* data, wl_shell_surface* surface, uint32_t edges,
                                            int32_t width, int32_t height)
{
  PlatformWayland* platform = static_cast<PlatformWayland*>(data);
  platform->m_surface_width = static_cast<u32>(width);
  platform->m_surface_height = static_cast<u32>(height);
}

void PlatformWayland::ShellSurfacePopupDone(void* data, wl_shell_surface* surface)
{
}

void PlatformWayland::GlobalRegistryHandler(void* data, wl_registry* registry, uint32_t id,
                                            const char* interface, uint32_t version)
{
  PlatformWayland* platform = static_cast<PlatformWayland*>(data);
  if (std::strcmp(interface, "wl_compositor") == 0)
  {
    platform->m_compositor = static_cast<wl_compositor*>(
        wl_registry_bind(platform->m_registry, id, &wl_compositor_interface, 1));
  }
  else if (std::strcmp(interface, "wl_shell") == 0)
  {
    platform->m_shell =
        static_cast<wl_shell*>(wl_registry_bind(platform->m_registry, id, &wl_shell_interface, 1));
  }
}

void PlatformWayland::GlobalRegistryRemover(void* data, wl_registry* registry, uint32_t id)
{
}

bool PlatformWayland::Init()
{
  m_display = wl_display_connect(nullptr);
  if (!m_display)
  {
    PanicAlert("Failed to connect to Wayland display.");
    return false;
  }

  static const wl_registry_listener registry_listener = {GlobalRegistryHandler,
                                                         GlobalRegistryRemover};
  m_registry = wl_display_get_registry(m_display);
  wl_registry_add_listener(m_registry, &registry_listener, this);

  // Call back to registry listener to get compositor/shell.
  wl_display_dispatch(m_display);
  wl_display_roundtrip(m_display);

  // We need a shell/compositor, or at least one we understand.
  if (!m_compositor || !m_display)
  {
    std::fprintf(stderr, "Missing Wayland shell/compositor\n");
    return false;
  }

  // Create the compositor and shell surface.
  m_surface_width = Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH);
  m_surface_height = Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT);
  if (!(m_surface = wl_compositor_create_surface(m_compositor)) ||
      !(m_shell_surface = wl_shell_get_shell_surface(m_shell, m_surface)))
  {
    std::fprintf(stderr, "Failed to create compositor/shell surfaces\n");
    return false;
  }

  static const wl_shell_surface_listener shell_surface_listener = {
      ShellSurfacePing, ShellSurfaceConfigure, ShellSurfacePopupDone};
  wl_shell_surface_add_listener(m_shell_surface, &shell_surface_listener, this);
  wl_shell_surface_set_toplevel(m_shell_surface);

  // Create region in the surface to draw into.
  m_region = wl_compositor_create_region(m_compositor);
  wl_region_add(m_region, Config::Get(Config::MAIN_RENDER_WINDOW_XPOS),
                Config::Get(Config::MAIN_RENDER_WINDOW_YPOS), m_surface_width, m_surface_height);
  wl_surface_set_opaque_region(m_surface, m_region);
  return true;
}

void PlatformWayland::SetTitle(const std::string& string)
{
  wl_shell_surface_set_title(m_shell_surface, string.c_str());
}

void PlatformWayland::MainLoop()
{
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs();
    ProcessEvents();

    // TODO: Is this sleep appropriate?
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

WindowSystemInfo PlatformWayland::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Wayland;
  wsi.display_connection = static_cast<void*>(m_display);
  wsi.render_surface = reinterpret_cast<void*>(m_surface);
  wsi.render_surface_width = m_surface_width;
  wsi.render_surface_height = m_surface_height;
  return wsi;
}

void PlatformWayland::ProcessEvents()
{
  wl_display_dispatch_pending(m_display);
}
}  // namespace

std::unique_ptr<Platform> Platform::CreateWaylandPlatform()
{
  return std::make_unique<PlatformWayland>();
}
