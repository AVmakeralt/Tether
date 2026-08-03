// main.cpp — tetherc driver
//
// Usage:
//   tetherc <file.tether>                Parse, check, lower to SSA, emit LLVM IR.
//   tetherc --emit-llvm <file.tether>    Emit .ll file (default).
//   tetherc --emit-ssa <file.tether>     Emit Tether SSA IR.
//   tetherc --emit-ast <file.tether>     Emit AST pretty-print.
//   tetherc --emit-tokens <file.tether>  Emit token stream.
//   tetherc --check <file.tether>        Parse + type check only.
//   tetherc --no-opt <file.tether>       Skip SSA optimization.
//   tetherc --incremental <file.tether>  Use incremental compilation cache.
//   tetherc --version
//   tetherc --help
//
// The full pipeline is:
//   source -> lexer -> parser -> AST
//          -> resolver -> type checker -> borrow checker
//          -> SSA builder (AST → SSA, with ownership/region/arena tracking)
//          -> SSA optimizer (CSE, DCE, const fold, SCCP, CFG simplify)
//          -> SSA → LLVM IR text (.ll)
//
// Why not AST → LLVM IR directly?
//   LLVM cannot verify ownership, track allocation domains, enforce
//   region invariants, monomorphize generics, resolve trait dispatch,
//   run rewrite rules, do partial evaluation, or compile pattern
//   matches with structural patterns. All of that happens on Tether's
//   own SSA module. LLVM only sees the final, optimized SSA lowered
//   to its own IR.

#include "ast/printer.hpp"
#include "borrow/borrow.hpp"
#include "check/check.hpp"
#include "diagnostics/diagnostics.hpp"
#include "lexer/lexer.hpp"
#include "module/loader.hpp"
#include "parser/parser.hpp"
#include "resolve/resolve.hpp"
#include "ssa/builder.hpp"
#include "ssa/emit_llvm.hpp"
#include "ssa/incremental.hpp"
#include "ssa/mono.hpp"
#include "ssa/optimizer.hpp"
#include "ssa/partial_eval.hpp"
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
    Ssa,
    Llvm,
    Check,
};

struct Options {
    EmitMode mode = EmitMode::Llvm;
    bool no_opt = false;
    bool incremental = false;
    std::string input;
    std::string output;
    std::string stdlib;
};

void print_help() {
    std::cout <<
        "tetherc — Tether compiler (v0.3: full SSA pipeline)\n"
        "\n"
        "Usage:\n"
        "  tetherc <file.tether>                Full pipeline → LLVM IR.\n"
        "  tetherc --emit-llvm <file.tether>    Emit .ll file (default).\n"
        "  tetherc --emit-ssa <file.tether>     Emit Tether SSA IR.\n"
        "  tetherc --emit-ast <file.tether>     Emit AST pretty-print.\n"
        "  tetherc --emit-tokens <file.tether>  Emit token stream.\n"
        "  tetherc --check <file.tether>        Parse + check only.\n"
        "  tetherc --no-opt <file.tether>       Skip SSA optimization.\n"
        "  tetherc --incremental <file.tether>  Use incremental cache.\n"
        "  tetherc -o <output> <file.tether>    Write output to <output>.\n"
        "  tetherc --stdlib <dir> <file.tether> Use <dir> as stdlib root.\n"
        "  tetherc --version\n"
        "  tetherc --help\n"
        "\n"
        "Pipeline: source → lexer → parser → AST → resolver →\n"
        "          type checker → borrow checker → SSA builder →\n"
        "          SSA optimizer → LLVM IR\n"
        "\n"
        "Tether's SSA module is its own IR. LLVM only sees the final,\n"
        "optimized SSA lowered to LLVM IR. This is required because LLVM\n"
        "cannot verify ownership, track allocation domains, enforce region\n"
        "invariants, monomorphize generics, resolve trait dispatch, run\n"
        "rewrite rules, do partial evaluation, or compile pattern matches.\n";
}

void print_version() {
    std::cout << "tetherc 0.3.0\n";
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
        if (a == "--emit-llvm")        opts.mode = EmitMode::Llvm;
        else if (a == "--emit-ssa")         opts.mode = EmitMode::Ssa;
        else if (a == "--emit-ast")         opts.mode = EmitMode::Ast;
        else if (a == "--emit-tokens")      opts.mode = EmitMode::Tokens;
        else if (a == "--check")            opts.mode = EmitMode::Check;
        else if (a == "--no-opt")           opts.no_opt = true;
        else if (a == "--incremental")      opts.incremental = true;
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

    if (diag.has_errors()) {
        std::cerr << diag.render(sm);
        return 1;
    }

    // Resolve + type check + borrow check every module.
    tether::type::TypeContext tc(arena, intern);
    for (const auto& m : loader.modules()) {
        if (!m.ast) continue;
        tether::resolve::Resolver resolver(tc, diag, intern, arena);
        resolver.resolve_module(*m.ast);
        tether::check::TypeChecker checker(tc, diag, resolver, intern);
        checker.check_module(*m.ast);
        tether::borrow::BorrowChecker borrow(tc, diag, resolver, intern);
        borrow.check_module(*m.ast);
    }

    if (diag.has_errors()) {
        std::cerr << diag.render(sm);
        if (opts.mode == EmitMode::Check) return 1;
    }

    // Lower each module to SSA (with monomorphization first).
    std::vector<tether::ssa::Module> ssa_modules;
    for (const auto& m : loader.modules()) {
        if (!m.ast) continue;
        // Run monomorphization: instantiate generic functions per
        // concrete type. This must happen before SSA lowering because
        // LLVM has no concept of generics.
        tether::mono::Monomorphizer mono(tc, diag, intern, arena);
        auto monomorphized = mono.run(*m.ast);
        tether::ssa::Builder builder(tc, diag, intern, arena);
        ssa_modules.push_back(builder.lower_module(*monomorphized));
    }

    // Optimize (unless --no-opt).
    if (!opts.no_opt) {
        tether::ssa::Optimizer opt(tc, diag);
        for (auto& ssa_mod : ssa_modules) {
            opt.run(ssa_mod);
        }
    }

    switch (opts.mode) {
        case EmitMode::Tokens: {
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
        case EmitMode::Ssa: {
            std::string all;
            for (const auto& ssa_mod : ssa_modules) {
                all += tether::ssa::render_module(ssa_mod, tc);
                all += "\n";
            }
            if (!opts.output.empty()) {
                std::ofstream out(opts.output);
                if (!out) {
                    std::cerr << "tetherc: cannot write '" << opts.output
                              << "'\n";
                    return 2;
                }
                out << all;
                std::cout << "wrote " << opts.output << " ("
                          << all.size() << " bytes)\n";
            } else {
                std::cout << all;
            }
            return diag.has_errors() ? 1 : 0;
        }
        case EmitMode::Llvm: {
            std::string all_ir;
            for (const auto& ssa_mod : ssa_modules) {
                tether::ssa::LlvmEmitter emitter(tc, diag, intern);
                all_ir += emitter.emit(ssa_mod);
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
