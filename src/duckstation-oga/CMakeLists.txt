add_executable(duckstation-oga
  main.cpp
  oga_host_display.cpp
  oga_host_display.h
  oga_host_interface.cpp
  oga_host_interface.h
)

target_link_libraries(duckstation-oga PRIVATE core common imgui glad simpleini frontend-common go2)

