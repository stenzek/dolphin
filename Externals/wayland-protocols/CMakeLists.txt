set(SRCS
  xdg-shell-client-protocol.c
  zxdg-decoration-unstable-v1-client-protocol.c
)

add_library(wayland-protocols "${SRCS}")
target_include_directories(wayland-protocols INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
