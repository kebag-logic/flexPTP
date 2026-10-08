# flexptpd: flexPTP as a Linux daemon, on the kernel's PHC and hardware
# timestamps (or software timestamps and a virtual clock, with -s).
#
#   cmake -S linux -B build && cmake --build build
#
# Cross build for the PocketBeagle 2 (the Buildroot toolchain):
#
#   cmake -S linux -B build-aarch64 -DCMAKE_TOOLCHAIN_FILE=linux/cmake/aarch64-buildroot.cmake

cmake_minimum_required(VERSION 3.18)

project(flexptpd C)

set(CMAKE_C_STANDARD 11)

set(DEFS "_GNU_SOURCE=1")

# the flexPTP library, built for Linux, from the tree above
set(FLEXPTP_COMPILE_DEFS ${DEFS})
set(FLEXPTP_INCLUDES ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/../src)
set(FLEXPTP_HWPORT LINUX)
set(FLEXPTP_NSD LINUX)
set(FLEXPTP_SERVO KALMAN)

add_subdirectory(.. flexptp)

# the platform files are kept warning-free
set(FLEXPTP_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/../src/flexptp)

set_source_files_properties(
    ${FLEXPTP_SRC_DIR}/port/example_netstack_drivers/nsd_linux.c
    ${FLEXPTP_SRC_DIR}/port/example_netstack_drivers/nsd_linux_txts.c
    DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra"
)

# ---- the daemon ----

add_executable(flexptpd
    flexptpd.c
    flexptpd_print.c
    flexptpd_print.h
    flexptpd_shm.h
    flexptp_options.h
    cli/cli.c
    cli/cli.h
    cli/term_colors.h
)

target_compile_definitions(flexptpd PUBLIC ${DEFS})
target_compile_options(flexptpd PRIVATE -Wall -Wextra)
target_link_libraries(flexptpd flexptp pthread rt)
target_include_directories(flexptpd PUBLIC ${FLEXPTP_INCLUDE_EXPORT} ${FLEXPTP_INCLUDES})

# ---- the status block reader ----

add_executable(flexptpd-status
    flexptpd_status.c
    flexptpd_shm.h
)

target_compile_definitions(flexptpd-status PUBLIC ${DEFS})
target_compile_options(flexptpd-status PRIVATE -Wall -Wextra)
target_link_libraries(flexptpd-status rt)
target_include_directories(flexptpd-status PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../src)

install(TARGETS flexptpd flexptpd-status RUNTIME DESTINATION bin)
