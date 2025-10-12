cmake_minimum_required(VERSION 3.20)

add_executable(sample_cmm
    src/sample_cmm.cc
)

target_include_directories(sample_cmm PRIVATE
    ${CMAKE_SOURCE_DIR}/ax620e_bsp_sdk/msp/out/arm64_glibc/include
)

target_link_directories(sample_cmm PRIVATE
    ${CMAKE_SOURCE_DIR}/ax620e_bsp_sdk/msp/out/arm64_glibc/lib
)

target_link_libraries(sample_cmm PRIVATE ax_sys ax_sys_cpp)

llm630_enable_contribution_checks(sample_cmm "${CMAKE_CURRENT_SOURCE_DIR}/src/sample_cmm.cc")
