set(DEVKITPRO_RAW "$ENV{DEVKITPRO}")
set(DEVKITA64_RAW "$ENV{DEVKITA64}")

string(REPLACE "\\" "/" DEVKITPRO "${DEVKITPRO_RAW}")
string(REPLACE "\\" "/" DEVKITA64 "${DEVKITA64_RAW}")

# Keep these as plain strings. On Windows, CACHE PATH can be rewritten back to
# backslashes during regeneration, which breaks the official devkitPro CMake
# toolchain because the resulting list items contain invalid escape sequences.
set(DEVKITPRO "${DEVKITPRO}" CACHE STRING "Path to devkitPro" FORCE)
set(DEVKITA64 "${DEVKITA64}" CACHE STRING "Path to devkitA64" FORCE)
set(ENV{DEVKITPRO} "${DEVKITPRO}")
set(ENV{DEVKITA64} "${DEVKITA64}")

if (NOT DEVKITPRO)
    message(FATAL_ERROR "DEVKITPRO is not set. Expected a path such as C:/devkitPro.")
endif ()

if (NOT DEVKITA64)
    message(FATAL_ERROR "DEVKITA64 is not set. Expected a path such as C:/devkitPro/devkitA64.")
endif ()

if (NOT EXISTS "${DEVKITPRO}/cmake/Switch.cmake")
    message(FATAL_ERROR "Official devkitPro Switch.cmake not found under ${DEVKITPRO}/cmake.")
endif ()

include("${DEVKITPRO}/cmake/Switch.cmake")

set(SWITCHBOX_SWITCH TRUE CACHE BOOL "Building for Nintendo Switch")

if (WIN32)
    # GCC-generated depfiles can break under CMake Makefiles when the workspace path
    # contains Windows drive prefixes or non-ASCII characters. Fall back to CMake's
    # dependency scanner for stable builds on this host.
    set(CMAKE_DEPENDS_USE_COMPILER FALSE)
endif ()
