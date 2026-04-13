add_library(switchbox_app STATIC
    source/application.cpp
)

target_include_directories(switchbox_app
    PUBLIC
        include
)

target_link_libraries(switchbox_app
    PUBLIC
        switchbox_core
)

target_compile_features(switchbox_app PUBLIC cxx_std_20)

if (SWITCHBOX_ENABLE_WARNINGS)
    if (MSVC)
        target_compile_options(switchbox_app PRIVATE /W4)
    else ()
        target_compile_options(switchbox_app PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
endif ()

if (SWITCHBOX_BUILD_DESKTOP)
    add_executable(switchbox_desktop
        source/main_desktop.cpp
    )

    target_link_libraries(switchbox_desktop
        PRIVATE
            switchbox_app
            switchbox_core
            switchbox_platform_desktop
    )

    target_include_directories(switchbox_desktop
        PRIVATE
            include
    )

    target_compile_features(switchbox_desktop PRIVATE cxx_std_20)

    if (SWITCHBOX_ENABLE_WARNINGS)
        if (MSVC)
            target_compile_options(switchbox_desktop PRIVATE /W4)
        else ()
            target_compile_options(switchbox_desktop PRIVATE -Wall -Wextra -Wpedantic)
        endif ()
    endif ()
endif ()

if (SWITCHBOX_BUILD_SWITCH)
    add_executable(switchbox_switch
        source/main_switch.cpp
    )

    target_link_libraries(switchbox_switch
        PRIVATE
            switchbox_app
            switchbox_core
            switchbox_platform_switch
    )

    target_include_directories(switchbox_switch
        PRIVATE
            include
    )

    target_compile_features(switchbox_switch PRIVATE cxx_std_20)

    if (SWITCHBOX_ENABLE_WARNINGS)
        if (MSVC)
            target_compile_options(switchbox_switch PRIVATE /W4)
        else ()
            target_compile_options(switchbox_switch PRIVATE -Wall -Wextra -Wpedantic)
        endif ()
    endif ()

    set(SWITCHBOX_NACP_PATH "${CMAKE_CURRENT_BINARY_DIR}/switchbox_switch.nacp")
    nx_generate_nacp(
        OUTPUT "${SWITCHBOX_NACP_PATH}"
        NAME "${SWITCHBOX_SWITCH_TITLE}"
        AUTHOR "${SWITCHBOX_AUTHOR}"
        VERSION "${PROJECT_VERSION}"
    )

    nx_create_nro(switchbox_switch
        NACP "${SWITCHBOX_NACP_PATH}"
    )
endif ()
