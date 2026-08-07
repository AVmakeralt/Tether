# Tether — CMake build
#
# Mirrors the Makefile. C++17, no external dependencies.

cmake_minimum_required(VERSION 3.16)

project(tether
    VERSION 0.9.0
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
        -DPROJECT_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\"
    )
elseif(MSVC)
    add_compile_options(/W4 /permissive-)
endif()

# Output directories.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---- Source files --------------------------------------------------------

set(SUPPORT_SRC
    src/support/arena.cpp
    src/support/intern.cpp
    src/support/source.cpp
)

set(DIAG_SRC src/diagnostics/diagnostics.cpp)

set(LEXER_SRC
    src/lexer/tokens.cpp
    src/lexer/lexer.cpp
)

set(AST_SRC
    src/ast/walk.cpp
    src/ast/printer.cpp
)

set(PARSER_SRC src/parser/parser.cpp)

set(SSA_SRC
    src/ssa/node.cpp
    src/ssa/builder.cpp
    src/ssa/optimizer.cpp
    src/ssa/emit_llvm.cpp
    src/ssa/partial_eval.cpp
    src/ssa/incremental.cpp
    src/ssa/mono.cpp
    src/ssa/rewrite.cpp
)

set(TYPES_SRC src/types/types.cpp)
set(RESOLVE_SRC src/resolve/resolve.cpp)
set(CHECK_SRC src/check/check.cpp)
set(BORROW_SRC src/borrow/borrow.cpp)
set(LLVM_SRC src/llvm/emit.cpp)
set(MODULE_SRC src/module/loader.cpp)

set(COMPILER_SRC
    ${SUPPORT_SRC}
    ${DIAG_SRC}
    ${LEXER_SRC}
    ${AST_SRC}
    ${PARSER_SRC}
    ${SSA_SRC}
    ${TYPES_SRC}
    ${RESOLVE_SRC}
    ${CHECK_SRC}
    ${BORROW_SRC}
    ${LLVM_SRC}
    ${MODULE_SRC}
)

# ---- tetherc executable --------------------------------------------------

add_executable(tetherc src/main.cpp ${COMPILER_SRC})
target_include_directories(tetherc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---- Tests ---------------------------------------------------------------
# Tests are a separate CMake target so the main build stays minimal.
# Run with: cmake --build build --target tether_tests && ctest --test-dir build

enable_testing()

set(TEST_SRC
    tests/main.cpp
    tests/test_lexer.cpp
    tests/test_parser.cpp
    tests/test_ast.cpp
    tests/test_types.cpp
    tests/test_pipeline.cpp
    tests/test_ssa.cpp
    tests/test_struct_enum.cpp
    tests/test_ffi.cpp
    tests/test_v5.cpp
    tests/test_v6.cpp
    tests/test_v7.cpp
    tests/test_v7b.cpp
    tests/test_v8.cpp
)

add_executable(tether_tests ${TEST_SRC} ${COMPILER_SRC})
target_include_directories(tether_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

add_test(NAME tether_tests COMMAND tether_tests)
