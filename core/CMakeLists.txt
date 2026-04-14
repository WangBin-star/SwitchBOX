add_library(switchbox_core STATIC
    source/app_config.cpp
    source/build_info.cpp
    source/language.cpp
    source/smb_browser.cpp
)

target_include_directories(switchbox_core
    PUBLIC
        include
)

target_compile_definitions(switchbox_core
    PUBLIC
        SWITCHBOX_APP_VERSION="${PROJECT_VERSION}"
)

if (WIN32 AND NOT SWITCHBOX_BUILD_SWITCH)
    target_link_libraries(switchbox_core PRIVATE Mpr)
endif ()

if (SWITCHBOX_BUILD_SWITCH)
    set(SWITCHBOX_SWITCH_PORTLIBS_INCLUDE_DIR "${DEVKITPRO}/portlibs/switch/include")
    set(SWITCHBOX_SWITCH_PORTLIBS_LIB_DIR "${DEVKITPRO}/portlibs/switch/lib")
    set(SWITCHBOX_SWITCH_LIBSMB2_LIBRARY "${SWITCHBOX_SWITCH_PORTLIBS_LIB_DIR}/libsmb2.a")

    if (NOT EXISTS "${SWITCHBOX_SWITCH_PORTLIBS_INCLUDE_DIR}/smb2/libsmb2.h")
        message(FATAL_ERROR "switch-libsmb2 headers not found under ${SWITCHBOX_SWITCH_PORTLIBS_INCLUDE_DIR}. Install or build libsmb2 for Switch first.")
    endif ()

    if (NOT EXISTS "${SWITCHBOX_SWITCH_LIBSMB2_LIBRARY}")
        message(FATAL_ERROR "switch-libsmb2 static library not found at ${SWITCHBOX_SWITCH_LIBSMB2_LIBRARY}. Install or build libsmb2 for Switch first.")
    endif ()

    target_include_directories(switchbox_core PRIVATE "${SWITCHBOX_SWITCH_PORTLIBS_INCLUDE_DIR}")
    target_link_libraries(switchbox_core PRIVATE "${SWITCHBOX_SWITCH_LIBSMB2_LIBRARY}")
endif ()

target_compile_features(switchbox_core PUBLIC cxx_std_20)

if (SWITCHBOX_ENABLE_WARNINGS)
    if (MSVC)
        target_compile_options(switchbox_core PRIVATE /W4)
    else ()
        target_compile_options(switchbox_core PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
endif ()
