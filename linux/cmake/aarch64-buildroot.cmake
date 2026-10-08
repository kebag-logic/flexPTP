# CMake toolchain file: a Buildroot aarch64 toolchain (glibc), such as the
# PocketBeagle 2's. Its compiler wrapper has the sysroot built in.
#
#   cmake -S linux -B build-aarch64 \
#         -DCMAKE_TOOLCHAIN_FILE=linux/cmake/aarch64-buildroot.cmake \
#         -DBR_HOST=<buildroot output>/host

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# the Buildroot output's host directory
if(NOT BR_HOST)
    set(BR_HOST $ENV{BR_HOST})
endif()

if(NOT BR_HOST)
    set(BR_HOST /home/alex/prjs-avb-on-fpga/srcs/buildroot-pb2/host)
endif()

set(CMAKE_C_COMPILER ${BR_HOST}/bin/aarch64-buildroot-linux-gnu-gcc)
set(CMAKE_SYSROOT ${BR_HOST}/aarch64-buildroot-linux-gnu/sysroot)

# libraries and headers from the sysroot only; programs from the host
set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
