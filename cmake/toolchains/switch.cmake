set(DEVKITPRO_RAW "$ENV{DEVKITPRO}")
set(DEVKITA64_RAW "$ENV{DEVKITA64}")

string(REPLACE "\\" "/" DEVKITPRO "${DEVKITPRO_RAW}")
string(REPLACE "\\" "/" DEVKITA64 "${DEVKITA64_RAW}")

set(DEVKITPRO "${DEVKITPRO}" CACHE PATH "Path to devkitPro" FORCE)
set(DEVKITA64 "${DEVKITA64}" CACHE PATH "Path to devkitA64" FORCE)

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
