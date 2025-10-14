# ----------------------------------------------------------------------------
# Toolchain bootstrap for the LLM630 sample project.
# This script downloads and configures the bundled ARM cross-compilation toolchain
# when the required compiler binaries are missing from the workspace.
# ----------------------------------------------------------------------------

set(LLM630_TOOLCHAIN_VERSION "gcc-arm-9.2-2019.12-x86_64-aarch64-none-linux-gnu")
set(LLM630_TOOLCHAIN_ROOT "${CMAKE_SOURCE_DIR}/toolchain")
set(LLM630_TOOLCHAIN_DIR "${LLM630_TOOLCHAIN_ROOT}/${LLM630_TOOLCHAIN_VERSION}")
set(LLM630_TOOLCHAIN_ARCHIVE "${LLM630_TOOLCHAIN_ROOT}/${LLM630_TOOLCHAIN_VERSION}.tar.xz")
set(LLM630_TOOLCHAIN_URL
    "https://developer.arm.com/-/media/Files/downloads/gnu-a/9.2-2019.12/binrel/${LLM630_TOOLCHAIN_VERSION}.tar.xz"
)

set(_llm630_toolchain_cxx "${LLM630_TOOLCHAIN_DIR}/bin/aarch64-none-linux-gnu-g++")
set(_llm630_toolchain_c "${LLM630_TOOLCHAIN_DIR}/bin/aarch64-none-linux-gnu-gcc")

set(_llm630_need_toolchain FALSE)
if(NOT EXISTS "${_llm630_toolchain_cxx}" OR NOT EXISTS "${_llm630_toolchain_c}")
    set(_llm630_need_toolchain TRUE)
endif()

if(_llm630_need_toolchain)
    file(MAKE_DIRECTORY "${LLM630_TOOLCHAIN_ROOT}")

    # Drop invalid cached archives so we always re-fetch a usable toolchain.
    if(EXISTS "${LLM630_TOOLCHAIN_ARCHIVE}")
        file(SIZE "${LLM630_TOOLCHAIN_ARCHIVE}" _llm630_archive_size)
        if(_llm630_archive_size LESS 1024)
            message(STATUS "Discarding invalid cached toolchain archive (${LLM630_TOOLCHAIN_ARCHIVE})")
            file(REMOVE "${LLM630_TOOLCHAIN_ARCHIVE}")
        endif()
    endif()

    if(NOT EXISTS "${LLM630_TOOLCHAIN_ARCHIVE}")
        message(STATUS "Downloading ARM GNU toolchain (${LLM630_TOOLCHAIN_VERSION}) ...")
        file(DOWNLOAD
            "${LLM630_TOOLCHAIN_URL}"
            "${LLM630_TOOLCHAIN_ARCHIVE}"
            SHOW_PROGRESS
            STATUS _llm630_download_status
            TLS_VERIFY ON
        )
        list(GET _llm630_download_status 0 _llm630_download_status_code)
        if(NOT _llm630_download_status_code EQUAL 0)
            message(FATAL_ERROR "Failed to download toolchain archive: ${_llm630_download_status}")
        endif()
    else()
        message(STATUS "Using cached toolchain archive: ${LLM630_TOOLCHAIN_ARCHIVE}")
    endif()

    file(SIZE "${LLM630_TOOLCHAIN_ARCHIVE}" _llm630_archive_size_after)
    if(_llm630_archive_size_after LESS 1024)
        file(REMOVE "${LLM630_TOOLCHAIN_ARCHIVE}")
        message(FATAL_ERROR "Downloaded toolchain archive is invalid (size ${_llm630_archive_size_after} bytes)")
    endif()

    if(EXISTS "${LLM630_TOOLCHAIN_DIR}")
        file(REMOVE_RECURSE "${LLM630_TOOLCHAIN_DIR}")
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

if(NOT EXISTS "${_llm630_toolchain_cxx}")
    message(FATAL_ERROR "Cross compiler not found at ${_llm630_toolchain_cxx}")
endif()
if(NOT EXISTS "${_llm630_toolchain_c}")
    message(FATAL_ERROR "Cross compiler not found at ${_llm630_toolchain_c}")
endif()

set(LLM630_TOOLCHAIN_BIN "${LLM630_TOOLCHAIN_DIR}/bin")
set(LLM630_C_COMPILER "${_llm630_toolchain_c}")
set(LLM630_CXX_COMPILER "${_llm630_toolchain_cxx}")

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
