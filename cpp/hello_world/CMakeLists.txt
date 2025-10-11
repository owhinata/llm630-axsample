add_executable(hello_world
    src/main.cpp
)

llm630_enable_contribution_checks(hello_world "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
