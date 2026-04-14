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

target_compile_features(switchbox_core PUBLIC cxx_std_20)

if (SWITCHBOX_ENABLE_WARNINGS)
    if (MSVC)
        target_compile_options(switchbox_core PRIVATE /W4)
    else ()
        target_compile_options(switchbox_core PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
endif ()
