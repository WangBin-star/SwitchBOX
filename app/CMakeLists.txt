add_library(switchbox_app STATIC
    source/application.cpp
    source/config_missing_activity.cpp
    source/header_status_hint.cpp
    source/home_activity.cpp
    source/placeholder_activity.cpp
    source/settings_activity.cpp
    source/smb_browser_activity.cpp
)

target_include_directories(switchbox_app
    PUBLIC
        include
)

target_link_libraries(switchbox_app
    PUBLIC
        borealis
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

    add_custom_target(switchbox_desktop_resources
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SWITCHBOX_RESOURCES_DIR}" "${CMAKE_BINARY_DIR}/resources"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/langs"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/langs" "${CMAKE_BINARY_DIR}/langs"
    )

    add_dependencies(switchbox_desktop switchbox_desktop_resources)
endif ()

if (SWITCHBOX_BUILD_SWITCH)
    add_executable(switchbox_switch
        source/main_switch.cpp
        "${CMAKE_SOURCE_DIR}/library/borealis/library/lib/platforms/switch/switch_wrapper.c"
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
    set(SWITCHBOX_SWITCH_ROMFS_DIR "${DEVKITPRO}/tmp/switchbox-switch-romfs")

    file(REMOVE_RECURSE "${SWITCHBOX_SWITCH_ROMFS_DIR}")
    file(MAKE_DIRECTORY "${SWITCHBOX_SWITCH_ROMFS_DIR}")
    file(COPY "${SWITCHBOX_RESOURCES_DIR}/" DESTINATION "${SWITCHBOX_SWITCH_ROMFS_DIR}")

    nx_generate_nacp(
        OUTPUT "${SWITCHBOX_NACP_PATH}"
        NAME "${SWITCHBOX_SWITCH_TITLE}"
        AUTHOR "${SWITCHBOX_AUTHOR}"
        VERSION "${PROJECT_VERSION}"
    )

    # elf2nro can fail on Windows workspaces with non-ASCII source paths.
    # Stage romfs assets under an ASCII-only path before packaging the NRO.
    add_custom_target(switchbox_switch_romfs_stage
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SWITCHBOX_SWITCH_ROMFS_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SWITCHBOX_SWITCH_ROMFS_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SWITCHBOX_RESOURCES_DIR}" "${SWITCHBOX_SWITCH_ROMFS_DIR}"
    )

    add_custom_target(switchbox_switch_runtime_assets
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/langs"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/langs" "${CMAKE_CURRENT_BINARY_DIR}/langs"
    )

    set(SWITCHBOX_RELEASE_ROOT "${SWITCHBOX_RELEASES_DIR}/${SWITCHBOX_RELEASE_VERSION}/switch")

    add_custom_target(switchbox_switch_package
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SWITCHBOX_RELEASE_ROOT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SWITCHBOX_RELEASE_ROOT}"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE_DIR:switchbox_switch>/switchbox_switch.nro" "${SWITCHBOX_RELEASE_ROOT}/SwitchBOX.nro"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/packaging/switch/switchbox.ini" "${SWITCHBOX_RELEASE_ROOT}/switchbox.ini"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/langs" "${SWITCHBOX_RELEASE_ROOT}/langs"
        COMMENT "Packaging Switch release to ${SWITCHBOX_RELEASE_ROOT}"
    )

    nx_create_nro(switchbox_switch
        NACP "${SWITCHBOX_NACP_PATH}"
        ROMFS "${SWITCHBOX_SWITCH_ROMFS_DIR}"
    )

    add_dependencies(switchbox_switch_nro switchbox_switch_romfs_stage)
    add_dependencies(switchbox_switch switchbox_switch_runtime_assets)
    add_dependencies(switchbox_switch_package switchbox_switch_nro switchbox_switch_runtime_assets)
endif ()
