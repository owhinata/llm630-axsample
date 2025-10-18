cmake_minimum_required(VERSION 3.20)
find_package(Threads REQUIRED)

set(TEST_SOURCES
    src/test_main.cc
    src/test_cmm_basic.cc
    src/test_cmm_cache_ops.cc
    src/test_cmm_dump_verify.cc
    src/test_cmm_fast.cc
    src/test_cmm_external.cc
    src/test_cmm_memquery.cc
    src/test_cmm_map_variants.cc
    src/test_cmm_scaling.cc
    src/test_cmm_pool.cc
)

add_executable(test_libax_sys_cpp ${TEST_SOURCES})

target_include_directories(test_libax_sys_cpp PRIVATE
    ${CMAKE_SOURCE_DIR}/ax620e_bsp_sdk/msp/out/arm64_glibc/include
)

target_link_directories(test_libax_sys_cpp PRIVATE
    ${CMAKE_SOURCE_DIR}/ax620e_bsp_sdk/msp/out/arm64_glibc/lib
)

target_link_libraries(test_libax_sys_cpp PRIVATE
    gtest
    gtest_main
    Threads::Threads
    ax_sys
    ax_sys_cpp
)

# Ensure googletest external project is built before the tests link
add_dependencies(test_libax_sys_cpp googletest)

# Use absolute paths for format/lint hooks (build dir working directory)
set(TEST_SOURCES_ABS ${TEST_SOURCES})
list(TRANSFORM TEST_SOURCES_ABS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
llm630_enable_contribution_checks(test_libax_sys_cpp ${TEST_SOURCES_ABS})
