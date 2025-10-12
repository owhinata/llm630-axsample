cmake_minimum_required(VERSION 3.20)

add_executable(sample_sysmap
    src/sample_sysmap.cc
)

target_include_directories(sample_sysmap PRIVATE
    ${CMAKE_SOURCE_DIR}/ax620e_bsp_sdk/msp/out/arm64_glibc/include
)

target_link_directories(sample_sysmap PRIVATE
    ${CMAKE_SOURCE_DIR}/ax620e_bsp_sdk/msp/out/arm64_glibc/lib
)

target_link_libraries(sample_sysmap PRIVATE ax_sys)

llm630_enable_contribution_checks(sample_sysmap "${CMAKE_CURRENT_SOURCE_DIR}/src/sample_sysmap.cc")

