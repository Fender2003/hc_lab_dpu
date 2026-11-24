cmake_minimum_required(VERSION 3.22)

project(hwlab-03-udp-echo)

add_compile_options(-Wall -Wextra -fstack-protector-all -fsanitize=address)
add_link_options(-fsanitize=address)

find_package(PkgConfig REQUIRED)
pkg_check_modules(
    DOCA
    REQUIRED
    doca-common
    doca-compress
    doca-eth
    doca-flow
)

add_definitions(${DOCA_FLAGS} -DDOCA_ALLOW_EXPERIMENTAL_API)
include_directories(. ${DOCA_INCLUDE_DIRS})
link_directories(${DOCA_LIBRARY_DIRS})

add_executable(03-udp-echo main.c util.c)
target_link_libraries(03-udp-echo PRIVATE ${DOCA_LIBRARIES})
