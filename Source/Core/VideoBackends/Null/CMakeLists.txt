add_library(videonull
  NullBackend.cpp
  NullTexture.cpp
  NullTexture.h
  PerfQuery.h
  Render.cpp
  Render.h
  TextureCache.h
  VertexManager.cpp
  VertexManager.h
  VideoBackend.h
)

target_link_libraries(videonull
PUBLIC
  common
  videocommon
)

if(ENABLE_PRECOMPILED_HEADERS)
  target_precompiled_header(videonull PCH.h)
endif()
