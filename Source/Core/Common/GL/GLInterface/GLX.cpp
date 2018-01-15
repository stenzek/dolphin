// Copyright 2012 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <array>
#include <atomic>
#include <dlfcn.h>
#include <sstream>
#include <thread>

#include "Common/GL/GLInterface/GLX.h"
#include "Common/Logging/Log.h"

#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092

typedef GLXFBConfig* (*PFNGLXCHOOSEFBCONFIGPROC)(Display* dpy, int screen, const int* attrib_list,
                                                 int* nelements);
typedef XVisualInfo* (*PFNGLXGETVISUALFROMFBCONFIGPROC)(Display* dpy, GLXFBConfig config);
typedef GLXContext (*PFNGLXCREATECONTEXTPROC)(Display* dpy, XVisualInfo* vis, GLXContext shareList,
                                              Bool direct);
typedef void (*PFNGLXDESTROYCONTEXTPROC)(Display* dpy, GLXContext ctx);
typedef Bool (*PFNGLXMAKECURRENTPROC)(Display* dpy, GLXDrawable drawable, GLXContext ctx);
typedef void (*PFNGLXCOPYCONTEXTPROC)(Display* dpy, GLXContext src, GLXContext dst,
                                      unsigned long mask);
typedef void (*PFNGLXSWAPBUFFERSPROC)(Display* dpy, GLXDrawable drawable);
typedef Bool (*PFNGLXQUERYVERSIONPROC)(Display* dpy, int* maj, int* min);
typedef __GLXextFuncPtr (*PFNGLXGETPROCADDRESSPROC)(const GLubyte* procName);
typedef const char* (*PFNGLXQUERYEXTENSIONSSTRINGPROC)(Display* dpy, int screen);
typedef GLXContext (*PFNGLXCREATECONTEXTATTRIBSPROC)(Display*, GLXFBConfig, GLXContext, Bool,
                                                     const int*);
typedef int (*PFNGLXSWAPINTERVALSGIPROC)(int interval);

static PFNGLXCHOOSEFBCONFIGPROC pglXChooseFBConfig;
static PFNGLXGETVISUALFROMFBCONFIGPROC pglXGetVisualFromFBConfig;
static PFNGLXCREATECONTEXTPROC pglXCreateContext;
static PFNGLXDESTROYCONTEXTPROC pglXDestroyContext;
static PFNGLXMAKECURRENTPROC pglXMakeCurrent;
static PFNGLXCOPYCONTEXTPROC pglXCopyContext;
static PFNGLXSWAPBUFFERSPROC pglXSwapBuffers;
static PFNGLXQUERYVERSIONPROC pglXQueryVersion;
static PFNGLXQUERYEXTENSIONSSTRINGPROC pglXQueryExtensionsString;
static PFNGLXGETPROCADDRESSPROC pglXGetProcAddress;
static PFNGLXCREATECONTEXTATTRIBSPROC pglXCreateContextAttribs;
static PFNGLXSWAPINTERVALSGIPROC pglXSwapIntervalSGI;
static PFNGLXCREATEGLXPBUFFERSGIXPROC pglXCreateGLXPbufferSGIX;
static PFNGLXDESTROYGLXPBUFFERSGIXPROC pglXDestroyGLXPbufferSGIX;

static std::atomic_int s_glx_reference_count{0};
static void* s_glx_module = nullptr;

template <typename T>
static bool ResolveSymbol(void* module, T& func_ptr, const char* name)
{
  func_ptr = reinterpret_cast<typename std::remove_reference<decltype(func_ptr)>::type>(
      dlsym(module, name));
  return func_ptr != nullptr;
}

template <typename T>
static bool ResolveSymbol(T& func_ptr, const char* name)
{
  func_ptr = reinterpret_cast<typename std::remove_reference<decltype(func_ptr)>::type>(
      pglXGetProcAddress(reinterpret_cast<const GLubyte*>(name)));
  return func_ptr != nullptr;
}

static bool InitGLX()
{
  // SStrictly speaking this isn't race-free, as two threads could call InitGLX
  // at the same time, and attempt to use the function pointers before the thread
  // which resolves the symbols finishes executing. But at the time of writing,
  // that wasn't the case.
  if (s_glx_reference_count.fetch_add(1) > 0)
    return s_glx_module != nullptr;

  void* mod = dlopen("libGL.so", RTLD_NOW);
  if (!mod)
  {
    ERROR_LOG(VIDEO, "Failed to load libGL.so");
    return false;
  }

  if (!ResolveSymbol(mod, pglXChooseFBConfig, "glXChooseFBConfig") ||
      !ResolveSymbol(mod, pglXGetVisualFromFBConfig, "glXGetVisualFromFBConfig") ||
      !ResolveSymbol(mod, pglXCreateContext, "glXCreateContext") ||
      !ResolveSymbol(mod, pglXDestroyContext, "glXDestroyContext") ||
      !ResolveSymbol(mod, pglXMakeCurrent, "glXMakeCurrent") ||
      !ResolveSymbol(mod, pglXCopyContext, "glXCopyContext") ||
      !ResolveSymbol(mod, pglXSwapBuffers, "glXSwapBuffers") ||
      !ResolveSymbol(mod, pglXQueryVersion, "glXQueryVersion") ||
      !ResolveSymbol(mod, pglXQueryExtensionsString, "glXQueryExtensionsString") ||
      !ResolveSymbol(mod, pglXGetProcAddress, "glXGetProcAddress"))
  {
    ERROR_LOG(VIDEO, "Failed to resolve one or more symbols from libGL.so");
    dlclose(mod);
    return false;
  }

  // Optional symbols.
  ResolveSymbol(pglXCreateContextAttribs, "glXCreateContextAttribsARB");
  ResolveSymbol(pglXSwapIntervalSGI, "glXSwapIntervalSGI");
  ResolveSymbol(pglXCreateGLXPbufferSGIX, "glXCreateGLXPbufferSGIX");
  ResolveSymbol(pglXDestroyGLXPbufferSGIX, "glXDestroyGLXPbufferSGIX");

  s_glx_module = mod;
  return true;
}

static void ShutdownGLX()
{
  if (s_glx_reference_count.fetch_sub(1) > 1)
    return;

  pglXChooseFBConfig = nullptr;
  pglXGetVisualFromFBConfig = nullptr;
  pglXCreateContext = nullptr;
  pglXDestroyContext = nullptr;
  pglXMakeCurrent = nullptr;
  pglXCopyContext = nullptr;
  pglXSwapBuffers = nullptr;
  pglXGetProcAddress = nullptr;
  pglXCreateContextAttribs = nullptr;
  pglXSwapIntervalSGI = nullptr;
  pglXCreateGLXPbufferSGIX = nullptr;
  pglXDestroyGLXPbufferSGIX = nullptr;
  if (s_glx_module)
  {
    dlclose(s_glx_module);
    s_glx_module = nullptr;
  }
}

static bool s_glxError;
static int ctxErrorHandler(Display* dpy, XErrorEvent* ev)
{
  s_glxError = true;
  return 0;
}

void cInterfaceGLX::SwapInterval(int Interval)
{
  if (pglXSwapIntervalSGI && m_has_handle)
    pglXSwapIntervalSGI(Interval);
  else
    ERROR_LOG(VIDEO, "No support for SwapInterval (framerate clamped to monitor refresh rate).");
}
void* cInterfaceGLX::GetFuncAddress(const std::string& name)
{
  return (void*)pglXGetProcAddress((const GLubyte*)name.c_str());
}

void cInterfaceGLX::Swap()
{
  pglXSwapBuffers(dpy, win);
}

// Create rendering window.
// Call browser: Core.cpp:EmuThread() > main.cpp:Video_Initialize()
bool cInterfaceGLX::Create(void* window_handle, bool stereo, bool core)
{
  m_has_handle = !!window_handle;
  m_host_window = (Window)window_handle;

  dpy = XOpenDisplay(nullptr);
  int screen = DefaultScreen(dpy);

  // Load GLX functions.
  if (!InitGLX())
    return false;

  // checking glx version
  int glxMajorVersion, glxMinorVersion;
  pglXQueryVersion(dpy, &glxMajorVersion, &glxMinorVersion);
  if (glxMajorVersion < 1 || (glxMajorVersion == 1 && glxMinorVersion < 4))
  {
    ERROR_LOG(VIDEO, "glX-Version %d.%d detected, but need at least 1.4", glxMajorVersion,
              glxMinorVersion);
    return false;
  }

  // loading core context creation function
  if (!pglXCreateContextAttribs)
  {
    ERROR_LOG(VIDEO,
              "glXCreateContextAttribsARB not found, do you support GLX_ARB_create_context?");
    return false;
  }

  // choosing framebuffer
  int visual_attribs[] = {GLX_X_RENDERABLE,
                          True,
                          GLX_DRAWABLE_TYPE,
                          GLX_WINDOW_BIT,
                          GLX_X_VISUAL_TYPE,
                          GLX_TRUE_COLOR,
                          GLX_RED_SIZE,
                          8,
                          GLX_GREEN_SIZE,
                          8,
                          GLX_BLUE_SIZE,
                          8,
                          GLX_DEPTH_SIZE,
                          0,
                          GLX_STENCIL_SIZE,
                          0,
                          GLX_DOUBLEBUFFER,
                          True,
                          GLX_STEREO,
                          stereo ? True : False,
                          None};
  int fbcount = 0;
  GLXFBConfig* fbc = pglXChooseFBConfig(dpy, screen, visual_attribs, &fbcount);
  if (!fbc || !fbcount)
  {
    ERROR_LOG(VIDEO, "Failed to retrieve a framebuffer config");
    return false;
  }
  fbconfig = *fbc;
  XFree(fbc);

  s_glxError = false;
  XErrorHandler oldHandler = XSetErrorHandler(&ctxErrorHandler);

  // Create a GLX context.
  // We try to get a 4.0 core profile, else we try 3.3, else try it with anything we get.
  std::array<int, 9> context_attribs = {
      {GLX_CONTEXT_MAJOR_VERSION_ARB, 4, GLX_CONTEXT_MINOR_VERSION_ARB, 0,
       GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, GLX_CONTEXT_FLAGS_ARB,
       GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB, None}};
  ctx = nullptr;
  if (core)
  {
    ctx = pglXCreateContextAttribs(dpy, fbconfig, 0, True, &context_attribs[0]);
    XSync(dpy, False);
    m_attribs.insert(m_attribs.end(), context_attribs.begin(), context_attribs.end());
  }
  if (core && (!ctx || s_glxError))
  {
    std::array<int, 9> context_attribs_33 = {
        {GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3,
         GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, GLX_CONTEXT_FLAGS_ARB,
         GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB, None}};
    s_glxError = false;
    ctx = pglXCreateContextAttribs(dpy, fbconfig, 0, True, &context_attribs_33[0]);
    XSync(dpy, False);
    m_attribs.clear();
    m_attribs.insert(m_attribs.end(), context_attribs_33.begin(), context_attribs_33.end());
  }
  if (!ctx || s_glxError)
  {
    std::array<int, 5> context_attribs_legacy = {
        {GLX_CONTEXT_MAJOR_VERSION_ARB, 1, GLX_CONTEXT_MINOR_VERSION_ARB, 0, None}};
    s_glxError = false;
    ctx = pglXCreateContextAttribs(dpy, fbconfig, 0, True, &context_attribs_legacy[0]);
    XSync(dpy, False);
    m_attribs.clear();
    m_attribs.insert(m_attribs.end(), context_attribs_legacy.begin(), context_attribs_legacy.end());
  }
  if (!ctx || s_glxError)
  {
    ERROR_LOG(VIDEO, "Unable to create GL context.");
    XSetErrorHandler(oldHandler);
    return false;
  }

  std::string tmp;
  std::istringstream buffer(pglXQueryExtensionsString(dpy, screen));
  while (buffer >> tmp)
  {
    if (tmp == "GLX_SGIX_pbuffer")
      m_supports_pbuffer = true;
  }

  if (!CreateWindowSurface())
  {
    ERROR_LOG(VIDEO, "Error: CreateWindowSurface failed\n");
    XSetErrorHandler(oldHandler);
    return false;
  }

  XSetErrorHandler(oldHandler);
  return true;
}

bool cInterfaceGLX::Create(cInterfaceBase* main_context)
{
  cInterfaceGLX* glx_context = static_cast<cInterfaceGLX*>(main_context);

  m_has_handle = false;
  m_supports_pbuffer = glx_context->m_supports_pbuffer;
  dpy = glx_context->dpy;
  fbconfig = glx_context->fbconfig;
  s_glxError = false;
  XErrorHandler oldHandler = XSetErrorHandler(&ctxErrorHandler);

  ctx = pglXCreateContextAttribs(dpy, fbconfig, glx_context->ctx, True, &glx_context->m_attribs[0]);
  XSync(dpy, False);

  if (!ctx || s_glxError)
  {
    ERROR_LOG(VIDEO, "Unable to create GL context.");
    XSetErrorHandler(oldHandler);
    return false;
  }

  if (m_supports_pbuffer && !CreateWindowSurface())
  {
    ERROR_LOG(VIDEO, "Error: CreateWindowSurface failed\n");
    XSetErrorHandler(oldHandler);
    return false;
  }

  XSetErrorHandler(oldHandler);
  return true;
}

std::unique_ptr<cInterfaceBase> cInterfaceGLX::CreateSharedContext()
{
  std::unique_ptr<cInterfaceBase> context = std::make_unique<cInterfaceGLX>();
  if (!context->Create(this))
    return nullptr;
  return context;
}

bool cInterfaceGLX::CreateWindowSurface()
{
  if (m_has_handle)
  {
    // Get an appropriate visual
    XVisualInfo* vi = pglXGetVisualFromFBConfig(dpy, fbconfig);

    XWindow.Initialize(dpy);

    XWindowAttributes attribs;
    if (!XGetWindowAttributes(dpy, m_host_window, &attribs))
    {
      ERROR_LOG(VIDEO, "Window attribute retrieval failed");
      return false;
    }

    s_backbuffer_width = attribs.width;
    s_backbuffer_height = attribs.height;

    win = XWindow.CreateXWindow(m_host_window, vi);
    XFree(vi);
  }
  else if (m_supports_pbuffer)
  {
    win = m_pbuffer = pglXCreateGLXPbufferSGIX(dpy, fbconfig, 1, 1, nullptr);
    if (!m_pbuffer)
      return false;
  }

  return true;
}

void cInterfaceGLX::DestroyWindowSurface()
{
  if (m_has_handle)
  {
    XWindow.DestroyXWindow();
  }
  else if (m_supports_pbuffer && m_pbuffer)
  {
    pglXDestroyGLXPbufferSGIX(dpy, m_pbuffer);
    m_pbuffer = 0;
  }
}

bool cInterfaceGLX::MakeCurrent()
{
  return pglXMakeCurrent(dpy, win, ctx);
}

bool cInterfaceGLX::ClearCurrent()
{
  return pglXMakeCurrent(dpy, None, nullptr);
}

// Close backend
void cInterfaceGLX::Shutdown()
{
  DestroyWindowSurface();
  if (ctx)
  {
    pglXDestroyContext(dpy, ctx);

    // Don't close the display connection if we are a shared context.
    // Saves doing reference counting on this object, and the main context will always
    // be shut down last anyway.
    if (m_has_handle)
    {
      XCloseDisplay(dpy);
      ctx = nullptr;
    }
  }
  ShutdownGLX();
}
