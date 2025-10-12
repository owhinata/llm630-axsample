cmake_minimum_required(VERSION 3.20)

add_executable(sample_pool
    src/sample_pool.cc
)

target_include_directories(sample_pool PRIVATE
    ${CMAKE_SOURCE_DIR}/ax620e_bsp_sdk/msp/out/arm64_glibc/include
)

target_link_directories(sample_pool PRIVATE
    ${CMAKE_SOURCE_DIR}/ax620e_bsp_sdk/msp/out/arm64_glibc/lib
)

target_link_libraries(sample_pool PRIVATE ax_sys)

llm630_enable_contribution_checks(sample_pool "${CMAKE_CURRENT_SOURCE_DIR}/src/sample_pool.cc")

