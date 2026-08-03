// types/types.hpp — Tether type representation
//
// The type system is the foundation for name resolution, type checking,
// borrow checking, and LLVM IR emission. Types are arena-allocated and
// interned (two types with the same structure share the same TypePtr).
//
// v0.2 supports:
//   - Integer types: i8, i16, i32, i64, u8, u16, u32, u64
//   - Float types: f32, f64
//   - bool, void (unit)
//   - ref T, mut ref T (with optional region)
//   - *const T, *mut T (unsafe only)
//   - [T; N] arrays, [T] slices
//   - (T, U, V) tuples
//   - fn(Sig) -> T function types
//   - User-defined struct, enum, union, trait types
//   - Generic type parameters (TypeVar)
//   - Inference variables (solved during type checking)

#pragma once

#include "support/arena.hpp"
#include "support/intern.hpp"
#include "support/source.hpp"

#include <cstdint>
#include <vector>

namespace tether::type {

enum class Kind : uint8_t {
    // Primitives
    Int,        // iN / uN (bit_width, is_signed)
    Float,      // fN (bit_width)
    Bool,
    Void,       // unit type ()

    // References
    Ref,        // ref T, mut ref T
    RawPtr,     // *const T, *mut T (unsafe only)

    // Aggregates
    Array,      // [T; N]
    Slice,      // [T]
    Tuple,      // (T, U, V)
    Struct,     // user-defined struct (possibly generic instantiation)
    Enum,       // user-defined enum
    Union,      // user-defined union

    // Functions
    Fn,         // fn(T, U) -> V

    // Traits
    Trait,      // trait object (dyn Trait)

    // Polymorphism
    TypeVar,    // generic parameter T
    Infer,      // inference variable (solved during checking)

    // Error
    Error,      // type error placeholder; propagation stops cascading errors
};

// Forward declarations.
struct Type;
using TypePtr = const Type*;

// A region/lifetime identifier. 0 = no region (static). Otherwise an
// index into the region table maintained by the borrow checker.
using RegionId = uint32_t;
constexpr RegionId kStaticRegion = 0;
constexpr RegionId kNoRegion = 0xFFFFFFFFu;

struct Type {
    Kind        kind;
    uint16_t    bit_width  = 0;     // Int, Float
    bool        is_signed  = false; // Int
    bool        is_mut     = false; // Ref, RawPtr
    RegionId    region     = kNoRegion; // Ref

    TypePtr     base       = nullptr;  // Ref, RawPtr, Array, Slice
    std::vector<TypePtr> args;         // Tuple, Fn params, Struct/Enum args
    TypePtr     return_type = nullptr; // Fn

    StrId       name       = kInvalidStrId; // Struct, Enum, Union, Trait

    uint64_t    array_size = 0;      // Array

    uint32_t    var_id     = 0;      // TypeVar, Infer (index into their tables)

    SourceRange loc;                 // where this type was written (for errors)
};

// TypeContext owns the arena and interns types. Two types with the
// same structure return the same TypePtr, which makes equality
// comparison a single pointer comparison.
class TypeContext {
public:
    explicit TypeContext(Arena& arena, InternTable& intern)
        : arena_(arena), intern_(intern) {
        install_primitives();
    }

    // ---- Primitive constructors ----------------------------------------

    TypePtr make_int(uint16_t bits, bool signed_);
    TypePtr make_float(uint16_t bits);
    TypePtr make_bool();
    TypePtr make_void();

    // Convenience accessors for common types.
    TypePtr i8()  const { return i8_; }
    TypePtr i16() const { return i16_; }
    TypePtr i32() const { return i32_; }
    TypePtr i64() const { return i64_; }
    TypePtr u8()  const { return u8_; }
    TypePtr u16() const { return u16_; }
    TypePtr u32() const { return u32_; }
    TypePtr u64() const { return u64_; }
    TypePtr f32() const { return f32_; }
    TypePtr f64() const { return f64_; }
    TypePtr boolean() const { return bool_; }
    TypePtr void_type() const { return void_; }

    // ---- Composite constructors ---------------------------------------

    TypePtr make_ref(TypePtr base, bool is_mut, RegionId region = kNoRegion);
    TypePtr make_raw_ptr(TypePtr base, bool is_mut);
    TypePtr make_array(TypePtr elem, uint64_t size);
    TypePtr make_slice(TypePtr elem);
    TypePtr make_tuple(std::vector<TypePtr> args);
    TypePtr make_fn(std::vector<TypePtr> params, TypePtr ret);
    TypePtr make_struct(StrId name, std::vector<TypePtr> args = {});
    TypePtr make_enum(StrId name, std::vector<TypePtr> args = {});
    TypePtr make_union(StrId name, std::vector<TypePtr> args = {});
    TypePtr make_trait(StrId name);
    TypePtr make_typevar(uint32_t var_id);
    TypePtr make_infer(uint32_t var_id);
    TypePtr make_error();

    // ---- Name-based lookup --------------------------------------------
    //
    // Map primitive type names ("i32", "u64", "f32", "bool", "void",
    // "()", "u8", etc.) to their TypePtr. Returns nullptr if the name
    // is not a builtin primitive.
    TypePtr lookup_primitive(std::string_view name) const;

    // ---- Properties ----------------------------------------------------

    // True if the type is an integer (signed or unsigned).
    bool is_integer(TypePtr t) const { return t && t->kind == Kind::Int; }

    // True if the type is a reference (ref or mut ref).
    bool is_ref(TypePtr t) const { return t && t->kind == Kind::Ref; }

    // True if the type is a raw pointer (*const or *mut).
    bool is_raw_ptr(TypePtr t) const { return t && t->kind == Kind::RawPtr; }

    // True if the type is a signed integer.
    bool is_signed(TypePtr t) const {
        return t && t->kind == Kind::Int && t->is_signed;
    }

    // True if the type is a boolean.
    bool is_boolean(TypePtr t) const {
        return t && t->kind == Kind::Bool;
    }

    // True if the type is a primitive scalar (int, float, bool).
    bool is_scalar(TypePtr t) const {
        return t && (t->kind == Kind::Int || t->kind == Kind::Float ||
                     t->kind == Kind::Bool);
    }

    // True if the type is the unit type.
    bool is_void(TypePtr t) const { return t && t->kind == Kind::Void; }

    // True if the type is an error placeholder.
    bool is_error(TypePtr t) const { return t && t->kind == Kind::Error; }

    // ---- Rendering -----------------------------------------------------
    //
    // Render a type as a human-readable string. Used by diagnostics
    // and by the pretty-printer.
    std::string render(TypePtr t) const;

    // Render the LLVM IR type name for this type. Used by the LLVM
    // emitter.
    std::string render_llvm(TypePtr t) const;

    Arena&       arena()       { return arena_; }
    InternTable& intern()      { return intern_; }

private:
    Arena&        arena_;
    InternTable&  intern_;

    // Cached primitive types.
    TypePtr i8_ = nullptr, i16_ = nullptr, i32_ = nullptr, i64_ = nullptr;
    TypePtr u8_ = nullptr, u16_ = nullptr, u32_ = nullptr, u64_ = nullptr;
    TypePtr f32_ = nullptr, f64_ = nullptr;
    TypePtr bool_ = nullptr;
    TypePtr void_ = nullptr;
    TypePtr error_ = nullptr;

    // Interning caches for composite types. Keyed by a hash of the
    // type's structure. These are per-TypeContext (not static) so
    // that destroying the Arena invalidates them safely.
    std::unordered_map<uint64_t, TypePtr> ref_cache_;
    std::unordered_map<uint64_t, TypePtr> ptr_cache_;

    void install_primitives();
};

// ---- Common type predicates -------------------------------------------

// True if `from` can be implicitly converted to `to` (e.g. i8 -> i32
// widening, or ref T -> ref T in the same region). v0.2 only allows
// trivial conversions: same type, or infer/error placeholders.
bool is_assignable(TypePtr from, TypePtr to);

// True if the two types are structurally identical.
bool same_type(TypePtr a, TypePtr b);

} // namespace tether::type
