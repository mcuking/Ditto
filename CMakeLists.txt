cmake_minimum_required(VERSION 3.17)
project(Ditto)

set(CMAKE_C_STANDARD 99)

set(SOURCES_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

set(SOURCES
        ${SOURCES_ROOT}/cli/cli.c
        ${SOURCES_ROOT}/lexer/lexer.c
        ${SOURCES_ROOT}/compiler/compiler.c
        ${SOURCES_ROOT}/vm/vm.c
        ${SOURCES_ROOT}/vm/core.c
        ${SOURCES_ROOT}/object/class.c
        ${SOURCES_ROOT}/object/header_obj.c
        ${SOURCES_ROOT}/object/meta_obj.c
        ${SOURCES_ROOT}/object/obj_fn.c
        ${SOURCES_ROOT}/object/obj_list.c
        ${SOURCES_ROOT}/object/obj_map.c
        ${SOURCES_ROOT}/object/obj_range.c
        ${SOURCES_ROOT}/object/obj_string.c
        ${SOURCES_ROOT}/object/obj_thread.c
        ${SOURCES_ROOT}/include/unicodeUtf8.c
        ${SOURCES_ROOT}/include/utils.c
        ${SOURCES_ROOT}/gc/gc.c
        )

include_directories(
        "${SOURCES_ROOT}/cli"
        "${SOURCES_ROOT}/compiler"
        "${SOURCES_ROOT}/lexer"
        "${SOURCES_ROOT}/object"
        "${SOURCES_ROOT}/vm"
        "${SOURCES_ROOT}/include"
        "${SOURCES_ROOT}/gc"
)

add_executable(di ${SOURCES})