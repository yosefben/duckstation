add_library(go2
  include/go2/audio.h
  include/go2/display.h
  include/go2/input.h
  include/go2/queue.h
  src/audio.c
  src/display.c
  src/input.c
  src/queue.c
)

target_include_directories(go2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/go2)
target_include_directories(go2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# TODO: find_package for these
target_include_directories(go2 PRIVATE /usr/include/drm)
target_link_libraries(go2 PRIVATE openal EGL evdev gbm drm m rga png asound)

target_link_libraries(go2 PUBLIC Threads::Threads)
