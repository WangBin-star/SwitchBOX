cmake_minimum_required(VERSION 3.21)

project(SwitchBOX VERSION 1.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(SWITCHBOX_AUTHOR "WangBin-star")
set(SWITCHBOX_SWITCH_TITLE "SwitchBOX")
set(SWITCHBOX_RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
set(SWITCHBOX_BOREALIS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/library/borealis")
set(SWITCHBOX_BOREALIS_LIBRARY_DIR "${SWITCHBOX_BOREALIS_DIR}/library")
set(SWITCHBOX_RELEASE_VERSION "${PROJECT_VERSION}" CACHE STRING "Version label used for packaged Switch releases" FORCE)
set(SWITCHBOX_RELEASES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/releases")

option(SWITCHBOX_ENABLE_WARNINGS "Enable strict compiler warnings" ON)
option(SWITCHBOX_SWITCH_USE_DEKO3D "Use Borealis deko3d renderer on Nintendo Switch" ON)

if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    message(STATUS "Switch target requested without a toolchain file. Use cmake/toolchains/switch.cmake.")
endif ()

if (MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif ()

if (WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif ()

if (NOT EXISTS "${SWITCHBOX_BOREALIS_LIBRARY_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Borealis vendored sources are missing under ${SWITCHBOX_BOREALIS_LIBRARY_DIR}.")
endif ()

set(PLATFORM_DESKTOP OFF CACHE BOOL "Build Borealis for desktop" FORCE)
set(PLATFORM_SWITCH ON CACHE BOOL "Build Borealis for Switch" FORCE)
set(USE_SDL2 OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_FMT OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_TINYXML2 OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_TWEENY OFF CACHE BOOL "" FORCE)
set(BRLS_UNITY_BUILD OFF CACHE BOOL "" FORCE)
set(BRLS_RESOURCES_DIR "." CACHE STRING "" FORCE)
set(USE_DEKO3D ${SWITCHBOX_SWITCH_USE_DEKO3D} CACHE BOOL "" FORCE)
set(BOREALIS_USE_DEKO3D ${SWITCHBOX_SWITCH_USE_DEKO3D} CACHE BOOL "" FORCE)
set(FMT_OS OFF CACHE BOOL "" FORCE)
set(USE_STD_THREAD OFF CACHE BOOL "" FORCE)

add_subdirectory("${SWITCHBOX_BOREALIS_LIBRARY_DIR}" "${CMAKE_BINARY_DIR}/vendor/borealis")

set(SWITCHBOX_SWITCH_PORTLIBS_INCLUDE_DIR "${DEVKITPRO}/portlibs/switch/include")

if (EXISTS "/opt/devkitpro/portlibs/switch/include/glm/vec2.hpp")
    set(SWITCHBOX_SWITCH_PORTLIBS_INCLUDE_DIR "/opt/devkitpro/portlibs/switch/include")
endif ()

if (NOT EXISTS "${SWITCHBOX_SWITCH_PORTLIBS_INCLUDE_DIR}/glm/vec2.hpp")
    message(FATAL_ERROR "switch-glm headers not found under ${SWITCHBOX_SWITCH_PORTLIBS_INCLUDE_DIR}. Install the switch-glm package first.")
endif ()

target_include_directories(borealis PUBLIC "${SWITCHBOX_SWITCH_PORTLIBS_INCLUDE_DIR}")

add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(app)
