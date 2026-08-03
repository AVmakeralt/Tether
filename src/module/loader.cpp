// module/loader.cpp — multi-file module resolution

#include "module/loader.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <utility>

namespace tether::module {

namespace {

bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool dir_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

uint64_t Loader::hash_path(const std::vector<StrId>& path) {
    uint64_t h = 1469598103934665603ULL; // FNV offset basis
    for (StrId s : path) {
        h ^= s;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string Loader::find_file(const std::vector<StrId>& path) const {
    if (path.empty()) return {};

    // Build the relative path: "foo/bar/baz"
    std::string rel;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i) rel += "/";
        rel += std::string(intern_.get(path[i]));
    }

    // Collect search roots: stdlib first, then user roots.
    std::vector<std::string> all_roots = roots_;
    if (!stdlib_.empty()) all_roots.push_back(stdlib_);

    for (const std::string& root : all_roots) {
        // Try foo/bar/baz.tether
        std::string candidate = join(root, rel + ".tether");
        if (file_exists(candidate)) return candidate;

        // Try foo/bar/baz/mod.tether
        candidate = join(root, rel + "/mod.tether");
        if (file_exists(candidate)) return candidate;
    }
    return {};
}

ast::ModulePtr Loader::parse_file(const std::string& file_path) {
    std::string content = read_file(file_path);
    if (content.empty()) {
        diag_.error({}, "cannot read file: " + file_path);
        return nullptr;
    }
    uint32_t fid = sm_.load_buffer(file_path, std::move(content));
    const SourceFile& f = sm_.file(fid);
    Lexer lexer(intern_, diag_, f);
    auto tokens = lexer.tokenize();
    Parser parser(intern_, diag_, sm_, arena_, std::move(tokens));
    return parser.parse_module();
}

const LoadedModule* Loader::load_entry(const std::string& path) {
    ast::ModulePtr ast = parse_file(path);
    if (!ast) return nullptr;

    LoadedModule m;
    m.file_path = path;
    m.ast = ast;
    m.path = ast->module_path;
    // Extract imports.
    for (ast::ItemPtr item : ast->items) {
        if (item && item->kind == ast::ItemKind::Import) {
            m.imports.push_back(item->path);
        }
    }
    modules_.push_back(std::move(m));
    size_t idx = modules_.size() - 1;

    // Cache it.
    if (!ast->module_path.empty()) {
        cache_[hash_path(ast->module_path)] = idx;
    }

    // Auto-discover the entry's directory as a search root.
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        roots_.push_back(path.substr(0, slash));
    } else {
        roots_.push_back(".");
    }

    // Recursively load imports.
    for (const auto& imp : modules_[idx].imports) {
        (void)load_by_path(imp);
    }
    return &modules_[idx];
}

const LoadedModule* Loader::load_by_path(const std::vector<StrId>& path) {
    if (path.empty()) return nullptr;
    uint64_t h = hash_path(path);
    auto it = cache_.find(h);
    if (it != cache_.end()) return &modules_[it->second];

    std::string file = find_file(path);
    if (file.empty()) {
        std::string dotted;
        for (size_t i = 0; i < path.size(); ++i) {
            if (i) dotted += "::";
            dotted += std::string(intern_.get(path[i]));
        }
        diag_.error({}, "cannot find module '" + dotted + "'");
        return nullptr;
    }

    ast::ModulePtr ast = parse_file(file);
    if (!ast) return nullptr;

    LoadedModule m;
    m.file_path = file;
    m.ast = ast;
    m.path = path;
    for (ast::ItemPtr item : ast->items) {
        if (item && item->kind == ast::ItemKind::Import) {
            m.imports.push_back(item->path);
        }
    }
    modules_.push_back(std::move(m));
    size_t idx = modules_.size() - 1;
    cache_[h] = idx;

    // Recursively load this module's imports.
    for (const auto& imp : modules_[idx].imports) {
        (void)load_by_path(imp);
    }
    return &modules_[idx];
}

} // namespace tether::module
