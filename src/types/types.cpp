// types/types.cpp — type system implementation

#include "types/types.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tether::type {

namespace {

// Hash key for type interning.
struct TypeKey {
    Kind     kind;
    uint16_t bit_width;
    bool     is_signed;
    bool     is_mut;
    RegionId region;
    TypePtr  base;
    TypePtr  return_type;
    std::vector<TypePtr> args;
    StrId    name;
    uint64_t array_size;
    uint32_t var_id;

    bool operator==(const TypeKey& o) const {
        return kind == o.kind && bit_width == o.bit_width &&
               is_signed == o.is_signed && is_mut == o.is_mut &&
               region == o.region && base == o.base &&
               return_type == o.return_type && args == o.args &&
               name == o.name && array_size == o.array_size &&
               var_id == o.var_id;
    }
};

struct TypeKeyHash {
    size_t operator()(const TypeKey& k) const {
        size_t h = static_cast<size_t>(k.kind);
        h = h * 31 + k.bit_width;
        h = h * 31 + (k.is_signed ? 1 : 0);
        h = h * 31 + (k.is_mut ? 1 : 0);
        h = h * 31 + k.region;
        h = h * 31 + reinterpret_cast<uintptr_t>(k.base);
        h = h * 31 + reinterpret_cast<uintptr_t>(k.return_type);
        for (auto a : k.args) {
            h = h * 31 + reinterpret_cast<uintptr_t>(a);
        }
        h = h * 31 + k.name;
        h = h * 31 + static_cast<size_t>(k.array_size);
        h = h * 31 + k.var_id;
        return h;
    }
};

} // namespace

void TypeContext::install_primitives() {
    i8_   = make_int(8,   true);
    i16_  = make_int(16,  true);
    i32_  = make_int(32,  true);
    i64_  = make_int(64,  true);
    u8_   = make_int(8,   false);
    u16_  = make_int(16,  false);
    u32_  = make_int(32,  false);
    u64_  = make_int(64,  false);
    f32_  = make_float(32);
    f64_  = make_float(64);
    bool_ = make_bool();
    void_ = make_void();
    error_ = make_error();
}

TypePtr TypeContext::make_int(uint16_t bits, bool signed_) {
    // Primitives are cached in install_primitives(); for non-standard
    // widths we allocate fresh. Equality is structural via
    // is_assignable, so this is correct but may use extra memory for
    // exotic widths.
    Type t;
    t.kind       = Kind::Int;
    t.bit_width  = bits;
    t.is_signed  = signed_;
    return arena_.construct<Type>(std::move(t));
}

TypePtr TypeContext::make_float(uint16_t bits) {
    Type t;
    t.kind      = Kind::Float;
    t.bit_width = bits;
    return arena_.construct<Type>(std::move(t));
}

TypePtr TypeContext::make_bool() {
    if (bool_) return bool_;
    Type t;
    t.kind = Kind::Bool;
    bool_ = arena_.construct<Type>(std::move(t));
    return bool_;
}

TypePtr TypeContext::make_void() {
    if (void_) return void_;
    Type t;
    t.kind = Kind::Void;
    void_ = arena_.construct<Type>(std::move(t));
    return void_;
}

TypePtr TypeContext::make_ref(TypePtr base, bool is_mut, RegionId region) {
    uint64_t key = (reinterpret_cast<uintptr_t>(base) >> 4) ^
                   (static_cast<uint64_t>(is_mut ? 1 : 0) << 32) ^
                   (static_cast<uint64_t>(region) << 33);
    auto it = ref_cache_.find(key);
    if (it != ref_cache_.end()) return it->second;
    Type t;
    t.kind   = Kind::Ref;
    t.base   = base;
    t.is_mut = is_mut;
    t.region = region;
    TypePtr ptr = arena_.construct<Type>(std::move(t));
    ref_cache_[key] = ptr;
    return ptr;
}

TypePtr TypeContext::make_raw_ptr(TypePtr base, bool is_mut) {
    uint64_t key = (reinterpret_cast<uintptr_t>(base) >> 4) ^
                   (static_cast<uint64_t>(is_mut ? 1 : 0) << 32);
    auto it = ptr_cache_.find(key);
    if (it != ptr_cache_.end()) return it->second;
    Type t;
    t.kind   = Kind::RawPtr;
    t.base   = base;
    t.is_mut = is_mut;
    TypePtr ptr = arena_.construct<Type>(std::move(t));
    ptr_cache_[key] = ptr;
    return ptr;
}

TypePtr TypeContext::make_array(TypePtr elem, uint64_t size) {
    Type t;
    t.kind       = Kind::Array;
    t.base       = elem;
    t.array_size = size;
    return arena_.construct<Type>(std::move(t));
}

TypePtr TypeContext::make_slice(TypePtr elem) {
    Type t;
    t.kind = Kind::Slice;
    t.base = elem;
    return arena_.construct<Type>(std::move(t));
}

TypePtr TypeContext::make_tuple(std::vector<TypePtr> args) {
    Type t;
    t.kind = Kind::Tuple;
    t.args = std::move(args);
    return arena_.construct<Type>(std::move(t));
}

TypePtr TypeContext::make_fn(std::vector<TypePtr> params, TypePtr ret) {
    Type t;
    t.kind         = Kind::Fn;
    t.args         = std::move(params);
    t.return_type  = ret;
    return arena_.construct<Type>(std::move(t));
}

TypePtr TypeContext::make_struct(StrId name, std::vector<TypePtr> args) {
    Type t;
    t.kind = Kind::Struct;
    t.name = name;
    t.args = std::move(args);
    return arena_.construct<Type>(std::move(t));
}

TypePtr TypeContext::make_enum(StrId name, std::vector<TypePtr> args) {
    Type t;
    t.kind = Kind::Enum;
    t.name = name;
    t.args = std::move(args);
    return arena_.construct<Type>(std::move(t));
}

TypePtr TypeContext::make_union(StrId name, std::vector<TypePtr> args) {
    Type t;
    t.kind = Kind::Union;
    t.name = name;
    t.args = std::move(args);
    return arena_.construct<Type>(std::move(t));
}

TypePtr TypeContext::make_trait(StrId name) {
    Type t;
    t.kind = Kind::Trait;
    t.name = name;
    return arena_.construct<Type>(std::move(t));
}

TypePtr TypeContext::make_typevar(uint32_t var_id) {
    Type t;
    t.kind   = Kind::TypeVar;
    t.var_id = var_id;
    return arena_.construct<Type>(std::move(t));
}

TypePtr TypeContext::make_infer(uint32_t var_id) {
    Type t;
    t.kind   = Kind::Infer;
    t.var_id = var_id;
    return arena_.construct<Type>(std::move(t));
}

TypePtr TypeContext::make_error() {
    if (error_) return error_;
    Type t;
    t.kind = Kind::Error;
    error_ = arena_.construct<Type>(std::move(t));
    return error_;
}

TypePtr TypeContext::lookup_primitive(std::string_view name) const {
    if (name == "i8")   return i8_;
    if (name == "i16")  return i16_;
    if (name == "i32")  return i32_;
    if (name == "i64")  return i64_;
    if (name == "u8")   return u8_;
    if (name == "u16")  return u16_;
    if (name == "u32")  return u32_;
    if (name == "u64")  return u64_;
    if (name == "f32")  return f32_;
    if (name == "f64")  return f64_;
    if (name == "bool") return bool_;
    if (name == "void") return void_;
    if (name == "()" )  return void_;
    return nullptr;
}

std::string TypeContext::render(TypePtr t) const {
    if (!t) return "<null>";
    switch (t->kind) {
        case Kind::Int:
            return std::string(t->is_signed ? "i" : "u") +
                   std::to_string(t->bit_width);
        case Kind::Float:
            return std::string("f") + std::to_string(t->bit_width);
        case Kind::Bool:  return "bool";
        case Kind::Void:  return "void";
        case Kind::Ref:
            return std::string(t->is_mut ? "mut ref " : "ref ") +
                   render(t->base);
        case Kind::RawPtr:
            return std::string(t->is_mut ? "*mut " : "*const ") +
                   render(t->base);
        case Kind::Array:
            return "[" + render(t->base) + "; " +
                   std::to_string(t->array_size) + "]";
        case Kind::Slice:
            return "[" + render(t->base) + "]";
        case Kind::Tuple: {
            if (t->args.empty()) return "()";
            std::string s = "(";
            for (size_t i = 0; i < t->args.size(); ++i) {
                if (i) s += ", ";
                s += render(t->args[i]);
            }
            s += ")";
            return s;
        }
        case Kind::Fn: {
            std::string s = "fn(";
            for (size_t i = 0; i < t->args.size(); ++i) {
                if (i) s += ", ";
                s += render(t->args[i]);
            }
            s += ") -> ";
            s += t->return_type ? render(t->return_type) : "void";
            return s;
        }
        case Kind::Struct:
        case Kind::Enum:
        case Kind::Union:
        case Kind::Trait: {
            std::string s = std::string(intern_.get(t->name));
            if (!t->args.empty()) {
                s += "<";
                for (size_t i = 0; i < t->args.size(); ++i) {
                    if (i) s += ", ";
                    s += render(t->args[i]);
                }
                s += ">";
            }
            return s;
        }
        case Kind::TypeVar:
            return std::string("T") + std::to_string(t->var_id);
        case Kind::Infer:
            return std::string("?") + std::to_string(t->var_id);
        case Kind::Error:
            return "<error>";
    }
    return "<unknown>";
}

std::string TypeContext::render_llvm_attrs(TypePtr t) const {
    if (!t) return "";
    switch (t->kind) {
        case Kind::Ref:
            // ref T: noalias (doesn't alias other pointers — borrow
            // checker guarantee), nonnull (refs are never null),
            // readonly (shared ref — can't write through it).
            // mut ref T: noalias, nonnull, but NOT readonly.
            if (t->is_mut) {
                return " noalias nonnull";
            } else {
                return " noalias nonnull readonly";
            }
        case Kind::RawPtr:
            // Raw pointers carry no attributes — they're unsafe and
            // may alias, be null, etc.
            return "";
        default:
            return "";
    }
}

std::string TypeContext::render_llvm(TypePtr t) const {
    if (!t) return "void";
    switch (t->kind) {
        case Kind::Int:
            return "i" + std::to_string(t->bit_width);
        case Kind::Float:
            return t->bit_width == 32 ? "float" : "double";
        case Kind::Bool:
            return "i1";
        case Kind::Void:
            return "void";
        case Kind::Ref:
        case Kind::RawPtr:
            // References and raw pointers both lower to LLVM pointers.
            // References carry a nonnull+nocapture+readonly attribute
            // (mut ref drops readonly); raw pointers carry no attributes.
            return render_llvm(t->base) + "*";
        case Kind::Array: {
            // [N x T]
            return "[" + std::to_string(t->array_size) + " x " +
                   render_llvm(t->base) + "]";
        }
        case Kind::Slice: {
            // Slices lower to a struct { ptr, len } — for v0.2 we emit
            // a pointer to the element type. A proper slice would
            // lower to { T*, i64 } but that requires struct types,
            // which the SSA layer will handle when it lands.
            return render_llvm(t->base) + "*";
        }
        case Kind::Tuple: {
            if (t->args.empty()) return "void";
            // { T, U, V }
            std::string s = "{ ";
            for (size_t i = 0; i < t->args.size(); ++i) {
                if (i) s += ", ";
                s += render_llvm(t->args[i]);
            }
            s += " }";
            return s;
        }
        case Kind::Fn: {
            // LLVM function pointer type: ret (T, U, V)*
            std::string ret = t->return_type ? render_llvm(t->return_type) : "void";
            std::string s = ret + " (";
            for (size_t i = 0; i < t->args.size(); ++i) {
                if (i) s += ", ";
                s += render_llvm(t->args[i]);
            }
            s += ")*";
            return s;
        }
        case Kind::Struct:
        case Kind::Enum:
        case Kind::Union:
        case Kind::Trait:
            // User-defined types lower to opaque structs identified by
            // their mangled name. The emitter will define them as
            // %struct.Name = type { ... } at module scope.
            return std::string("%struct.") + std::string(intern_.get(t->name));
        case Kind::TypeVar:
        case Kind::Infer:
            // These should have been monomorphized / solved before
            // LLVM emission. If we see them here, it's a compiler bug.
            return "; <infer-or-typevar " + render(t) + ">";
        case Kind::Error:
            return "; <type-error>";
    }
    return "void";
}

bool is_assignable(TypePtr from, TypePtr to) {
    if (!from || !to) return false;
    if (from == to) return true;
    // Error type is assignable to/from anything (suppresses cascading
    // errors).
    if (from->kind == Kind::Error || to->kind == Kind::Error) return true;
    // Infer types are solved during checking; if we see them here,
    // treat as assignable (the solver will have already errored if
    // unsolvable).
    if (from->kind == Kind::Infer || to->kind == Kind::Infer) return true;
    // Same structure: recurse.
    if (from->kind != to->kind) return false;
    switch (from->kind) {
        case Kind::Int:
            return from->bit_width == to->bit_width &&
                   from->is_signed == to->is_signed;
        case Kind::Float:
            return from->bit_width == to->bit_width;
        case Kind::Ref:
        case Kind::RawPtr:
            return from->is_mut == to->is_mut &&
                   is_assignable(from->base, to->base);
        case Kind::Array:
            return from->array_size == to->array_size &&
                   is_assignable(from->base, to->base);
        case Kind::Slice:
        case Kind::Bool:
        case Kind::Void:
            return true;
        case Kind::Tuple:
        case Kind::Fn: {
            if (from->args.size() != to->args.size()) return false;
            for (size_t i = 0; i < from->args.size(); ++i) {
                if (!is_assignable(from->args[i], to->args[i])) return false;
            }
            if (from->kind == Kind::Fn) {
                return is_assignable(from->return_type, to->return_type);
            }
            return true;
        }
        case Kind::Struct:
        case Kind::Enum:
        case Kind::Union:
        case Kind::Trait:
            return from->name == to->name && from->args == to->args;
        case Kind::TypeVar:
            return from->var_id == to->var_id;
        case Kind::Infer:
        case Kind::Error:
            return true;
    }
    return false;
}

bool same_type(TypePtr a, TypePtr b) {
    return a == b || is_assignable(a, b);
}

} // namespace tether::type
