add_library(switchbox_core STATIC
    source/app_config.cpp
    source/build_info.cpp
    source/language.cpp
    source/playback_launcher.cpp
    source/playback_target.cpp
    source/smb2_mount_fs.cpp
    source/switch_mpv_player.cpp
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

set(SWITCHBOX_SWITCH_PORTLIBS_INCLUDE_DIR "${DEVKITPRO}/portlibs/switch/include")
set(SWITCHBOX_SWITCH_PORTLIBS_LIB_DIR "${DEVKITPRO}/portlibs/switch/lib")
set(SWITCHBOX_SWITCH_LIBSMB2_LIBRARY "${SWITCHBOX_SWITCH_PORTLIBS_LIB_DIR}/libsmb2.a")

set(SWITCHBOX_SWITCH_MEDIA_PORTLIBS_PREFIX "${DEVKITPRO}/tmp/switchbox-portlibs" CACHE PATH "Custom Switch media portlibs prefix for libmpv/FFmpeg")
if (EXISTS "/opt/devkitpro/tmp/switchbox-portlibs/include/mpv/render_dk3d.h")
    set(SWITCHBOX_SWITCH_MEDIA_PORTLIBS_PREFIX "/opt/devkitpro/tmp/switchbox-portlibs" CACHE PATH "" FORCE)
endif ()

set(SWITCHBOX_SWITCH_MEDIA_INCLUDE_DIR "${SWITCHBOX_SWITCH_MEDIA_PORTLIBS_PREFIX}/include")
set(SWITCHBOX_SWITCH_MEDIA_LIB_DIR "${SWITCHBOX_SWITCH_MEDIA_PORTLIBS_PREFIX}/lib")
set(SWITCHBOX_SWITCH_LIBMPV_LIBRARY "${SWITCHBOX_SWITCH_MEDIA_LIB_DIR}/libmpv.a")
set(SWITCHBOX_SWITCH_LIBUAM_LIBRARY "${SWITCHBOX_SWITCH_MEDIA_LIB_DIR}/libuam.a")
set(SWITCHBOX_SWITCH_AVFILTER_LIBRARY "${SWITCHBOX_SWITCH_MEDIA_LIB_DIR}/libavfilter.a")
set(SWITCHBOX_SWITCH_SWSCALE_LIBRARY "${SWITCHBOX_SWITCH_MEDIA_LIB_DIR}/libswscale.a")
set(SWITCHBOX_SWITCH_POSTPROC_LIBRARY "${SWITCHBOX_SWITCH_MEDIA_LIB_DIR}/libpostproc.a")
set(SWITCHBOX_SWITCH_AVFORMAT_LIBRARY "${SWITCHBOX_SWITCH_MEDIA_LIB_DIR}/libavformat.a")
set(SWITCHBOX_SWITCH_AVCODEC_LIBRARY "${SWITCHBOX_SWITCH_MEDIA_LIB_DIR}/libavcodec.a")
set(SWITCHBOX_SWITCH_SWRESAMPLE_LIBRARY "${SWITCHBOX_SWITCH_MEDIA_LIB_DIR}/libswresample.a")
set(SWITCHBOX_SWITCH_AVUTIL_LIBRARY "${SWITCHBOX_SWITCH_MEDIA_LIB_DIR}/libavutil.a")

if (NOT EXISTS "${SWITCHBOX_SWITCH_PORTLIBS_INCLUDE_DIR}/smb2/libsmb2.h")
    message(FATAL_ERROR "switch-libsmb2 headers not found under ${SWITCHBOX_SWITCH_PORTLIBS_INCLUDE_DIR}. Install or build libsmb2 for Switch first.")
endif ()

if (NOT EXISTS "${SWITCHBOX_SWITCH_LIBSMB2_LIBRARY}")
    message(FATAL_ERROR "switch-libsmb2 static library not found at ${SWITCHBOX_SWITCH_LIBSMB2_LIBRARY}. Install or build libsmb2 for Switch first.")
endif ()

target_include_directories(switchbox_core PRIVATE "${SWITCHBOX_SWITCH_MEDIA_INCLUDE_DIR}" "${SWITCHBOX_SWITCH_PORTLIBS_INCLUDE_DIR}")
target_link_libraries(switchbox_core PRIVATE "${SWITCHBOX_SWITCH_LIBSMB2_LIBRARY}")

if (
    EXISTS "${SWITCHBOX_SWITCH_MEDIA_INCLUDE_DIR}/mpv/client.h" AND
    EXISTS "${SWITCHBOX_SWITCH_MEDIA_INCLUDE_DIR}/mpv/render_dk3d.h" AND
    EXISTS "${SWITCHBOX_SWITCH_LIBMPV_LIBRARY}" AND
    EXISTS "${SWITCHBOX_SWITCH_LIBUAM_LIBRARY}" AND
    EXISTS "${SWITCHBOX_SWITCH_AVFILTER_LIBRARY}" AND
    EXISTS "${SWITCHBOX_SWITCH_AVFORMAT_LIBRARY}" AND
    EXISTS "${SWITCHBOX_SWITCH_AVCODEC_LIBRARY}" AND
    EXISTS "${SWITCHBOX_SWITCH_AVUTIL_LIBRARY}"
)
    message(STATUS "Using custom Switch media portlibs from ${SWITCHBOX_SWITCH_MEDIA_PORTLIBS_PREFIX}")
    target_compile_definitions(switchbox_core PRIVATE SWITCHBOX_HAS_SWITCH_MPV=1)
    target_link_libraries(switchbox_core
        PRIVATE
            "${SWITCHBOX_SWITCH_LIBMPV_LIBRARY}"
            deko3d
            "${SWITCHBOX_SWITCH_LIBUAM_LIBRARY}"
            "${SWITCHBOX_SWITCH_AVFILTER_LIBRARY}"
            "${SWITCHBOX_SWITCH_SWSCALE_LIBRARY}"
            "${SWITCHBOX_SWITCH_POSTPROC_LIBRARY}"
            "${SWITCHBOX_SWITCH_AVFORMAT_LIBRARY}"
            mbedtls
            mbedx509
            mbedcrypto
            "${SWITCHBOX_SWITCH_AVCODEC_LIBRARY}"
            dav1d
            "${SWITCHBOX_SWITCH_SWRESAMPLE_LIBRARY}"
            "${SWITCHBOX_SWITCH_AVUTIL_LIBRARY}"
            ass
            harfbuzz
            fribidi
            freetype
            bz2
            png16
            nx
            m
            z
    )
else ()
    message(WARNING "Custom switch libmpv/deko3d toolchain not found under ${SWITCHBOX_SWITCH_MEDIA_PORTLIBS_PREFIX}. Switch playback backend will remain disabled.")
endif ()

target_compile_features(switchbox_core PUBLIC cxx_std_20)

if (SWITCHBOX_ENABLE_WARNINGS)
    if (MSVC)
        target_compile_options(switchbox_core PRIVATE /W4)
    else ()
        target_compile_options(switchbox_core PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
endif ()
