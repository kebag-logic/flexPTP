# flexptpd: flexPTP as a Linux daemon, on the kernel's PHC and hardware timestamps.
#
#   cmake -S linux -B build && cmake --build build

cmake_minimum_required(VERSION 3.15)

project(flexptpd C)

set(CMAKE_C_STANDARD 11)

add_executable(flexptpd
    flexptpd.c
    flexptp_options.h
)

set(DEFS "_GNU_SOURCE=1")
target_compile_definitions(flexptpd PUBLIC ${DEFS})
target_compile_options(flexptpd PRIVATE -Wall -Wextra)

# the flexPTP library, built for Linux, from the tree above
set(FLEXPTP_COMPILE_DEFS ${DEFS})
set(FLEXPTP_INCLUDES ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/../src)
set(FLEXPTP_HWPORT LINUX)
set(FLEXPTP_NSD LINUX)
set(FLEXPTP_SERVO KALMAN)

add_subdirectory(.. flexptp)

target_link_libraries(flexptpd flexptp pthread rt)
target_include_directories(flexptpd PUBLIC ${FLEXPTP_INCLUDE_EXPORT} ${FLEXPTP_INCLUDES})
