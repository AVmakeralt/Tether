// resolve/resolve.hpp — name resolution + symbol tables
//
// The resolver walks the AST and resolves every identifier and path to
// a concrete declaration. It builds symbol tables for:
//
//   - Modules (path -> ModuleDecl*)
//   - Types (name -> TypeDecl*)
//   - Functions (name -> FnDecl*)
//   - Local bindings (name -> BindingDecl*)
//
// After resolution, every Ident and Path expression in the AST has
// been resolved to a Decl* (or an error has been reported).
//
// The resolver also handles imports: it walks the module's import
// list and makes the imported names visible in the current scope.

#pragma once

#include "ast/nodes.hpp"
#include "diagnostics/diagnostics.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"
#include "support/source.hpp"
#include "types/types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace tether::resolve {

using ast::ItemPtr;
using ast::ExprPtr;
using ast::StmtPtr;
using ast::PatternPtr;
using type::TypePtr;

// A resolved declaration. All declarations (functions, structs, enums,
// etc.) are represented by the AST Item they came from; the resolver
// just records the Item* and the resolved type.
struct Decl {
    enum class Kind : uint8_t {
        Function,
        Struct,
        Enum,
        Union,
        Trait,
        TypeAlias,
        Const,
        Static,
        ExternFn,
        Binding,    // local let-binding
        Param,      // function parameter
        Self,       // the implicit 'self' parameter
        Variant,    // enum variant
        Field,      // struct/union field
        Module,     // imported module (placeholder for cross-module refs)
    };

    Kind        kind;
    StrId       name      = kInvalidStrId;
    ItemPtr     item      = nullptr;   // for top-level decls
    TypePtr     type      = nullptr;   // resolved type
    SourceRange range;

    // For bindings/params: the slot in the function's local table.
    uint32_t    slot      = 0xFFFFFFFFu;

    // For variants: the enum they belong to + index.
    ItemPtr     parent_enum = nullptr;
    uint32_t    variant_idx = 0;

    // For struct fields: index in the struct.
    uint32_t    field_idx = 0;
};

using DeclPtr = const Decl*;

// Mutable Decl pointer — used internally by the resolver when updating
// resolved types after the first pass.
using DeclMutPtr = Decl*;

// A scope is a flat symbol table. Scopes are stacked: when resolving
// an identifier, we look in the innermost scope first, then walk
// outward. There is no shadowing warning — inner scopes simply hide
// outer declarations.
struct Scope {
    std::unordered_map<StrId, Decl*> names;
};

class Resolver {
public:
    Resolver(type::TypeContext& tc, DiagnosticEmitter& diag,
             InternTable& intern, Arena& arena)
        : tc_(tc), diag_(diag), intern_(intern), arena_(arena) {}

    // Resolve a module. Returns true on success (no errors).
    bool resolve_module(const ast::Module& m);

    bool has_errors() const { return diag_.has_errors(); }

private:
    type::TypeContext&  tc_;
    DiagnosticEmitter&  diag_;
    InternTable&        intern_;
    Arena&              arena_;

    // Top-level declarations in the current module. Keyed by name.
    // Stored as non-const Decl* so we can update the resolved type
    // after the first pass.
    std::unordered_map<StrId, Decl*> top_level_;

    // Local scopes. The back() is the innermost scope.
    std::vector<Scope> scopes_;

    // Stack of function-local binding slots. Each function gets a
    // fresh slot counter.
    uint32_t next_slot_ = 0;

    // ---- Scope helpers ----
    void push_scope() { scopes_.emplace_back(); }
    void pop_scope()  { scopes_.pop_back(); }
    DeclPtr lookup_local(StrId name) const;

    // ---- Declaration registration ----
    Decl* register_decl(Decl d);

    // ---- Type resolution ----
    //
    // Resolve an AST type to a type::Type. Handles primitive names,
    // user-defined struct/enum/union names, refs, arrays, tuples, fn
    // types, and generic type arguments.
    TypePtr resolve_type(ast::TypePtr t);

    // ---- Item resolution ----
    void resolve_item(ItemPtr item);
    void resolve_fn(ItemPtr item);
    void resolve_struct(ItemPtr item);
    void resolve_enum(ItemPtr item);
    void resolve_union(ItemPtr item);
    void resolve_trait(ItemPtr item);
    void resolve_impl(ItemPtr item);
    void resolve_type_alias(ItemPtr item);
    void resolve_const(ItemPtr item, bool is_static);
    void resolve_extern(ItemPtr item);

    // ---- Statement / expression / pattern resolution ----
    void resolve_block(ast::BlockPtr b);
    void resolve_stmt(StmtPtr s);
    TypePtr resolve_expr(ExprPtr e);
    void resolve_pattern(PatternPtr p, TypePtr expected = nullptr);
};

} // namespace tether::resolve
