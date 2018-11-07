// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <unistd.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include "DolphinNoGUI/Platform.h"

#include "Common/MsgHandler.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/State.h"

#include <climits>
#include <cstdio>
#include <thread>

#include "VideoCommon/RenderBase.h"

@interface MetalView : NSView {
}
@end

@implementation MetalView
/*
-(BOOL) wantsUpdateLayer { return YES; }


+(Class) layerClass { return [CAMetalLayer class]; }

-(CALayer*) makeBackingLayer {
  CALayer* layer = [self.class.layerClass layer];
  CGSize viewScale = [self convertSizeToBacking: CGSizeMake(1.0, 1.0)];
  layer.contentsScale = MIN(viewScale.width, viewScale.height);
  return layer;
}*/

@end

namespace
{
class PlatformMacOS : public Platform
{
public:
  ~PlatformMacOS() override;

  bool Init() override;
  void SetTitle(const std::string& string) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;

private:
  void CloseDisplay();
  void UpdateWindowPosition();
  void ProcessEvents();

  NSWindow* m_window = nil;
  NSWindowController* m_window_controller = nil;
  NSView* m_view = nil;
};

PlatformMacOS::~PlatformMacOS()
{
  if (m_view != nil)
    [m_view release];
  if (m_window_controller != nil)
    [m_window_controller release];
  if (m_window != nil)
    [m_window release];
}

bool PlatformMacOS::Init()
{
  NSUInteger window_style = NSTitledWindowMask | NSClosableWindowMask | NSResizableWindowMask;

  const NSRect window_rc = NSMakeRect(100, 100, 640, 480);
  m_window = [[NSWindow alloc] initWithContentRect:window_rc
                                        styleMask:window_style
                                        backing:NSBackingStoreBuffered
                                        defer:NO];

  m_window_controller = [[NSWindowController alloc] initWithWindow:m_window];

  m_view = [[NSView alloc] initWithFrame: window_rc];
  [m_window setContentView: m_view];
  [m_window orderFrontRegardless];
  ProcessEvents();

  //m_view.wantsLayer = YES;
  //[m_view setLayer: [CAMetalLayer layer]];


  return true;
}

void PlatformMacOS::SetTitle(const std::string& string)
{
  //NSString* title_ns = [[NSString alloc] initWithCString: string.c_str()];
  //[m_window setTitle: title_ns];
  //[title_ns release];
  printf("%s\n", string.c_str());
}

void PlatformMacOS::MainLoop()
{
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs();
    ProcessEvents();
    UpdateWindowPosition();

    // TODO: Is this sleep appropriate?
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

WindowSystemInfo PlatformMacOS::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::MacOS;
  wsi.display_connection = nullptr;
  wsi.render_surface = reinterpret_cast<void*>(m_view);
  return wsi;
}

void PlatformMacOS::UpdateWindowPosition()
{
  if (m_window_fullscreen)
    return;

  NSRect frame = [m_window frame];

  auto& config = SConfig::GetInstance();
  config.iRenderWindowXPos = static_cast<int>(frame.origin.x);
  config.iRenderWindowYPos = static_cast<int>(frame.origin.y);
  config.iRenderWindowWidth = static_cast<int>(frame.size.width);
  config.iRenderWindowHeight = static_cast<int>(frame.size.height);
}

void PlatformMacOS::ProcessEvents()
{
  NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
  for (;;)
  {
    NSEvent* event = [NSApp nextEventMatchingMask: NSAnyEventMask untilDate: NSDate.distantPast inMode: NSDefaultRunLoopMode dequeue: YES];
    if (event == nil)
      break;

    [NSApp sendEvent: event];
    [NSApp updateWindows];
  }

  [pool release];
}
}  // namespace

std::unique_ptr<Platform> Platform::CreateMacOSPlatform()
{
  return std::make_unique<PlatformMacOS>();
}
