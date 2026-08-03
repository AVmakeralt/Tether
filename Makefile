# Tether — Makefile (no CMake required)
#
# Usage:
#   make            build the compiler (bin/tetherc)
#   make tests      build and run the test suite
#   make clean      remove build artifacts

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic \
            -Wno-unused-parameter -Werror=return-type
INCLUDES := -Isrc -Itests

# ---- Source files --------------------------------------------------------

SUPPORT_SRC := \
    src/support/arena.cpp \
    src/support/intern.cpp \
    src/support/source.cpp

DIAG_SRC := src/diagnostics/diagnostics.cpp

LEXER_SRC := \
    src/lexer/tokens.cpp \
    src/lexer/lexer.cpp

AST_SRC := \
    src/ast/walk.cpp \
    src/ast/printer.cpp

PARSER_SRC := src/parser/parser.cpp

SSA_SRC := \
    src/ssa/node.cpp \
    src/ssa/builder.cpp \
    src/ssa/optimizer.cpp \
    src/ssa/emit_llvm.cpp \
    src/ssa/partial_eval.cpp \
    src/ssa/incremental.cpp

TYPES_SRC := src/types/types.cpp

RESOLVE_SRC := src/resolve/resolve.cpp

CHECK_SRC := src/check/check.cpp

BORROW_SRC := src/borrow/borrow.cpp

LLVM_SRC := src/llvm/emit.cpp

MODULE_SRC := src/module/loader.cpp

COMPILER_SRC := \
    $(SUPPORT_SRC) \
    $(DIAG_SRC) \
    $(LEXER_SRC) \
    $(AST_SRC) \
    $(PARSER_SRC) \
    $(SSA_SRC) \
    $(TYPES_SRC) \
    $(RESOLVE_SRC) \
    $(CHECK_SRC) \
    $(BORROW_SRC) \
    $(LLVM_SRC) \
    $(MODULE_SRC)

# ---- Compiler binary -----------------------------------------------------

bin/tetherc: src/main.cpp $(COMPILER_SRC) | bin
	$(CXX) $(CXXFLAGS) $(INCLUDES) src/main.cpp $(COMPILER_SRC) -o $@

bin:
	mkdir -p bin

# ---- Tests ---------------------------------------------------------------

TEST_SRC := \
    tests/main.cpp \
    tests/test_lexer.cpp \
    tests/test_parser.cpp \
    tests/test_ast.cpp \
    tests/test_types.cpp \
    tests/test_pipeline.cpp \
    tests/test_ssa.cpp

bin/tether_tests: $(TEST_SRC) $(COMPILER_SRC) | bin
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(TEST_SRC) $(COMPILER_SRC) -o $@

tests: bin/tether_tests
	./bin/tether_tests

# ---- Convenience ---------------------------------------------------------

clean:
	rm -rf bin obj

.PHONY: tests clean
