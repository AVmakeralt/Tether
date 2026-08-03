// module/loader.hpp — multi-file module resolution
//
// The loader discovers source files for modules based on their import
// paths. Given a module path like `parser::ast`, it looks for:
//
//   1. parser/ast.tether         (relative to a search root)
//   2. parser/ast/mod.tether     (rust-style module file)
//
// Search roots include:
//   - The directory of the entry-point file
//   - Directories added via add_search_root()
//   - The standard library directory (compiled-in path, can be
//     overridden via TETHER_STDLIB environment variable)
//
// The loader caches parsed modules by file_id so each file is parsed
// exactly once.

#pragma once

#include "ast/nodes.hpp"
#include "diagnostics/diagnostics.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"
#include "support/source.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace tether::module {

struct LoadedModule {
    std::vector<StrId>     path;       // e.g. [parser, ast]
    std::string            file_path;  // physical path, or "<buffer>"
    ast::ModulePtr         ast;        // parsed AST (nullptr on error)
    std::vector<std::vector<StrId>> imports; // paths this module imports
};

class Loader {
public:
    Loader(InternTable& intern, DiagnosticEmitter& diag,
           SourceManager& sm, Arena& arena)
        : intern_(intern), diag_(diag), sm_(sm), arena_(arena) {}

    // Add a directory to the search path.
    void add_search_root(std::string dir) { roots_.push_back(std::move(dir)); }

    // Set the standard library directory.
    void set_stdlib_root(std::string dir) { stdlib_ = std::move(dir); }

    // Load and parse the entry-point file. Returns the loaded module,
    // or nullptr on error.
    const LoadedModule* load_entry(const std::string& path);

    // Load a module by its dotted path (e.g. [parser, ast]). Looks
    // up the file, parses it, caches it, and returns it. Returns
    // nullptr if the module cannot be found.
    const LoadedModule* load_by_path(const std::vector<StrId>& path);

    // All modules loaded so far (including the entry point).
    const std::vector<LoadedModule>& modules() const { return modules_; }

private:
    InternTable&        intern_;
    DiagnosticEmitter&  diag_;
    SourceManager&      sm_;
    Arena&              arena_;

    std::vector<std::string> roots_;
    std::string               stdlib_;

    std::vector<LoadedModule> modules_;
    // Cache: path -> index into modules_.
    std::unordered_map<uint64_t, size_t> cache_;

    // Find a file for the given path components. Returns the full
    // file path, or empty string if not found.
    std::string find_file(const std::vector<StrId>& path) const;

    // Hash a path for cache lookup.
    static uint64_t hash_path(const std::vector<StrId>& path);

    // Parse a file and return its AST.
    ast::ModulePtr parse_file(const std::string& file_path);
};

} // namespace tether::module
