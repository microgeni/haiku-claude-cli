# CMake toolchain file for cross-compiling to Haiku/arm64 (aarch64).
#
# The cross-tools GCC already has its sysroot compiled in, so we deliberately
# do NOT set CMAKE_SYSROOT — doing so would override the built-in header
# search paths (Haiku keeps them under develop/headers, not /usr/include)
# and the compiler would stop finding its own libstdc++.
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=/path/to/haiku-arm64-toolchain.cmake ...

set(CMAKE_SYSTEM_NAME Haiku)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED CROSS_TOOLS)
	set(CROSS_TOOLS "/Data/Code/Repos/haiku/generated.arm64/cross-tools-arm64")
endif()

set(CMAKE_C_COMPILER   "${CROSS_TOOLS}/bin/aarch64-unknown-haiku-gcc")
set(CMAKE_CXX_COMPILER "${CROSS_TOOLS}/bin/aarch64-unknown-haiku-g++")
set(CMAKE_AR           "${CROSS_TOOLS}/bin/aarch64-unknown-haiku-ar")
set(CMAKE_RANLIB       "${CROSS_TOOLS}/bin/aarch64-unknown-haiku-ranlib")

# Look for headers and libraries in the target sysroot only; run programs
# from the host.
set(CMAKE_FIND_ROOT_PATH "${CROSS_TOOLS}/sysroot/boot/system")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
