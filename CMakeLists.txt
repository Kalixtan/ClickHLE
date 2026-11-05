cmake_minimum_required(VERSION 3.20)

project(ClickHLE
    VERSION 0.1.0
    DESCRIPTION "An iPod click-wheel game emulator"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# --- Options -----------------------------------------------------------------

option(CLICKHLE_WERROR      "Treat warnings as errors" OFF)

set(CLICKHLE_CPU_BACKEND "unicorn" CACHE STRING
    "The only CPU backend")
if(NOT CLICKHLE_CPU_BACKEND STREQUAL "unicorn")
    message(FATAL_ERROR "ClickHLE supports only the Unicorn CPU backend")
endif()

# Compiler cache, if one is installed. Nothing depends on it being present.
find_program(CLICKHLE_COMPILER_CACHE NAMES ccache sccache)
if(CLICKHLE_COMPILER_CACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CLICKHLE_COMPILER_CACHE}")
endif()

# Warnings shared across targets.

add_library(clickhle_flags INTERFACE)

if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    target_compile_options(clickhle_flags INTERFACE /W4 /permissive- /utf-8)
    if(CLICKHLE_WERROR)
        target_compile_options(clickhle_flags INTERFACE /WX)
    endif()
else()
    target_compile_options(clickhle_flags INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion
        -Wold-style-cast -Wnon-virtual-dtor)
    if(CLICKHLE_WERROR)
        target_compile_options(clickhle_flags INTERFACE -Werror)
    endif()
endif()

# Core library.

add_library(clickhle_core
    src/common/log.cpp
    src/core/address_space.cpp
    src/core/cpu.cpp
    src/core/cpu_unicorn.cpp
    src/core/emulator.cpp
    src/loader/eapp_image.cpp
    src/hle/call_frame.cpp
    src/hle/format.cpp
    src/hle/guest_heap.cpp
    src/hle/registry.cpp
    src/hle/vfs.cpp
    src/hle/scheduler.cpp
    src/hle/modules.cpp
    src/hle/modules/opengles.cpp
    src/hle/modules/misc.cpp
    src/hle/modules/input.cpp
    src/hle/modules/settings.cpp
    src/hle/modules/fileio.cpp
    src/hle/modules/audio.cpp
    src/hle/modules/metadata.cpp
    src/audio/audio_device.cpp
    src/video/display.cpp
)

target_include_directories(clickhle_core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

target_link_libraries(clickhle_core PRIVATE clickhle_flags)

# Unicorn is dynamically loaded from the host system at runtime.
target_link_libraries(clickhle_core PRIVATE ${CMAKE_DL_LIBS})

target_compile_definitions(clickhle_core PUBLIC
    CLICKHLE_CPU_BACKEND_DEFAULT="${CLICKHLE_CPU_BACKEND}")

# Optional video and audio support.
# SDL3 (zlib) and desktop OpenGL. Both optional: without them the tree still
# builds and tests, and Display::create() returns a headless implementation that
# fails to open a window rather than crashing. Only src/video/display_sdl.cpp
# ever includes SDL, and only src/hle/modules/gles.cpp ever calls GL.

option(CLICKHLE_WITH_VIDEO "Build the SDL3/OpenGL video backend" ON)

set(CLICKHLE_VIDEO_ENABLED OFF)
if(CLICKHLE_WITH_VIDEO)
    find_package(SDL3 QUIET)
    find_package(OpenGL QUIET)
    if(SDL3_FOUND AND OpenGL_FOUND)
        target_sources(clickhle_core PRIVATE
            src/video/display_sdl.cpp
            src/audio/audio_sdl.cpp
            src/hle/modules/gles.cpp)
        target_link_libraries(clickhle_core PUBLIC SDL3::SDL3 OpenGL::GL)
        target_compile_definitions(clickhle_core PRIVATE CLICKHLE_HAVE_SDL=1)
        # SDL's own headers use C-style casts. They are not ours to fix, and
        # they are the only reason these files would trip the warning.
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
            set_source_files_properties(
                src/video/display_sdl.cpp
                src/audio/audio_sdl.cpp
                src/hle/modules/gles.cpp
                PROPERTIES COMPILE_OPTIONS "-Wno-old-style-cast")
        endif()
        set(CLICKHLE_VIDEO_ENABLED ON)
    endif()
endif()

if(NOT CLICKHLE_VIDEO_ENABLED)
    # No GL context to draw into; the entry points still register so coverage
    # reports and the ordinal-forwarding table stay meaningful.
    target_sources(clickhle_core PRIVATE src/hle/modules/gles_stub.cpp)
endif()

# Frontends.

add_executable(clickhle frontend/cli/main.cpp frontend/cli/boot.cpp)
target_link_libraries(clickhle PRIVATE clickhle_core clickhle_flags)

# Optional Qt desktop interface.

option(CLICKHLE_BUILD_GUI "Build the Qt desktop frontend when Qt is available" ON)
if(CLICKHLE_BUILD_GUI)
    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(Qt6Widgets_FOUND)
        add_executable(clickhle-gui frontend/gui/main.cpp)
        target_link_libraries(clickhle-gui PRIVATE
            clickhle_core clickhle_flags Qt6::Widgets)
    else()
        message(WARNING "Qt 6 Widgets was not found; clickhle-gui will not be built")
    endif()
endif()

message(STATUS "ClickHLE ${PROJECT_VERSION}")
message(STATUS "  CPU backend  : ${CLICKHLE_CPU_BACKEND}")
message(STATUS "  Video        : ${CLICKHLE_VIDEO_ENABLED}")
if(CLICKHLE_COMPILER_CACHE)
    message(STATUS "  Compiler cache: ${CLICKHLE_COMPILER_CACHE}")
else()
    message(STATUS "  Compiler cache: none found (optional)")
endif()
