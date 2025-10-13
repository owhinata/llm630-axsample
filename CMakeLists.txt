cmake_minimum_required(VERSION 3.20)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ToolchainBootstrap.cmake")

# ------------------------------------------------------------------------------
# Project configuration
# ------------------------------------------------------------------------------
project(llm630_axsample LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Strict warnings for C++ builds
add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-Werror>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall>
    $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
    $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
    $<$<COMPILE_LANGUAGE:CXX>:-Wconversion>
    $<$<COMPILE_LANGUAGE:CXX>:-Wsign-conversion>
    $<$<COMPILE_LANGUAGE:CXX>:-Wshadow>
    $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
    $<$<COMPILE_LANGUAGE:CXX>:-Wuseless-cast>
    $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
    $<$<COMPILE_LANGUAGE:CXX>:-Wformat=2>
    $<$<COMPILE_LANGUAGE:CXX>:-Wnull-dereference>
    $<$<COMPILE_LANGUAGE:CXX>:-Wlogical-op>
    $<$<COMPILE_LANGUAGE:CXX>:-Wduplicated-cond>
    $<$<COMPILE_LANGUAGE:CXX>:-Wduplicated-branches>
    $<$<COMPILE_LANGUAGE:CXX>:-Wsuggest-override>
    $<$<COMPILE_LANGUAGE:CXX>:-Wunused>
    $<$<COMPILE_LANGUAGE:CXX>:-Wunused-variable>
    $<$<COMPILE_LANGUAGE:CXX>:-Wunused-parameter>
    $<$<COMPILE_LANGUAGE:CXX>:-Wunused-but-set-variable>
    $<$<COMPILE_LANGUAGE:CXX>:-Wunused-but-set-parameter>
)

set(VALID_BUILD_TYPES Debug Release RelWithDebInfo MinSizeRel Contribution)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
string(TOUPPER "${CMAKE_BUILD_TYPE}" _upper_build_type)
if(_upper_build_type STREQUAL "CONTRIBUTION")
    set(CMAKE_BUILD_TYPE "Contribution" CACHE STRING "Build type" FORCE)
elseif(_upper_build_type STREQUAL "DEBUG")
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
elseif(_upper_build_type STREQUAL "RELEASE")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
elseif(_upper_build_type STREQUAL "RELWITHDEBINFO")
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
elseif(_upper_build_type STREQUAL "MINSIZEREL")
    set(CMAKE_BUILD_TYPE "MinSizeRel" CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${VALID_BUILD_TYPES})

find_program(CLANG_FORMAT_EXE NAMES clang-format)
if(NOT CLANG_FORMAT_EXE)
    message(WARNING "clang-format not found; C++ sources will not be auto-formatted.")
endif()

find_program(CPPLINT_EXE NAMES cpplint)
if(NOT CPPLINT_EXE)
    message(WARNING "cpplint not found; C++ sources will not be linted after build.")
endif()

set(AXERA_SUBMODULE_DIR "${CMAKE_SOURCE_DIR}/ax620e_bsp_sdk")
if(NOT EXISTS "${AXERA_SUBMODULE_DIR}/README.md")
    message(STATUS "ax620e_bsp_sdk submodule missing; updating...")
    execute_process(
        COMMAND git submodule update --init --recursive ax620e_bsp_sdk
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _axera_submodule_status
    )
    if(NOT _axera_submodule_status EQUAL 0)
        message(FATAL_ERROR "Failed to initialise ax620e_bsp_sdk submodule (exit code ${_axera_submodule_status}).")
    endif()
endif()

add_subdirectory(cpp)

set(LLM630_DEPLOY_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/llm630_deploy.cmake")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployToDevice.cmake.in"
    "${LLM630_DEPLOY_SCRIPT}"
    @ONLY
)

install(SCRIPT "${LLM630_DEPLOY_SCRIPT}")
