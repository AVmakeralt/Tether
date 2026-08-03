// ast/nodes.hpp — AST node definitions
//
// Design:
//   AST nodes are immutable, arena-allocated, and reference each other
//   by raw pointer (the arena outlives every node it contains). The
//   spec mandates immutable trees with structural sharing on rewrites;
//   v0.1 enforces immutability by exposing only const accessors and by
//   constructing nodes through a builder that never mutates after
//   construction.
//
//   Every node carries a SourceRange. The AST has no ownership; it is
//   all owned by the Arena.

#pragma once

#include "support/arena.hpp"
#include "support/intern.hpp"
#include "support/source.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tether {

// Forward declarations for all node kinds.
namespace ast {

class Node;
class Module;
class Item;
class Type;
class Pattern;
class Expr;
class Stmt;
class Block;

using NodePtr     = const Node*;
using ModulePtr   = const Module*;
using ItemPtr     = const Item*;
using TypePtr     = const Type*;
using PatternPtr  = const Pattern*;
using ExprPtr     = const Expr*;
using StmtPtr     = const Stmt*;
using BlockPtr    = const Block*;

// ---- Type AST ----------------------------------------------------------

enum class TypeKind : uint8_t {
    Named,        // Foo, Foo::Bar, foo::Bar<T>
    Ref,          // ref Foo, mut ref Foo, ref(region) Foo
    BorrowRef,    // borrow ref Foo, borrow mut ref Foo
    RawPtr,       // *const Foo, *mut Foo (only in extern)
    Array,        // [T; N]
    Slice,        // [T]
    Tuple,        // (T, U, V)
    Fn,           // fn(T, U) -> V
    Infer,        // _  (let compiler infer)
};

struct TypeRef {
    TypePtr type = nullptr;
};

class Type {
public:
    TypeKind kind = TypeKind::Infer;
    SourceRange range;

    // Named: name path + optional type args.
    // For Ref: base type.
    // For Array/Slice: element type + (Array only) length expr.
    // For Tuple: element types.
    // For Fn: param types + return type.
    TypePtr base = nullptr;            // Ref, BorrowRef, RawPtr, Array, Slice
    std::vector<TypePtr> args;         // Named (type args), Tuple, Fn params
    ExprPtr length = nullptr;          // Array length, may be null

    // Named: path components (interned).
    std::vector<StrId> path;

    // Mutability flags.
    bool is_mut       = false;         // mut ref, *mut, etc.
    bool is_borrow    = false;         // borrow ref (vs plain ref)
    StrId region      = kInvalidStrId; // ref(region) T

    // Fn return type (null = unit).
    TypePtr return_type = nullptr;

    // Raw pointer kind.
    bool is_const_ptr = false;
};

// ---- Pattern AST -------------------------------------------------------

enum class PatternKind : uint8_t {
    Wildcard,     // _
    Binding,      // x
    Bool,         // true / false
    Int,          // 42, -7
    String,       // "..."
    Char,         // 'a'
    Variant,      // Foo(a, b)  or Foo::Bar
    Tuple,        // (a, b, c)
    Struct,       // { x: a, y: b, .. }
    As,           // pat as x
};

class Pattern {
public:
    PatternKind kind = PatternKind::Wildcard;
    SourceRange range;

    // Binding (and As target).
    StrId name = kInvalidStrId;
    bool is_mut = false;

    // Bool / Int / Char.
    uint64_t int_value = 0;

    // String.
    StrId str_value = kInvalidStrId;

    // Variant.
    std::vector<StrId> path;             // Foo::Bar
    std::vector<PatternPtr> subpatterns; // (a, b)

    // Tuple.
    // (uses subpatterns)

    // Struct.
    struct FieldPattern {
        StrId       name      = kInvalidStrId;
        PatternPtr  sub       = nullptr;
        bool        shorthand = false; // {x} == {x: x}
    };
    std::vector<FieldPattern> fields;
    bool has_rest = false;               // {..}

    // As.
    PatternPtr inner = nullptr;
};

// ---- Expression AST ----------------------------------------------------

enum class ExprKind : uint8_t {
    // Literals
    IntLit,
    FloatLit,
    StringLit,
    CharLit,
    BoolLit,

    // Identifiers and paths
    Ident,
    Path,            // Foo::Bar::baz

    // Operations
    Unary,
    Binary,
    Assign,
    Call,
    MethodCall,      // x.method(args)
    FieldAccess,
    Index,
    Question,        // expr?

    // Blocks and control flow
    Block,
    If,
    Match,
    Loop,
    While,
    For,
    Return,
    Break,
    Continue,
    Defer,

    // Memory
    Alloc,
    Move,
    Borrow,
    Unsafe,

    // Concurrency
    Spawn,
    Await,

    // Misc
    Tuple,
    ArrayLit,
};

enum class UnaryOp : uint8_t {
    Neg,        // -
    Not,        // !
    BitNot,     // ~
    Deref,      // *  (only in unsafe)
    Borrow,     // borrow
    BorrowMut,  // borrow mut
    Move,       // move
};

enum class BinaryOp : uint8_t {
    Add, Sub, Mul, Div, Mod,
    Eq, Neq, Lt, Gt, Le, Ge,
    And, Or,                    // logical
    BitAnd, BitOr, BitXor,
    Shl, Shr,
};

enum class AssignOp : uint8_t {
    Assign, AddAssign, SubAssign, MulAssign, DivAssign, ModAssign,
};

struct MatchArm {
    PatternPtr pattern = nullptr;
    ExprPtr    body    = nullptr;
    SourceRange range;
};

class Expr {
public:
    ExprKind kind = ExprKind::IntLit;
    SourceRange range;

    // Literals.
    uint64_t int_value   = 0;
    double   float_value = 0.0;
    StrId    str_value   = kInvalidStrId;

    // Ident / Path / MethodCall target.
    std::vector<StrId> path;
    StrId method_name = kInvalidStrId; // MethodCall

    // Unary / Binary / Assign.
    UnaryOp  unary_op  = UnaryOp::Neg;
    BinaryOp binary_op = BinaryOp::Add;
    AssignOp assign_op = AssignOp::Assign;
    ExprPtr  lhs = nullptr;
    ExprPtr  rhs = nullptr;

    // Call / MethodCall / Tuple / ArrayLit args.
    std::vector<ExprPtr> args;

    // Field access / Index.
    StrId   field_name = kInvalidStrId;
    ExprPtr index      = nullptr;

    // Block.
    BlockPtr block = nullptr;

    // If / Match / While / For.
    ExprPtr cond        = nullptr;
    ExprPtr then_branch = nullptr;
    ExprPtr else_branch = nullptr;
    std::vector<MatchArm> arms;        // Match
    StrId   loop_var    = kInvalidStrId; // For
    ExprPtr iterable    = nullptr;       // For
    ExprPtr body        = nullptr;       // Loop, While, For, Spawn

    // Return.
    ExprPtr return_value = nullptr;

    // Alloc.
    enum class AllocTarget : uint8_t { Arena, Heap, Named };
    AllocTarget alloc_target = AllocTarget::Heap;
    StrId       alloc_arena_name = kInvalidStrId; // Named
    ExprPtr     alloc_value = nullptr;

    // Tuple / ArrayLit / ArrayLit-with-type.
    TypePtr element_type = nullptr;
};

// ---- Statements --------------------------------------------------------

enum class StmtKind : uint8_t {
    Let,
    Expr,
    Return,
    Defer,
    Break,
    Continue,
    Unsafe,
    Block,
};

class Stmt {
public:
    StmtKind kind = StmtKind::Expr;
    SourceRange range;

    // Let.
    StrId    let_name    = kInvalidStrId;
    bool     let_is_mut  = false;
    TypePtr  let_type    = nullptr;
    ExprPtr  let_value   = nullptr;

    // Expr / Return.
    ExprPtr  expr        = nullptr;       // Expr stmt, Return value

    // Block / Unsafe.
    BlockPtr block       = nullptr;
};

class Block {
public:
    SourceRange range;
    std::vector<StmtPtr> stmts;
    ExprPtr trailing = nullptr;            // final expr (block-as-expr)
};

// ---- Items -------------------------------------------------------------

enum class ItemKind : uint8_t {
    Module,        // module foo::bar
    Import,        // import foo::bar
    Export,        // export <inner item>
    Fn,
    Struct,
    Enum,
    Union,
    Trait,
    Impl,
    TypeAlias,
    Const,
    Static,
    Extern,
};

struct Field {
    StrId    name = kInvalidStrId;
    TypePtr  type = nullptr;
    SourceRange range;
};

struct Variant {
    StrId    name = kInvalidStrId;
    std::vector<TypePtr> args;
    SourceRange range;
};

struct Param {
    StrId    name          = kInvalidStrId;
    TypePtr  type          = nullptr;
    bool     is_self       = false;
    bool     is_borrow     = false;
    bool     is_borrow_mut = false;
    bool     is_variadic   = false;          // extern only
    SourceRange range;
};

struct TypeParam {
    StrId    name = kInvalidStrId;
    std::vector<std::vector<StrId>> bounds; // each bound is a path
    SourceRange range;
};

class Item {
public:
    ItemKind kind = ItemKind::Fn;
    SourceRange range;

    // Module / Import: path.
    std::vector<StrId> path;
    StrId import_alias = kInvalidStrId;     // import ... as foo

    // Export wraps an inner item.
    ItemPtr inner = nullptr;

    // Fn / Struct / Enum / Union / Trait / Impl / TypeAlias / Const /
    // Static: name + type params.
    StrId name = kInvalidStrId;
    std::vector<TypeParam> type_params;

    // Fn.
    std::vector<Param> params;
    TypePtr return_type = nullptr;
    BlockPtr body = nullptr;
    bool is_extern = false;

    // Struct / Union fields.
    std::vector<Field> fields;

    // Enum variants.
    std::vector<Variant> variants;

    // Trait members (signatures only).
    std::vector<ItemPtr> trait_members;

    // Impl: target type + optional trait being implemented.
    TypePtr impl_type  = nullptr;
    TypePtr impl_trait = nullptr;
    std::vector<ItemPtr> impl_members;

    // TypeAlias: underlying type.
    TypePtr alias_type = nullptr;

    // Const / Static: value.
    TypePtr const_type = nullptr;
    ExprPtr const_value = nullptr;

    // Extern: header name (for ffi "..."), declaration body.
    StrId  ffi_header = kInvalidStrId;
    ItemPtr extern_decl = nullptr;
};

class Module {
public:
    SourceRange range;
    std::vector<StrId> module_path;
    std::vector<ItemPtr> items;
};

// ---- Visitor -----------------------------------------------------------

class Visitor {
public:
    virtual ~Visitor() = default;
    virtual void visit_module(const Module&) {}
    virtual void visit_item(const Item&) {}
    virtual void visit_type(const Type&) {}
    virtual void visit_pattern(const Pattern&) {}
    virtual void visit_expr(const Expr&) {}
    virtual void visit_stmt(const Stmt&) {}
    virtual void visit_block(const Block&) {}
};

// Walk the AST depth-first, calling the appropriate visitor method on
// each node. The default implementation does not recurse — subclasses
// may call these helpers manually if needed.
void walk(const Module& m, Visitor& v);
void walk(const Item& i, Visitor& v);
void walk(const Type& t, Visitor& v);
void walk(const Pattern& p, Visitor& v);
void walk(const Expr& e, Visitor& v);
void walk(const Stmt& s, Visitor& v);
void walk(const Block& b, Visitor& v);

} // namespace ast

// ---- AST builder -------------------------------------------------------
//
// The builder is the only way to create AST nodes. It allocates from an
// arena; nodes are immutable once constructed. All fields are public
// for in-place construction via aggregate init on a temporary, then
// copied into the arena.

class AstBuilder {
public:
    explicit AstBuilder(Arena& arena) : arena_(arena) {}

    // Allocate an immutable copy of `t` in the arena.
    template <typename T>
    const T* make(T t) {
        return arena_.template construct<T>(std::move(t));
    }

    // Allocate a vector of items as a stable span. The vector itself
    // is arena-allocated so the span does not dangle.
    template <typename T>
    const std::vector<T>* make_vec(std::vector<T> v) {
        return arena_.template construct<std::vector<T>>(std::move(v));
    }

private:
    Arena& arena_;
};

} // namespace tether
