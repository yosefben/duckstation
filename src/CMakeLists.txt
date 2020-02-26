add_subdirectory(common)
add_subdirectory(core)

if(BUILD_SDL_FRONTEND OR BUILD_QT_FRONTEND OR BUILD_OGA_FRONTEND)
  add_subdirectory(frontend-common)
endif()

if(BUILD_SDL_FRONTEND)
  add_subdirectory(duckstation-sdl)
endif()

if(BUILD_QT_FRONTEND)
  add_subdirectory(duckstation-qt)
endif()

if(BUILD_OGA_FRONTEND)
  add_subdirectory(duckstation-oga)
endif()
