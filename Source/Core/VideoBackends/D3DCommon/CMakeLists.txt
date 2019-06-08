add_library(videod3dcommon
  Common.cpp
  Common.h
  Shader.cpp
  Shader.h
  SwapChain.cpp
  SwapChain.h
)

target_link_libraries(videod3dcommon
PUBLIC
  common
  videocommon
)

if(ENABLE_PRECOMPILED_HEADERS)
  target_precompiled_header(videod3dcommon PCH.h)
endif()
