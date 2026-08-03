// main.cpp — tetherc driver
//
// Usage:
//   tetherc <file.tether>                Parse, check, emit LLVM IR.
//   tetherc --emit-llvm <file.tether>    Emit .ll file (default).
//   tetherc --emit-ast <file.tether>     Emit AST pretty-print.
//   tetherc --emit-tokens <file.tether>  Emit token stream.
//   tetherc --check <file.tether>        Parse + type check only.
//   tetherc --version
//   tetherc --help
//
// The default pipeline is:
//   source -> lexer -> parser -> AST
//          -> resolver -> type checker -> borrow checker
//          -> LLVM IR text (.ll)
//
// Multi-file: imports are resolved relative to the entry file's
// directory and the standard library directory.

#include "ast/printer.hpp"
#include "borrow/borrow.hpp"
#include "check/check.hpp"
#include "diagnostics/diagnostics.hpp"
#include "lexer/lexer.hpp"
#include "llvm/emit.hpp"
#include "module/loader.hpp"
#include "parser/parser.hpp"
#include "resolve/resolve.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"
#include "support/source.hpp"
#include "types/types.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

enum class EmitMode {
    Tokens,
    Ast,
    Llvm,
    Check,
};

struct Options {
    EmitMode mode = EmitMode::Llvm;
    std::string input;
    std::string output;
    std::string stdlib;
};

void print_help() {
    std::cout <<
        "tetherc — Tether compiler (v0.2: full pipeline to LLVM IR)\n"
        "\n"
        "Usage:\n"
        "  tetherc <file.tether>                Lex, parse, check, emit LLVM IR.\n"
        "  tetherc --emit-llvm <file.tether>    Emit .ll file (default).\n"
        "  tetherc --emit-ast <file.tether>     Emit AST pretty-print.\n"
        "  tetherc --emit-tokens <file.tether>  Emit token stream.\n"
        "  tetherc --check <file.tether>        Parse + check only (no codegen).\n"
        "  tetherc -o <output> <file.tether>    Write output to <output>.\n"
        "  tetherc --stdlib <dir> <file.tether> Use <dir> as stdlib root.\n"
        "  tetherc --version\n"
        "  tetherc --help\n"
        "\n"
        "Pipeline: source -> lexer -> parser -> AST -> resolver ->\n"
        "          type checker -> borrow checker -> LLVM IR\n"
        "\n"
        "Multi-file: imports are resolved relative to the entry file's\n"
        "directory and the stdlib root.\n";
}

void print_version() {
    std::cout << "tetherc 0.2.0\n";
}

bool parse_args(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            print_help();
            std::exit(0);
        }
        if (a == "--version" || a == "-v") {
            print_version();
            std::exit(0);
        }
        if (a == "--emit-llvm")   opts.mode = EmitMode::Llvm;
        else if (a == "--emit-ast")    opts.mode = EmitMode::Ast;
        else if (a == "--emit-tokens") opts.mode = EmitMode::Tokens;
        else if (a == "--check")       opts.mode = EmitMode::Check;
        else if (a == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "tetherc: -o requires an argument\n";
                return false;
            }
            opts.output = argv[++i];
        }
        else if (a == "--stdlib") {
            if (i + 1 >= argc) {
                std::cerr << "tetherc: --stdlib requires an argument\n";
                return false;
            }
            opts.stdlib = argv[++i];
        }
        else if (a.size() >= 2 && a[0] == '-' && a[1] == '-') {
            std::cerr << "tetherc: unknown option '" << a << "'\n";
            return false;
        }
        else if (opts.input.empty()) {
            opts.input = a;
        }
        else {
            std::cerr << "tetherc: unexpected argument '" << a << "'\n";
            return false;
        }
    }
    if (opts.input.empty()) {
        std::cerr << "usage: tetherc <file.tether>\n"
                  << "       tetherc --help\n";
        return false;
    }
    return true;
}

int run(Options& opts) {
    tether::InternTable       intern;
    tether::DiagnosticEmitter diag;
    tether::SourceManager     sm;
    tether::Arena             arena;

    // Determine stdlib root.
    std::string stdlib = opts.stdlib;
    if (stdlib.empty()) {
        if (const char* env = std::getenv("TETHER_STDLIB")) {
            stdlib = env;
        }
    }

    // Load and parse all modules.
    tether::module::Loader loader(intern, diag, sm, arena);
    if (!stdlib.empty()) loader.set_stdlib_root(stdlib);

    const tether::module::LoadedModule* entry = loader.load_entry(opts.input);
    if (!entry) {
        std::cerr << diag.render(sm);
        return 2;
    }

    // Lex errors are reported during load.
    if (diag.has_errors()) {
        std::cerr << diag.render(sm);
        return 1;
    }

    // Resolve + type check + borrow check every module.
    tether::type::TypeContext tc(arena, intern);
    bool all_ok = true;
    for (const auto& m : loader.modules()) {
        if (!m.ast) continue;
        tether::resolve::Resolver resolver(tc, diag, intern, arena);
        if (!resolver.resolve_module(*m.ast)) all_ok = false;

        tether::check::TypeChecker checker(tc, diag, resolver, intern);
        if (!checker.check_module(*m.ast)) all_ok = false;

        tether::borrow::BorrowChecker borrow(tc, diag, resolver, intern);
        if (!borrow.check_module(*m.ast)) all_ok = false;
    }

    if (diag.has_errors()) {
        std::cerr << diag.render(sm);
        if (opts.mode == EmitMode::Check) return 1;
        // For codegen modes, still try to emit partial output.
    }

    switch (opts.mode) {
        case EmitMode::Tokens: {
            // Re-lex the entry file and print tokens.
            uint32_t fid = sm.load_file(opts.input);
            const tether::SourceFile& f = sm.file(fid);
            tether::Lexer lexer(intern, diag, f);
            auto tokens = lexer.tokenize();
            for (const auto& t : tokens) {
                std::cout << tether::token_kind_name(t.kind);
                if (t.str_id != tether::kInvalidStrId) {
                    std::cout << "(\"" << intern.get(t.str_id) << "\")";
                } else if (t.kind == tether::TokenKind::IntLit) {
                    std::cout << "(" << t.int_val << ")";
                } else if (t.kind == tether::TokenKind::FloatLit) {
                    std::cout << "(" << t.float_val << ")";
                }
                std::cout << "\n";
            }
            return 0;
        }
        case EmitMode::Ast: {
            tether::ast::Printer printer(std::cout, intern);
            printer.print(*entry->ast);
            return diag.has_errors() ? 1 : 0;
        }
        case EmitMode::Check:
            return diag.has_errors() ? 1 : 0;
        case EmitMode::Llvm: {
            // Emit LLVM IR for every module, concatenated.
            std::string all_ir;
            for (const auto& m : loader.modules()) {
                if (!m.ast) continue;
                tether::llvm::Emitter emitter(tc, diag, intern, arena);
                all_ir += emitter.emit_module(*m.ast);
                all_ir += "\n";
            }
            if (!opts.output.empty()) {
                std::ofstream out(opts.output);
                if (!out) {
                    std::cerr << "tetherc: cannot write '" << opts.output
                              << "'\n";
                    return 2;
                }
                out << all_ir;
                std::cout << "wrote " << opts.output << " ("
                          << all_ir.size() << " bytes)\n";
            } else {
                std::cout << all_ir;
            }
            return diag.has_errors() ? 1 : 0;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse_args(argc, argv, opts)) return 2;
    return run(opts);
}
