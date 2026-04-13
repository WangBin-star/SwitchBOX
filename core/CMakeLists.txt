add_library(switchbox_core STATIC
    source/build_info.cpp
)

target_include_directories(switchbox_core
    PUBLIC
        include
)

target_compile_features(switchbox_core PUBLIC cxx_std_20)

if (SWITCHBOX_ENABLE_WARNINGS)
    if (MSVC)
        target_compile_options(switchbox_core PRIVATE /W4)
    else ()
        target_compile_options(switchbox_core PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
endif ()
