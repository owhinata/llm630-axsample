cmake_minimum_required(VERSION 3.20)

add_library(ax_sys_cpp SHARED
    src/system.cc
    src/cmm.cc
)

target_include_directories(ax_sys_cpp
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
      ${CMAKE_SOURCE_DIR}/ax620e_bsp_sdk/msp/out/arm64_glibc/include
)

target_link_directories(ax_sys_cpp PRIVATE
    ${CMAKE_SOURCE_DIR}/ax620e_bsp_sdk/msp/out/arm64_glibc/lib
)

target_link_libraries(ax_sys_cpp PRIVATE ax_sys)

llm630_enable_contribution_checks(ax_sys_cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cmm.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/system.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/axsys/sys.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/axsys/system.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/axsys/cmm.hpp")
