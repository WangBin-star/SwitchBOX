add_library(switchbox_platform_switch STATIC
    source/platform_switch.cpp
)

target_include_directories(switchbox_platform_switch
    PUBLIC
        include
)

target_compile_features(switchbox_platform_switch PUBLIC cxx_std_20)

if (SWITCHBOX_ENABLE_WARNINGS)
    if (MSVC)
        target_compile_options(switchbox_platform_switch PRIVATE /W4)
    else ()
        target_compile_options(switchbox_platform_switch PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
endif ()
