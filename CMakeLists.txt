cmake_minimum_required(VERSION 3.20)

# ------------------------------------------------------------------------------
# Toolchain bootstrap
# ------------------------------------------------------------------------------
set(LLM630_TOOLCHAIN_VERSION "gcc-arm-9.2-2019.12-x86_64-aarch64-none-linux-gnu")
set(LLM630_TOOLCHAIN_ROOT "${CMAKE_SOURCE_DIR}/toolchain")
set(LLM630_TOOLCHAIN_DIR "${LLM630_TOOLCHAIN_ROOT}/${LLM630_TOOLCHAIN_VERSION}")
set(LLM630_TOOLCHAIN_ARCHIVE "${LLM630_TOOLCHAIN_ROOT}/${LLM630_TOOLCHAIN_VERSION}.tar.xz")
set(LLM630_TOOLCHAIN_URL
    "https://developer.arm.com/-/media/Files/downloads/gnu-a/9.2-2019.12/binrel/${LLM630_TOOLCHAIN_VERSION}.tar.xz"
)

if(NOT EXISTS "${LLM630_TOOLCHAIN_DIR}/bin/aarch64-none-linux-gnu-g++")
    file(MAKE_DIRECTORY "${LLM630_TOOLCHAIN_ROOT}")

    if(NOT EXISTS "${LLM630_TOOLCHAIN_ARCHIVE}")
        message(STATUS "Downloading ARM GNU toolchain (${LLM630_TOOLCHAIN_VERSION}) ...")
        file(DOWNLOAD
            "${LLM630_TOOLCHAIN_URL}"
            "${LLM630_TOOLCHAIN_ARCHIVE}"
            SHOW_PROGRESS
        )
    else()
        message(STATUS "Using cached toolchain archive: ${LLM630_TOOLCHAIN_ARCHIVE}")
    endif()

    message(STATUS "Extracting toolchain to ${LLM630_TOOLCHAIN_ROOT}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xvf "${LLM630_TOOLCHAIN_ARCHIVE}"
        WORKING_DIRECTORY "${LLM630_TOOLCHAIN_ROOT}"
        RESULT_VARIABLE _toolchain_extract_result
    )

    if(NOT _toolchain_extract_result EQUAL 0)
        message(FATAL_ERROR "Failed to extract toolchain archive (exit code ${_toolchain_extract_result})")
    endif()
endif()

set(LLM630_TOOLCHAIN_BIN "${LLM630_TOOLCHAIN_DIR}/bin")
set(LLM630_C_COMPILER "${LLM630_TOOLCHAIN_BIN}/aarch64-none-linux-gnu-gcc")
set(LLM630_CXX_COMPILER "${LLM630_TOOLCHAIN_BIN}/aarch64-none-linux-gnu-g++")

if(NOT EXISTS "${LLM630_C_COMPILER}")
    message(FATAL_ERROR "Cross compiler not found at ${LLM630_C_COMPILER}")
endif()
if(NOT EXISTS "${LLM630_CXX_COMPILER}")
    message(FATAL_ERROR "Cross compiler not found at ${LLM630_CXX_COMPILER}")
endif()

if(NOT DEFINED CMAKE_SYSTEM_NAME)
    set(CMAKE_SYSTEM_NAME "Linux")
endif()
if(NOT DEFINED CMAKE_SYSTEM_PROCESSOR)
    set(CMAKE_SYSTEM_PROCESSOR "aarch64")
endif()

if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "${LLM630_C_COMPILER}" CACHE FILEPATH "LLM630 AArch64 GCC" FORCE)
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "${LLM630_CXX_COMPILER}" CACHE FILEPATH "LLM630 AArch64 G++" FORCE)
endif()

set(CMAKE_FIND_ROOT_PATH
    "${LLM630_TOOLCHAIN_DIR}/aarch64-none-linux-gnu"
    CACHE STRING "Find root paths for cross compilation" FORCE
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# ------------------------------------------------------------------------------
# Project configuration
# ------------------------------------------------------------------------------
project(llm630_axsample LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
