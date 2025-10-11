add_executable(hello_world
    src/main.cpp
)

if(CMAKE_BUILD_TYPE STREQUAL "Contribution" AND CLANG_FORMAT_EXE)
    add_custom_command(
        TARGET hello_world
        PRE_BUILD
        COMMAND ${CLANG_FORMAT_EXE} --style=google -i "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
        COMMENT "Formatting hello_world sources with clang-format (Google style)"
        VERBATIM
    )
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Contribution" AND CPPLINT_EXE)
    add_custom_command(
        TARGET hello_world
        POST_BUILD
        COMMAND ${CPPLINT_EXE} --filter=-legal/copyright "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
        COMMENT "Linting hello_world sources with cpplint"
        VERBATIM
    )
endif()
