# Cross-compile for Raspberry Pi 4/5 (64-bit OS).
# Usage:
#   cmake -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake \
#         -DPI_SYSROOT=$HOME/pi-sysroot -DGUARDEYE_BUILD_TESTS=OFF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
if(NOT PI_SYSROOT)
  message(FATAL_ERROR "Pass -DPI_SYSROOT=<path to rsync'd Pi root filesystem>")
endif()
set(CMAKE_SYSROOT ${PI_SYSROOT})
set(CMAKE_FIND_ROOT_PATH ${PI_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_SYSROOT_DIR} ${PI_SYSROOT})
set(ENV{PKG_CONFIG_LIBDIR} ${PI_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${PI_SYSROOT}/usr/lib/pkgconfig)
