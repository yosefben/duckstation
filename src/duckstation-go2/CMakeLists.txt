find_package(EGL REQUIRED)

add_executable(duckstation-go2
  go2_controller_interface.cpp
  go2_controller_interface.h
  go2_host_display.cpp
  go2_host_display.h
  go2_host_interface.cpp
  go2_host_interface.h
  main.cpp
)

target_link_libraries(duckstation-go2 PRIVATE core common imgui glad frontend-common scmversion vulkan-loader go2 EGL::EGL)

