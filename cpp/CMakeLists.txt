function(llm630_enable_contribution_checks target)
    if(NOT ARGN)
        message(FATAL_ERROR "llm630_enable_contribution_checks requires at least one source file")
    endif()

    if(NOT CMAKE_BUILD_TYPE STREQUAL "Contribution")
        return()
    endif()

    if(CLANG_FORMAT_EXE)
        add_custom_command(
            TARGET ${target}
            PRE_BUILD
            COMMAND ${CLANG_FORMAT_EXE} --style=google -i ${ARGN}
            COMMENT "Formatting ${target} sources with clang-format (Google style)"
            VERBATIM
        )
    endif()

    if(CPPLINT_EXE)
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND ${CPPLINT_EXE} --filter=-legal/copyright ${ARGN}
            COMMENT "Linting ${target} sources with cpplint"
            VERBATIM
        )
    endif()
endfunction()

add_subdirectory(hello_world)
add_subdirectory(sample_sysmap)
add_subdirectory(sample_sysmap_ax)
add_subdirectory(sample_pool)
add_subdirectory(sample_cmm)
add_subdirectory(libax_sys_cpp)
