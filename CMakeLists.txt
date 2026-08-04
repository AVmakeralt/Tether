# Tether — CMake build
#
# v0.1: lexer + parser + AST. No external dependencies. C++17.

cmake_minimum_required(VERSION 3.16)

project(tether
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release if unspecified.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Hard warnings — Tether's safety story starts with the implementation
# being clean.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter   # visitor pattern leaves unused params
        -Werror=return-type
    )
elseif(MSVC)
    add_compile_options(/W4 /permissive-)
endif()

# Output directories.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---- tetherc executable --------------------------------------------------

add_executable(tetherc
    src/main.cpp

    src/support/arena.cpp
    src/support/intern.cpp
    src/support/source.cpp

    src/diagnostics/diagnostics.cpp

    src/lexer/tokens.cpp
    src/lexer/lexer.cpp

    src/ast/walk.cpp
    src/ast/printer.cpp

    src/parser/parser.cpp

    src/ssa/node.cpp
)

target_include_directories(tetherc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---- Tests ---------------------------------------------------------------
# Tests are a separate CMake target so the main build stays minimal.
# Run with: cmake --build build --target tether_tests && ctest --test-dir build

enable_testing()

add_executable(tether_tests
    tests/test_lexer.cpp
    tests/test_parser.cpp
    tests/test_ast.cpp
    tests/main.cpp

    src/support/arena.cpp
    src/support/intern.cpp
    src/support/source.cpp

    src/diagnostics/diagnostics.cpp

    src/lexer/tokens.cpp
    src/lexer/lexer.cpp

    src/ast/walk.cpp
    src/ast/printer.cpp

    src/parser/parser.cpp

    src/ssa/node.cpp
)

target_include_directories(tether_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

add_test(NAME tether_tests COMMAND tether_tests)
