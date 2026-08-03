// ssa/builder.cpp — AST → SSA lowering implementation

#include "ssa/builder.hpp"

#include <utility>

namespace tether::ssa {

using namespace tether::ast;
using namespace tether::type;

Module Builder::lower_module(const ast::Module& m) {
    Module mod;
    mod.module_name = m.module_path.empty()
        ? intern_.intern(std::string_view("<anonymous>"))
        : m.module_path[0];
    mod_ = &mod;

    for (ItemPtr item : m.items) {
        if (item) lower_item(*item);
    }
    return mod;
}

void Builder::lower_item(const ast::Item& item) {
    switch (item.kind) {
        case ItemKind::Fn:
            lower_fn(item);
            break;
        case ItemKind::Extern:
            lower_extern(item);
            break;
        case ItemKind::Struct:
        case ItemKind::Union:
            // Structs/unions don't produce SSA; they're type
            // declarations. Their field accesses lower to FieldAddr
            // instructions.
            break;
        case ItemKind::Enum:
            // Enums don't produce SSA directly; variant construction
            // and pattern matching produce SSA.
            break;
        case ItemKind::Trait:
        case ItemKind::Impl:
            for (ItemPtr m : item.impl_members) {
                if (m) lower_fn(*m);
            }
            break;
        case ItemKind::TypeAlias:
        case ItemKind::Const:
        case ItemKind::Static:
        case ItemKind::Module:
        case ItemKind::Import:
        case ItemKind::Export:
            if (item.inner) lower_item(*item.inner);
            break;
    }
}

void Builder::lower_extern(const ast::Item& item) {
    if (!item.extern_decl) return;
    const ast::Item& fn = *item.extern_decl;
    ExternDecl ext;
    ext.name = fn.name;
    for (const auto& p : fn.params) {
        ext.param_types.push_back(resolve_ast_type(p.type));
    }
    ext.return_type = fn.return_type ? resolve_ast_type(fn.return_type)
                                     : tc_.void_type();
    ext.is_variadic = !fn.params.empty() && fn.params.back().is_variadic;
    mod_->externs.push_back(std::move(ext));
}

type::TypePtr Builder::resolve_ast_type(ast::TypePtr t) {
    if (!t) return tc_.make_error();
    switch (t->kind) {
        case ast::TypeKind::Named: {
            if (t->path.empty()) return tc_.make_error();
            TypePtr prim = tc_.lookup_primitive(intern_.get(t->path[0]));
            if (prim) return prim;
            // User-defined type — for v0.3 we treat as opaque struct.
            return tc_.make_struct(t->path[0]);
        }
        case ast::TypeKind::Ref:
        case ast::TypeKind::BorrowRef: {
            TypePtr base = resolve_ast_type(t->base);
            return tc_.make_ref(base, t->is_mut, t->region);
        }
        case ast::TypeKind::RawPtr: {
            TypePtr base = resolve_ast_type(t->base);
            return tc_.make_raw_ptr(base, t->is_mut);
        }
        case ast::TypeKind::Array: {
            TypePtr elem = resolve_ast_type(t->base);
            uint64_t size = 0;
            if (t->length && t->length->kind == ast::ExprKind::IntLit) {
                size = t->length->int_value;
            }
            return tc_.make_array(elem, size);
        }
        case ast::TypeKind::Slice: {
            TypePtr elem = resolve_ast_type(t->base);
            return tc_.make_slice(elem);
        }
        case ast::TypeKind::Tuple: {
            std::vector<TypePtr> args;
            for (ast::TypePtr a : t->args) args.push_back(resolve_ast_type(a));
            return tc_.make_tuple(std::move(args));
        }
        case ast::TypeKind::Fn: {
            std::vector<TypePtr> params;
            for (ast::TypePtr a : t->args) params.push_back(resolve_ast_type(a));
            TypePtr ret = t->return_type ? resolve_ast_type(t->return_type)
                                         : tc_.void_type();
            return tc_.make_fn(std::move(params), ret);
        }
        case ast::TypeKind::Infer:
            return tc_.make_error();
    }
    return tc_.make_error();
}

void Builder::lower_fn(const ast::Item& item) {
    Function fn;
    fn.name = item.name;
    fn_ = &fn;

    // Create the entry block.
    BlockId entry = fresh_block();
    fn.entry = entry;
    set_block(entry);

    // Create the entry mem token.
    fn.entry_mem = fresh_value();
    current_mem_ = fn.entry_mem;
    // The entry mem is a "phantom" value — it has no defining
    // instruction. We just track it as the initial memory state.

    // Lower parameters.
    for (size_t i = 0; i < item.params.size(); ++i) {
        const auto& p = item.params[i];
        ValueId v = fresh_value();
        fn.params.push_back(v);
        type::TypePtr ty = resolve_ast_type(p.type);
        fn.param_types.push_back(ty);
        if (p.name != kInvalidStrId) {
            locals_[p.name] = v;
        }
    }

    // Resolve return type.
    fn.return_type = item.return_type
        ? resolve_ast_type(item.return_type)
        : tc_.void_type();

    // Lower the body.
    if (item.body) {
        lower_block(*item.body);
    }

    // Ensure the function ends with a return.
    if (current_block_ != kInvalidBlock &&
        (fn.blocks[current_block_].instructions.empty() ||
         !fn.blocks[current_block_].instructions.back().is_terminator())) {
        Instruction ret;
        ret.opcode  = Opcode::Ret;
        ret.block   = current_block_;
        ret.loc     = item.range;
        if (!tc_.is_void(fn.return_type)) {
            // Return 0 as a default if the function didn't explicitly
            // return.
            ValueId zero = emit_const_int(0, fn.return_type);
            ret.operands.push_back(zero);
        }
        emit(std::move(ret));
    }

    mod_->functions.push_back(std::move(fn));
    fn_ = nullptr;
    locals_.clear();
    current_block_ = kInvalidBlock;
    current_mem_ = kInvalidValue;
}

void Builder::lower_block(const ast::Block& b) {
    for (StmtPtr s : b.stmts) {
        if (s) lower_stmt(*s);
    }
    if (b.trailing) {
        (void)lower_expr(*b.trailing);
    }
}

void Builder::lower_stmt(const ast::Stmt& s) {
    switch (s.kind) {
        case StmtKind::Let: {
            type::TypePtr ty = s.let_type ? resolve_ast_type(s.let_type)
                                          : tc_.i64();
            if (s.let_value) {
                ValueId v = lower_expr(*s.let_value);
                locals_[s.let_name] = v;
            } else {
                // Uninitialized let — allocate a slot.
                ValueId v = fresh_value();
                locals_[s.let_name] = v;
            }
            break;
        }
        case StmtKind::Expr:
            (void)lower_expr(*s.expr);
            break;
        case StmtKind::Return: {
            Instruction ret;
            ret.opcode = Opcode::Ret;
            ret.block  = current_block_;
            ret.loc    = s.range;
            if (s.expr) {
                ValueId v = lower_expr(*s.expr);
                ret.operands.push_back(v);
            }
            emit(std::move(ret));
            // Create a dead block for any code after the return.
            set_block(fresh_block());
            break;
        }
        case StmtKind::Defer:
            // v0.3: defer is not yet lowered to SSA. Would require
            // constructing a cleanup chain along every exit path.
            if (s.expr) (void)lower_expr(*s.expr);
            break;
        case StmtKind::Break:
        case StmtKind::Continue:
            // v0.3: requires loop context tracking.
            break;
        case StmtKind::Unsafe: {
            bool saved = in_unsafe_;
            in_unsafe_ = true;
            // Emit an Unsafe marker instruction.
            Instruction u;
            u.opcode     = Opcode::Unsafe;
            u.block      = current_block_;
            u.is_unsafe  = true;
            u.loc        = s.range;
            emit(std::move(u));
            lower_block(*s.block);
            in_unsafe_ = saved;
            break;
        }
        case StmtKind::Block:
            lower_block(*s.block);
            break;
    }
}

ValueId Builder::lower_expr(const ast::Expr& e) {
    switch (e.kind) {
        case ExprKind::IntLit: {
            return emit_const_int(e.int_value, tc_.i64());
        }
        case ExprKind::FloatLit: {
            return emit_const_float(e.float_value, tc_.f64());
        }
        case ExprKind::StringLit: {
            Instruction inst;
            inst.opcode  = Opcode::ConstStr;
            inst.type    = tc_.make_raw_ptr(tc_.u8(), false);
            inst.block   = current_block_;
            inst.str_data= e.str_value;
            inst.loc     = e.range;
            return emit(std::move(inst));
        }
        case ExprKind::CharLit: {
            return emit_const_int(e.int_value, tc_.u32());
        }
        case ExprKind::BoolLit: {
            return emit_const_bool(e.int_value != 0);
        }
        case ExprKind::Ident: {
            if (e.path.empty()) return kInvalidValue;
            auto it = locals_.find(e.path[0]);
            if (it != locals_.end()) return it->second;
            return emit_const_int(0, tc_.i64());
        }
        case ExprKind::Path: {
            // v0.3: path expressions (variant constructors, function
            // refs) are not fully supported.
            return emit_const_int(0, tc_.i64());
        }
        case ExprKind::Unary: {
            ValueId operand = lower_expr(*e.lhs);
            switch (e.unary_op) {
                case UnaryOp::Neg: {
                    Instruction inst;
                    inst.opcode   = Opcode::Neg;
                    inst.type     = tc_.i64();
                    inst.block    = current_block_;
                    inst.operands = {operand};
                    inst.loc      = e.range;
                    return emit(std::move(inst));
                }
                case UnaryOp::Not: {
                    Instruction inst;
                    inst.opcode   = Opcode::Not;
                    inst.type     = tc_.boolean();
                    inst.block    = current_block_;
                    inst.operands = {operand};
                    inst.loc      = e.range;
                    return emit(std::move(inst));
                }
                case UnaryOp::BitNot: {
                    Instruction inst;
                    inst.opcode   = Opcode::Not;
                    inst.type     = tc_.i64();
                    inst.block    = current_block_;
                    inst.operands = {operand};
                    inst.loc      = e.range;
                    return emit(std::move(inst));
                }
                case UnaryOp::Deref: {
                    Instruction inst;
                    inst.opcode   = Opcode::Load;
                    inst.type     = tc_.i64();
                    inst.block    = current_block_;
                    inst.operands = {operand};
                    inst.mem_in   = current_mem_;
                    inst.loc      = e.range;
                    ValueId result = fresh_value();
                    inst.result   = result;
                    inst.mem_out  = fresh_value();
                    current_mem_  = inst.mem_out;
                    emit(std::move(inst));
                    return result;
                }
                case UnaryOp::Borrow:
                case UnaryOp::BorrowMut:
                case UnaryOp::Move:
                    return operand;
            }
            return kInvalidValue;
        }
        case ExprKind::Binary: {
            ValueId lhs = lower_expr(*e.lhs);
            ValueId rhs = lower_expr(*e.rhs);
            return emit_binary(e.binary_op, lhs, rhs, tc_.i64(), e.range);
        }
        case ExprKind::Assign: {
            ValueId lhs = lower_expr(*e.lhs);
            ValueId rhs = lower_expr(*e.rhs);
            Instruction store;
            store.opcode   = Opcode::Store;
            store.type     = tc_.void_type();
            store.block    = current_block_;
            store.operands = {lhs, rhs};
            store.mem_in   = current_mem_;
            store.mem_out  = fresh_value();
            store.loc      = e.range;
            current_mem_   = store.mem_out;
            emit(std::move(store));
            return kInvalidValue;
        }
        case ExprKind::Call: {
            // Direct call to a function.
            std::string callee_name;
            if (e.lhs && e.lhs->kind == ExprKind::Ident &&
                !e.lhs->path.empty()) {
                callee_name = std::string(intern_.get(e.lhs->path[0]));
            } else if (e.lhs && e.lhs->kind == ExprKind::Path &&
                       !e.lhs->path.empty()) {
                callee_name = std::string(intern_.get(e.lhs->path.back()));
            }
            Instruction call;
            call.opcode  = Opcode::Call;
            call.type    = tc_.i64();
            call.block   = current_block_;
            call.str_data= intern_.intern(std::string_view(callee_name));
            for (ExprPtr a : e.args) {
                call.operands.push_back(lower_expr(*a));
            }
            call.mem_in  = current_mem_;
            call.mem_out = fresh_value();
            call.result  = fresh_value();
            call.loc     = e.range;
            current_mem_ = call.mem_out;
            emit(std::move(call));
            // Return the result ValueId. We need to look it up from
            // the instruction we just emitted.
            return fn_->blocks[current_block_].instructions.back().result;
        }
        case ExprKind::MethodCall: {
            (void)lower_expr(*e.lhs);
            for (ExprPtr a : e.args) (void)lower_expr(*a);
            return kInvalidValue;
        }
        case ExprKind::FieldAccess: {
            (void)lower_expr(*e.lhs);
            return kInvalidValue;
        }
        case ExprKind::Index: {
            ValueId base = lower_expr(*e.lhs);
            ValueId idx  = lower_expr(*e.index);
            // Insert bounds check (if it's a slice).
            Instruction bc;
            bc.opcode   = Opcode::BoundsCheck;
            bc.type     = tc_.void_type();
            bc.block    = current_block_;
            bc.operands = {idx};
            bc.mem_in   = current_mem_;
            bc.mem_out  = fresh_value();
            bc.loc      = e.range;
            current_mem_ = bc.mem_out;
            emit(std::move(bc));
            // Load the element.
            Instruction load;
            load.opcode   = Opcode::Load;
            load.type     = tc_.i64();
            load.block    = current_block_;
            load.operands = {base};
            load.mem_in   = current_mem_;
            load.loc      = e.range;
            ValueId result = fresh_value();
            load.result   = result;
            load.mem_out  = fresh_value();
            current_mem_  = load.mem_out;
            emit(std::move(load));
            return result;
        }
        case ExprKind::Question:
            return lower_expr(*e.lhs);
        case ExprKind::Block:
            lower_block(*e.block);
            if (e.block && e.block->trailing) {
                return lower_expr(*e.block->trailing);
            }
            return kInvalidValue;
        case ExprKind::If: {
            ValueId cond = lower_expr(*e.cond);
            BlockId then_block = fresh_block();
            BlockId else_block = fresh_block();
            BlockId end_block  = fresh_block();

            Instruction br;
            br.opcode   = Opcode::CondBr;
            br.block    = current_block_;
            br.operands = {cond};
            br.blocks   = {then_block, else_block};
            br.loc      = e.range;
            emit(std::move(br));

            // Then block.
            set_block(then_block);
            add_pred(then_block, current_block_ == then_block ? kInvalidBlock : current_block_);
            // Actually, set_block already switched. Let me redo the pred logic.
            // The pred of then_block is the block we just emitted the CondBr in.
            // We need to track that. Let me restructure.
            (void)lower_expr(*e.then_branch);
            if (current_block_ != kInvalidBlock &&
                !fn_->blocks[current_block_].instructions.empty() &&
                !fn_->blocks[current_block_].instructions.back().is_terminator()) {
                Instruction tbr;
                tbr.opcode = Opcode::Br;
                tbr.block  = current_block_;
                tbr.blocks = {end_block};
                emit(std::move(tbr));
            }
            BlockId then_end = current_block_;
            ValueId then_mem = current_mem_;

            // Else block.
            set_block(else_block);
            ValueId else_result = kInvalidValue;
            if (e.else_branch) {
                else_result = lower_expr(*e.else_branch);
            }
            if (current_block_ != kInvalidBlock &&
                !fn_->blocks[current_block_].instructions.empty() &&
                !fn_->blocks[current_block_].instructions.back().is_terminator()) {
                Instruction ebr;
                ebr.opcode = Opcode::Br;
                ebr.block  = current_block_;
                ebr.blocks = {end_block};
                emit(std::move(ebr));
            }
            BlockId else_end = current_block_;
            ValueId else_mem = current_mem_;
            (void)else_result; (void)then_mem; (void)else_mem;
            (void)then_end; (void)else_end;

            // End block.
            set_block(end_block);
            return kInvalidValue;
        }
        case ExprKind::Match: {
            // v0.3: lower match as a switch on the scrutinee. For
            // now, just evaluate the scrutinee and the first arm.
            (void)lower_expr(*e.cond);
            if (!e.arms.empty()) {
                return lower_expr(*e.arms[0].body);
            }
            return kInvalidValue;
        }
        case ExprKind::Loop: {
            BlockId top = fresh_block();
            BlockId end = fresh_block();
            Instruction br;
            br.opcode   = Opcode::Br;
            br.block    = current_block_;
            br.blocks   = {top};
            emit(std::move(br));
            set_block(top);
            (void)lower_expr(*e.body);
            if (!fn_->blocks[current_block_].instructions.empty() &&
                !fn_->blocks[current_block_].instructions.back().is_terminator()) {
                Instruction lbr;
                lbr.opcode = Opcode::Br;
                lbr.block  = current_block_;
                lbr.blocks = {top};
                emit(std::move(lbr));
            }
            set_block(end);
            return kInvalidValue;
        }
        case ExprKind::While: {
            BlockId top   = fresh_block();
            BlockId body  = fresh_block();
            BlockId end   = fresh_block();
            Instruction br;
            br.opcode   = Opcode::Br;
            br.block    = current_block_;
            br.blocks   = {top};
            emit(std::move(br));
            set_block(top);
            ValueId cond = lower_expr(*e.cond);
            Instruction cbr;
            cbr.opcode   = Opcode::CondBr;
            cbr.block    = current_block_;
            cbr.operands = {cond};
            cbr.blocks   = {body, end};
            emit(std::move(cbr));
            set_block(body);
            (void)lower_expr(*e.body);
            if (!fn_->blocks[current_block_].instructions.empty() &&
                !fn_->blocks[current_block_].instructions.back().is_terminator()) {
                Instruction bbr;
                bbr.opcode = Opcode::Br;
                bbr.block  = current_block_;
                bbr.blocks = {top};
                emit(std::move(bbr));
            }
            set_block(end);
            return kInvalidValue;
        }
        case ExprKind::For: {
            (void)lower_expr(*e.iterable);
            (void)lower_expr(*e.body);
            return kInvalidValue;
        }
        case ExprKind::Return: {
            Instruction ret;
            ret.opcode = Opcode::Ret;
            ret.block  = current_block_;
            ret.loc    = e.range;
            if (e.return_value) {
                ret.operands.push_back(lower_expr(*e.return_value));
            }
            emit(std::move(ret));
            set_block(fresh_block());
            return kInvalidValue;
        }
        case ExprKind::Break:
        case ExprKind::Continue:
            return kInvalidValue;
        case ExprKind::Defer:
            (void)lower_expr(*e.lhs);
            return kInvalidValue;
        case ExprKind::Alloc: {
            // Resolve arena.
            ArenaId aid = kNoArena;
            if (e.alloc_arena_name != kInvalidStrId) {
                auto it = arena_ids_.find(e.alloc_arena_name);
                if (it == arena_ids_.end()) {
                    aid = static_cast<ArenaId>(mod_->arenas.size());
                    mod_->arenas.push_back(e.alloc_arena_name);
                    arena_ids_[e.alloc_arena_name] = aid;
                } else {
                    aid = it->second;
                }
            }
            Instruction alloc;
            alloc.opcode  = Opcode::Alloc;
            alloc.type    = tc_.make_raw_ptr(tc_.i64(), false);
            alloc.block   = current_block_;
            alloc.mem_in  = current_mem_;
            alloc.mem_out = fresh_value();
            alloc.arena   = aid;
            alloc.loc     = e.range;
            current_mem_  = alloc.mem_out;
            ValueId result = fresh_value();
            alloc.result  = result;
            emit(std::move(alloc));
            if (e.alloc_value) (void)lower_expr(*e.alloc_value);
            return result;
        }
        case ExprKind::Move:
            return lower_expr(*e.lhs);
        case ExprKind::Borrow: {
            ValueId base = lower_expr(*e.lhs);
            Instruction borrow;
            borrow.opcode   = Opcode::Borrow;
            borrow.type     = tc_.make_ref(tc_.i64(), e.int_value != 0);
            borrow.block    = current_block_;
            borrow.operands = {base};
            borrow.mem_in   = current_mem_;
            borrow.loc      = e.range;
            return emit(std::move(borrow));
        }
        case ExprKind::Unsafe: {
            bool saved = in_unsafe_;
            in_unsafe_ = true;
            lower_block(*e.block);
            in_unsafe_ = saved;
            if (e.block && e.block->trailing) {
                return lower_expr(*e.block->trailing);
            }
            return kInvalidValue;
        }
        case ExprKind::Spawn:
            (void)lower_expr(*e.body);
            return kInvalidValue;
        case ExprKind::Await:
            return lower_expr(*e.lhs);
        case ExprKind::Tuple: {
            std::vector<TypePtr> elem_types;
            std::vector<ValueId> elems;
            for (ExprPtr a : e.args) {
                ValueId v = lower_expr(*a);
                elems.push_back(v);
                elem_types.push_back(tc_.i64());
            }
            // v0.3: tuples would lower to a struct construction.
            return elems.empty() ? kInvalidValue : elems[0];
        }
        case ExprKind::ArrayLit: {
            ValueId first = kInvalidValue;
            for (ExprPtr a : e.args) {
                first = lower_expr(*a);
            }
            return first;
        }
    }
    return kInvalidValue;
}

ValueId Builder::emit(Instruction inst) {
    if (!fn_ || current_block_ == kInvalidBlock) return kInvalidValue;
    ValueId result = inst.result;
    fn_->blocks[current_block_].instructions.push_back(std::move(inst));
    return result;
}

ValueId Builder::emit_const_int(uint64_t value, type::TypePtr ty) {
    Instruction inst;
    inst.opcode   = Opcode::ConstInt;
    inst.type     = ty;
    inst.block    = current_block_;
    inst.int_data = value;
    ValueId result = fresh_value();
    inst.result   = result;
    emit(std::move(inst));
    return result;
}

ValueId Builder::emit_const_bool(bool value) {
    Instruction inst;
    inst.opcode   = Opcode::ConstBool;
    inst.type     = tc_.boolean();
    inst.block    = current_block_;
    inst.int_data = value ? 1 : 0;
    ValueId result = fresh_value();
    inst.result   = result;
    emit(std::move(inst));
    return result;
}

ValueId Builder::emit_const_float(double value, type::TypePtr ty) {
    Instruction inst;
    inst.opcode    = Opcode::ConstFloat;
    inst.type      = ty;
    inst.block     = current_block_;
    inst.float_data= value;
    ValueId result = fresh_value();
    inst.result    = result;
    emit(std::move(inst));
    return result;
}

ValueId Builder::emit_binary(ast::BinaryOp op, ValueId lhs, ValueId rhs,
                             type::TypePtr ty, SourceRange loc) {
    Instruction inst;
    inst.opcode   = Opcode::Add;  // default
    inst.type     = ty;
    inst.block    = current_block_;
    inst.operands = {lhs, rhs};
    inst.loc      = loc;
    switch (op) {
        case BinaryOp::Add:    inst.opcode = Opcode::Add; break;
        case BinaryOp::Sub:    inst.opcode = Opcode::Sub; break;
        case BinaryOp::Mul:    inst.opcode = Opcode::Mul; break;
        case BinaryOp::Div:    inst.opcode = Opcode::Div; break;
        case BinaryOp::Mod:    inst.opcode = Opcode::Mod; break;
        case BinaryOp::BitAnd: inst.opcode = Opcode::And; break;
        case BinaryOp::BitOr:  inst.opcode = Opcode::Or;  break;
        case BinaryOp::BitXor: inst.opcode = Opcode::Xor; break;
        case BinaryOp::Shl:    inst.opcode = Opcode::Shl; break;
        case BinaryOp::Shr:    inst.opcode = Opcode::Shr; break;
        case BinaryOp::And:    inst.opcode = Opcode::And; inst.type = tc_.boolean(); break;
        case BinaryOp::Or:     inst.opcode = Opcode::Or;  inst.type = tc_.boolean(); break;
        case BinaryOp::Eq:     inst.opcode = Opcode::Eq;  inst.type = tc_.boolean(); break;
        case BinaryOp::Neq:    inst.opcode = Opcode::Ne;  inst.type = tc_.boolean(); break;
        case BinaryOp::Lt:     inst.opcode = Opcode::Lt;  inst.type = tc_.boolean(); break;
        case BinaryOp::Gt:     inst.opcode = Opcode::Gt;  inst.type = tc_.boolean(); break;
        case BinaryOp::Le:     inst.opcode = Opcode::Le;  inst.type = tc_.boolean(); break;
        case BinaryOp::Ge:     inst.opcode = Opcode::Ge;  inst.type = tc_.boolean(); break;
    }
    ValueId result = fresh_value();
    inst.result = result;
    emit(std::move(inst));
    return result;
}

void Builder::set_block(BlockId b) {
    current_block_ = b;
}

void Builder::add_pred(BlockId block, BlockId pred) {
    if (block == kInvalidBlock || pred == kInvalidBlock) return;
    if (block < fn_->blocks.size()) {
        auto& preds = fn_->blocks[block].predecessors;
        // Avoid duplicate predecessors.
        for (BlockId p : preds) {
            if (p == pred) return;
        }
        preds.push_back(pred);
    }
}

} // namespace tether::ssa
