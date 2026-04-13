cmake_minimum_required(VERSION 3.21)

project(SwitchBOX VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(SWITCHBOX_AUTHOR "WangBin-star")
set(SWITCHBOX_SWITCH_TITLE "SwitchBOX")
set(SWITCHBOX_RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")

option(SWITCHBOX_BUILD_DESKTOP "Build the desktop debug target" ON)
option(SWITCHBOX_BUILD_SWITCH "Build the Nintendo Switch target" OFF)
option(SWITCHBOX_ENABLE_WARNINGS "Enable strict compiler warnings" ON)

if (NOT SWITCHBOX_BUILD_DESKTOP AND NOT SWITCHBOX_BUILD_SWITCH)
    message(FATAL_ERROR "At least one build target must be enabled.")
endif ()

if (SWITCHBOX_BUILD_SWITCH AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    message(STATUS "Switch target requested without a toolchain file. Use cmake/toolchains/switch.cmake.")
endif ()

if (MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif ()

add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(app)
