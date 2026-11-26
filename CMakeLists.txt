cmake_minimum_required(VERSION 3.24)
project(rta_sfw LANGUAGES C)

if(APPLE)
    enable_language(OBJC)
endif()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

option(RTA_BUILD_RAYLIB "Build Raylib example" ON)
option(RTA_BUILD_SDL3   "Build SDL3 example" ON)

include(FetchContent)
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/deps" CACHE PATH "Fetched dependencies directory" FORCE)

# Core DSP library (zero external dependencies)
add_library(rta_sfw STATIC
    src/rta_sfw.c
    src/rta_sfw_abi.c
)
target_include_directories(rta_sfw
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/includes"
)
if(UNIX AND NOT APPLE)
    target_link_libraries(rta_sfw PUBLIC m)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rta_sfw PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- Raylib Example ---
if(RTA_BUILD_RAYLIB)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG        5.5
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(raylib)

    add_executable(rta_raylib_simple examples/raylib/simple.c)
    target_link_libraries(rta_raylib_simple PRIVATE rta_sfw raylib)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(rta_raylib_simple PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# --- SDL3 Example ---
if(RTA_BUILD_SDL3)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-3.2.8
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(SDL3)

    add_executable(rta_sdl3_simple examples/sdl3/simple.c)
    target_link_libraries(rta_sdl3_simple PRIVATE rta_sfw SDL3::SDL3)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(rta_sdl3_simple PRIVATE -Wall -Wextra -Wpedantic)
    endif()


    add_executable(rta_sdl3_finalizer
        examples/sdl3/finalizer.c
        examples/sdl3/wav_stream_io.c
    )
    target_include_directories(rta_sdl3_finalizer PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/sdl3"
    )
    target_link_libraries(rta_sdl3_finalizer PRIVATE rta_sfw SDL3::SDL3)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(rta_sdl3_finalizer PRIVATE -Wall -Wextra -Wpedantic)
    endif()


endif()
