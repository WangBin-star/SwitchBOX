add_library(switchbox_platform_desktop STATIC
    source/platform_desktop.cpp
)

target_include_directories(switchbox_platform_desktop
    PUBLIC
        include
)

target_compile_features(switchbox_platform_desktop PUBLIC cxx_std_20)

if (SWITCHBOX_ENABLE_WARNINGS)
    if (MSVC)
        target_compile_options(switchbox_platform_desktop PRIVATE /W4)
    else ()
        target_compile_options(switchbox_platform_desktop PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
endif ()
