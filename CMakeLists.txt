cmake_minimum_required(VERSION 3.24)
project(dreamcomp VERSION 0.1.0 LANGUAGES C CXX)

# dreamcomp: a framework for native PC ports of Dreamcast games. The recompiler and the emulated
# hardware are the vendored engine (engine/, dream-recomp, GPL-2.0); this layer adds what a port
# needs on top. A port repository adds this directory and then calls dreamcomp_add_port().
# Architecture: docs/ARCHITECTURE.md. Decisions: docs/DECISIONS.md.

set(DREAMCOMP_ROOT ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "dreamcomp source root")

# Engine defaults for ports (set before the engine's own option() calls, which then keep them).
# D-003: the interpreter fallback ships, so a call target discovery missed degrades to slower
# code instead of a crash, and the miss is reported for the next config round.
set(DREAM_DEV_INTERPRETER ON CACHE BOOL "SH-4 interpreter fallback for untranslated code")
set(DREAM_WERROR OFF CACHE BOOL "Warnings as errors in engine targets (engine CI keeps this on)")
# The generated code of one title is a single file of a few hundred MB that compiles on one core
# (Ecco: 244 MB, 514 s on a 32-thread machine). Split it once per hardware thread so the compiler
# builds the parts in parallel; -DDREAM_GEN_SHARDS=1 restores the single file.
cmake_host_system_information(RESULT _dc_threads QUERY NUMBER_OF_LOGICAL_CORES)
if(_dc_threads LESS 1)
  set(_dc_threads 1)
elseif(_dc_threads GREATER 64)
  set(_dc_threads 64)
endif()
set(DREAM_GEN_SHARDS ${_dc_threads} CACHE STRING "Parts the generated game code is split into")
if(NOT PROJECT_IS_TOP_LEVEL)
  set(DREAM_BUILD_TESTS OFF CACHE BOOL "Engine unit tests (on when building dreamcomp itself)")
endif()

# Third-party binaries fetched by `tools/dc.py deps` land in .deps/; find them without flags.
file(GLOB _dc_sdl3 LIST_DIRECTORIES true "${DREAMCOMP_ROOT}/.deps/SDL3-*/cmake")
if(WIN32 AND _dc_sdl3 AND NOT SDL3_DIR)
  list(GET _dc_sdl3 -1 _dc_sdl3)
  set(SDL3_DIR ${_dc_sdl3} CACHE PATH "SDL3 config directory")
endif()

add_subdirectory(engine)
include(cmake/DreamcompPort.cmake)

# The dreamcomp layer linked into every port executable. An OBJECT library, because its
# extensions register themselves from static constructors that a static library would drop.
add_library(dreamcomp_core OBJECT src/core.cpp src/settings.cpp src/hud.cpp src/sharp2d.cpp)
target_include_directories(dreamcomp_core PUBLIC ${DREAMCOMP_ROOT}/include)
target_link_libraries(dreamcomp_core PUBLIC dream::runtime dream::render)
target_compile_features(dreamcomp_core PUBLIC cxx_std_20)
target_compile_definitions(dreamcomp_core PRIVATE DREAMCOMP_VERSION="${PROJECT_VERSION}")
if(TARGET dream_render_vk)
  target_link_libraries(dreamcomp_core PUBLIC dream::render_vk)
  target_compile_definitions(dreamcomp_core PRIVATE DREAM_WITH_RENDERER)
endif()

# The launcher window players see before the game (docs/FRONTEND.md). Needs SDL3; a headless
# build (no SDL3, or -DDREAMCOMP_FRONTEND=OFF) configures without it and starts the game directly.
option(DREAMCOMP_FRONTEND "Build the RmlUi launcher (fetches FreeType, RmlUi and fonts)" ON)
if(DREAMCOMP_FRONTEND)
  find_package(SDL3 QUIET CONFIG)
  # Only with the renderer: the launcher hands over to a windowed game, which a headless build
  # (-DDREAM_RENDERER=OFF) cannot start.
  if(SDL3_FOUND AND TARGET dream_render_vk)
    include(cmake/DreamcompFrontend.cmake)
    target_link_libraries(dreamcomp_core PUBLIC dreamcomp_frontend)
    target_compile_definitions(dreamcomp_core PRIVATE DREAMCOMP_WITH_FRONTEND)
  else()
    message(STATUS "dreamcomp: no SDL3 or no renderer; the launcher is not built")
  endif()
endif()

# Developer tools (dreamcomp): not part of a port's executable.
if(TARGET dream_render)
  add_executable(dreamcomp_ta_dump tools/native/ta_dump.cpp)
  target_link_libraries(dreamcomp_ta_dump PRIVATE dream::render)
  add_executable(dreamcomp_ta_match tools/native/ta_match.cpp)
  target_link_libraries(dreamcomp_ta_match PRIVATE dream::render)
endif()

# HUD correction and overrides on synthetic frames (not part of `all`):
#   cmake --build <build> --target dreamcomp_hud_tests && <build>/dreamcomp/dreamcomp_hud_tests <scratch dir>
if(TARGET dream_render)
  add_executable(dreamcomp_hud_tests EXCLUDE_FROM_ALL src/tests/test_hud.cpp src/hud.cpp src/sharp2d.cpp)
  target_include_directories(dreamcomp_hud_tests PRIVATE ${DREAMCOMP_ROOT}/include)
  target_link_libraries(dreamcomp_hud_tests PRIVATE dream::render)
  target_compile_features(dreamcomp_hud_tests PRIVATE cxx_std_20)
endif()
