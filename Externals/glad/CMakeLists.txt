add_library(glad src/glad.c)
target_include_directories(glad PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(ENABLE_EGL AND EGL_FOUND)
	target_sources(glad src/glad_egl.c)
endif()

if(ENABLE_X11 AND X11_FOUND)
	target_sources(glad src/glad_glx.c)
endif()

if(WIN32)
	target_sources(glad src/glad_wgl.c)
endif()
