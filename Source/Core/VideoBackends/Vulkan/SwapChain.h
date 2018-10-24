// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/WindowSystemInfo.h"
#include "VideoBackends/Vulkan/Constants.h"
#include "VideoBackends/Vulkan/Texture2D.h"

namespace Vulkan
{
class CommandBufferManager;
class ObjectCache;

class SwapChain
{
public:
  SwapChain(const WindowSystemInfo& wsi, VkSurfaceKHR surface, bool vsync);
  ~SwapChain();

  // Creates a vulkan-renderable surface for the specified window handle.
  static VkSurfaceKHR CreateVulkanSurface(VkInstance instance, const WindowSystemInfo& wsi);

  // Create a new swap chain from a pre-existing surface.
  static std::unique_ptr<SwapChain> Create(const WindowSystemInfo& wsi, VkSurfaceKHR surface,
                                           bool vsync);

  VkSurfaceKHR GetSurface() const { return m_surface; }
  VkSurfaceFormatKHR GetSurfaceFormat() const { return m_surface_format; }
  bool IsVSyncEnabled() const { return m_vsync_enabled; }
  bool IsStereoEnabled() const { return m_layers == 2; }
  VkSwapchainKHR GetSwapChain() const { return m_swap_chain; }
  VkRenderPass GetRenderPass() const { return m_render_pass; }
  u32 GetWidth() const { return m_width; }
  u32 GetHeight() const { return m_height; }
  u32 GetCurrentImageIndex() const { return m_current_swap_chain_image_index; }
  VkImage GetCurrentImage() const
  {
    return m_swap_chain_images[m_current_swap_chain_image_index].image;
  }
  Texture2D* GetCurrentTexture() const
  {
    return m_swap_chain_images[m_current_swap_chain_image_index].texture.get();
  }
  VkFramebuffer GetCurrentFramebuffer() const
  {
    return m_swap_chain_images[m_current_swap_chain_image_index].framebuffer;
  }

  VkResult AcquireNextImage(VkSemaphore available_semaphore);

  bool RecreateSurface(void* native_handle);
  bool ResizeSwapChain();
  bool RecreateSwapChain();

  // Change vsync enabled state. This may fail as it causes a swapchain recreation.
  bool SetVSync(bool enabled);

private:
  bool SelectSurfaceFormat();
  bool SelectPresentMode();

  bool CreateSwapChain();
  void DestroySwapChain();

  bool CreateRenderPass();

  bool SetupSwapChainImages();
  void DestroySwapChainImages();

  void DestroySurface();

  struct SwapChainImage
  {
    VkImage image;
    std::unique_ptr<Texture2D> texture;
    VkFramebuffer framebuffer;
  };

  WindowSystemInfo m_wsi;
  VkSurfaceKHR m_surface = VK_NULL_HANDLE;
  VkSurfaceFormatKHR m_surface_format = {};
  VkPresentModeKHR m_present_mode = VK_PRESENT_MODE_RANGE_SIZE_KHR;
  bool m_vsync_enabled;

  VkSwapchainKHR m_swap_chain = VK_NULL_HANDLE;
  std::vector<SwapChainImage> m_swap_chain_images;
  u32 m_current_swap_chain_image_index = 0;

  VkRenderPass m_render_pass = VK_NULL_HANDLE;

  u32 m_width = 0;
  u32 m_height = 0;
  u32 m_layers = 0;
};

}  // namespace Vulkan
