#include "core/sema/sema.h"

#include <cmath>
#include <cstdio>

namespace jules {

namespace {

constexpr u64 kComptimeStepLimit = 1'000'000; // halts runaway comptime loops

struct Value {
    TypeId ty = ty_none();
    u64 iv = 0;
    f64 fv = 0;
};

bool val_is_fp(const Value& v) { return ty_is_float(v.ty); }

class Sema {
public:
    Sema(ModuleAst& mod, SemaModule& out, Diagnostics& diag, SymbolTable& syms)
        : mod_(mod), out_(out), diag_(diag), syms_(syms) {}

    bool run() {
        register_user_types();
        register_traits();
        move_impl_methods();
        declare_globals();
        // The methods table records mod_.fns (ast) indices; externs are not
        // registered in out_.fns, so out indices shift. Translate once.
        for (size_t i = 0; i < out_.fns.size(); ++i)
            ast_to_out_.insert(out_.fns[i].ast_index, i);
        for (FnDecl& fn : mod_.fns) check_fn(fn);
        check_trait_impl_signatures();
        for (FnDecl& fn : mod_.fns)
            if (!fn.is_extern) run_escape_analysis(fn);
        if (!out_.has_main && !diag_.has_errors()) {
            SourcePos pos = mod_.fns.empty() ? SourcePos{} : mod_.fns[0].pos;
            diag_.error(pos, "no 'fn main' found: an executable entry point is required");
        }
        return !diag_.has_errors();
    }

private:
    // ---- user type registration -----------------------------------------
    // Registration order = declaration order; a field type must already be
    // declared (declare-before-use, single pass, documented MVP rule).
    // Registration follows SOURCE ORDER across kinds: a field/alias target
    // type must already be declared (declare-before-use, single pass).
    void register_user_types() {
        struct Item { u32 line; u8 kind; size_t idx; }; // 0 st 1 en 2 bm 3 bf 4 al
        std::vector<Item> order;
        for (size_t i = 0; i < mod_.structs.size(); ++i)
            order.push_back(Item{mod_.structs[i].pos.line, 0, i});
        for (size_t i = 0; i < mod_.enums.size(); ++i)
            order.push_back(Item{mod_.enums[i].pos.line, 1, i});
        for (size_t i = 0; i < mod_.bitmasks.size(); ++i)
            order.push_back(Item{mod_.bitmasks[i].pos.line, 2, i});
        for (size_t i = 0; i < mod_.bitfields.size(); ++i)
            order.push_back(Item{mod_.bitfields[i].pos.line, 3, i});
        for (size_t i = 0; i < mod_.aliases.size(); ++i)
            order.push_back(Item{mod_.aliases[i].pos.line, 4, i});
        // stable insertion sort by line (ties keep parse order)
        for (size_t i = 1; i < order.size(); ++i) {
            Item key = order[i];
            size_t j = i;
            while (j > 0 && order[j - 1].line > key.line) {
                order[j] = order[j - 1];
                --j;
            }
            order[j] = key;
        }
        for (const Item& it : order) {
            switch (it.kind) {
                case 0: register_struct(mod_.structs[it.idx]); break;
                case 1: register_enum(mod_.enums[it.idx]); break;
                case 2: register_bitmask(mod_.bitmasks[it.idx]); break;
                case 3: register_bitfield(mod_.bitfields[it.idx]); break;
                default: register_alias(mod_.aliases[it.idx]); break;
            }
        }
        compute_struct_layouts();
    }

    void register_struct(StructDecl& sd) {
        if (out_.user_types.by_name.contains(sd.name)) {
            diag_.error(sd.pos, "duplicate type name '" +
                        std::string(syms_.name(sd.name)) + "'");
            return;
        }
        UserType u;
        u.kind = UserKind::Struct;
        u.name = sd.name;
        u.pos = sd.pos;
        // phase 1: publish the name BEFORE resolving fields, so a field of
        // type *mut Self resolves (the classic linked-node shape)
        out_.user_types.types.push_back(std::move(u));
        TypeId self_id =
            user_index_to_type(static_cast<u32>(out_.user_types.types.size() - 1));
        out_.user_types.by_name.insert(sd.name, self_id);
        if (sd.fields.empty()) {
            diag_.error(sd.pos, "struct '" + std::string(syms_.name(sd.name)) +
                        "' has no fields (empty structs are not in the MVP; a "
                        "size-0 type would corrupt every allocation around it)");
            return;
        }
        // Fields resolve into a LOCAL buffer: resolving a field may create a
        // pointer-to-struct type (struct_ptr_of), which grows the types
        // vector and would dangle a reference into it.
        std::vector<StructField> fields;
        for (auto& [fname, fty] : sd.fields) {
            TypeId rt = resolve_type(fty, sd.pos);
            if (rt == ty_none()) continue;
            if (ty_is_user(rt)) {
                bool ok_field = out_.user_types.struct_of(rt) != ty_none() ||
                                out_.user_types.pointee_struct(rt) != ty_none() ||
                                out_.user_types.backing_of(rt) != ty_none() ||
                                folds_to_scalar(rt); // alias to a scalar
                if (!ok_field) {
                    diag_.error(sd.pos, "struct field '" + std::string(syms_.name(fname)) +
                                "' has type '" + user_type_name(rt) +
                                "' — only scalars, pointers, enums/bitmasks/bitfields "
                                "and nested structs are fields");
                }
            } else if (!ty_is_scalar(rt)) {
                diag_.error(sd.pos, "struct field '" + std::string(syms_.name(fname)) +
                            "' has a non-value type");
            }
            StructField f;
            f.name = fname;
            f.ty = rt;
            fields.push_back(f);
        }
        out_.user_types.get_mut(self_id).fields = std::move(fields);
    }

    // Does this user type (through aliases) denote a lattice scalar?
    bool folds_to_scalar(TypeId t) {
        const UserType* u = &out_.user_types.get(t);
        int depth = 0;
        while (u->kind == UserKind::Alias && depth++ < 16) {
            if (!ty_is_user(u->target)) return ty_is_scalar(u->target);
            u = &out_.user_types.get(u->target);
        }
        return false;
    }

    void register_enum(EnumDecl& ed) {
        if (out_.user_types.by_name.contains(ed.name)) {
            diag_.error(ed.pos, "duplicate type name '" +
                        std::string(syms_.name(ed.name)) + "'");
            return;
        }
        UserType u;
        u.kind = UserKind::Enum;
        u.name = ed.name;
        u.pos = ed.pos;
        u.backing = ed.backing;
        u64 next = 0;
        for (auto& [vname, vexpr] : ed.variants) {
            if (vexpr) {
                std::optional<Value> v = eval_const(*vexpr, ty_none());
                if (!v) {
                    diag_.error(ed.pos, "enum variant value must be a compile-time "
                                "integer constant");
                    continue;
                }
                next = v->iv;
            }
            if (!fits_int(next, u.backing)) {
                diag_.error(ed.pos, "enum variant '" + std::string(syms_.name(vname)) +
                            "' value does not fit the " + std::string(ty_name(u.backing)) +
                            " backing");
            }
            u.variants.emplace_back(vname, next);
            ++next;
        }
        out_.user_types.types.push_back(std::move(u));
        out_.user_types.by_name.insert(ed.name,
            user_index_to_type(static_cast<u32>(out_.user_types.types.size() - 1)));
    }

    void register_bitmask(BitmaskDecl& bd) {
        if (out_.user_types.by_name.contains(bd.name)) {
            diag_.error(bd.pos, "duplicate type name '" +
                        std::string(syms_.name(bd.name)) + "'");
            return;
        }
        UserType u;
        u.kind = UserKind::Bitmask;
        u.name = bd.name;
        u.pos = bd.pos;
        u.backing = ty_u64();
        u64 next_bit = 0; // next auto flag = 1 << ordinal
        for (auto& [vname, vexpr] : bd.variants) {
            u64 val;
            if (vexpr) {
                std::optional<Value> v = eval_const(*vexpr, ty_none());
                if (!v) {
                    diag_.error(bd.pos, "bitmask flag value must be a compile-time "
                                "integer constant");
                    continue;
                }
                val = v->iv;
            } else {
                if (next_bit >= 64) {
                    diag_.error(bd.pos, "bitmask has more than 64 auto-numbered flags; "
                                "assign explicit values");
                    continue;
                }
                val = u64(1) << next_bit;
            }
            u.variants.emplace_back(vname, val);
            ++next_bit;
        }
        out_.user_types.types.push_back(std::move(u));
        out_.user_types.by_name.insert(bd.name,
            user_index_to_type(static_cast<u32>(out_.user_types.types.size() - 1)));
    }

    void register_bitfield(BitfieldDecl& bd) {
        if (out_.user_types.by_name.contains(bd.name)) {
            diag_.error(bd.pos, "duplicate type name '" +
                        std::string(syms_.name(bd.name)) + "'");
            return;
        }
        UserType u;
        u.kind = UserKind::Bitfield;
        u.name = bd.name;
        u.pos = bd.pos;
        u.backing = ty_u64();
        u32 shift = 0;
        for (auto& [fname, width] : bd.segs) {
            if (shift + width > 64) {
                diag_.error(bd.pos, "bitfield segments exceed 64 bits");
                break;
            }
            BitSeg seg;
            seg.name = fname;
            seg.width = width;
            seg.shift = shift;
            u.segs.push_back(seg);
            shift += width;
        }
        out_.user_types.types.push_back(std::move(u));
        out_.user_types.by_name.insert(bd.name,
            user_index_to_type(static_cast<u32>(out_.user_types.types.size() - 1)));
    }

    void register_alias(AliasDecl& ad) {
        if (out_.user_types.by_name.contains(ad.name)) {
            diag_.error(ad.pos, "duplicate type name '" +
                        std::string(syms_.name(ad.name)) + "'");
            return;
        }
        TypeId t = resolve_type(ad.target, ad.pos);
        UserType u;
        u.kind = UserKind::Alias;
        u.name = ad.name;
        u.pos = ad.pos;
        u.target = t;
        out_.user_types.types.push_back(std::move(u));
        out_.user_types.by_name.insert(ad.name,
            user_index_to_type(static_cast<u32>(out_.user_types.types.size() - 1)));
    }

    static bool fits_int(u64 v, TypeId t) {
        switch (type_desc(t).ty) {
            case Ty::I32: return v <= static_cast<u64>(INT32_MAX);
            case Ty::U32: return v <= 0xFFFFFFFFull;
            case Ty::I64: return v <= static_cast<u64>(INT64_MAX);
            case Ty::U64: return true;
            default: return false;
        }
    }

    // get-or-create the StructPtr type for a registered struct
    TypeId struct_ptr_of(TypeId struct_ty) {
        TypeId base = out_.user_types.struct_of(struct_ty);
        if (base == ty_none()) return ty_none();
        const UserType& s = out_.user_types.get(base);
        SymbolId pname = syms_.intern("*" + std::string(syms_.name(s.name)));
        if (const TypeId* p = out_.user_types.by_name.find(pname)) return *p;
        UserType u;
        u.kind = UserKind::StructPtr;
        u.name = pname;
        u.pos = s.pos;
        u.target = base;
        u.size = 8;
        u.align = 8;
        out_.user_types.types.push_back(std::move(u));
        TypeId id = user_index_to_type(static_cast<u32>(out_.user_types.types.size() - 1));
        out_.user_types.by_name.insert(pname, id);
        return id;
    }

    // Resolve a parser pending type slot against the registered table.
    TypeId resolve_type(TypeId t, SourcePos pos) {
        if (!ModuleAst::is_pending(t)) return t;

        const TypeSlot& slot = mod_.type_slots[t & ~kPendingTyFlag];
        const TypeId* known = out_.user_types.by_name.find(syms_.intern(slot.name));
        if (slot.is_ptr) {
            if (!known) {
                // pointer to a builtin scalar? (parse only pends unknown names,
                // but alias targets can re-enter here after resolution)
                TypeId scalar = builtin_named_type(slot.name);
                if (scalar == ty_none()) {
                    diag_.error(pos, "unknown type '" + slot.name +
                                "' (types must be declared before use)");
                    return ty_none();
                }
                TypeId p = ty_ptr(scalar);
                if (p == ty_none()) {
                    diag_.error(pos, "pointer to non-scalar type is not in the MVP lattice");
                    return ty_none();
                }
                return p;
            }
            TypeId base = *known;
            if (out_.user_types.struct_of(base) == ty_none()) {
                diag_.error(pos, "pointer to '" + slot.name +
                            "': only pointers to structs and scalars exist "
                            "(integer-backed types use their backing integer)");
                return ty_none();
            }
            return struct_ptr_of(base);
        }
        if (!known) {
            diag_.error(pos, "unknown type '" + slot.name +
                        "' (types must be declared before use)");
            return ty_none();
        }
        return *known;
    }

    static TypeId builtin_named_type(const std::string& n) {
        if (n == "i32") return ty_i32();
        if (n == "i64" || n == "usize") return ty_i64();
        if (n == "u32") return ty_u32();
        if (n == "u64") return ty_u64();
        if (n == "f32") return ty_f32();
        if (n == "f64") return ty_f64();
        if (n == "bool") return ty_i1();
        return ty_none();
    }

    // Layout: sequential, no reordering, natural alignment per field.
    // Recursion through VALUE fields is rejected (infinite size).
    void compute_struct_layouts() {
        for (u32 i = 0; i < out_.user_types.types.size(); ++i) {
            UserType& u = out_.user_types.types[i];
            if (u.kind != UserKind::Struct) continue;
            if (u.size != 0) continue; // laid out via a cycle that resolved
            if (!layout_struct(i, 0)) continue;
        }
    }
    bool layout_struct(u32 idx, int depth) {
        UserType& u = out_.user_types.types[idx];
        if (depth > 64) {
            diag_.error(u.pos, "struct '" + std::string(syms_.name(u.name)) +
                        "' nests too deeply");
            return false;
        }
        if (u.laying_out) { // cycle through value fields
            diag_.error(u.pos, "struct '" + std::string(syms_.name(u.name)) +
                        "' is recursive through value fields (infinite size); "
                        "use '*mut " + std::string(syms_.name(u.name)) + "'");
            return false;
        }
        if (u.size != 0) return true; // already laid out (size >= 1 is valid:
                                      // a 1-byte struct { b: bool } is legal)
        u.laying_out = true;
        u32 off = 0, align = 1;
        bool ok = true;
        for (StructField& f : u.fields) {
            u32 fsz = 0, fal = 1;
            if (ty_is_user(f.ty)) {
                // fold alias chains: a field may be declared through an alias
                TypeId ft = f.ty;
                const UserType* fu = &out_.user_types.get(ft);
                int fdepth = 0;
                while (fu->kind == UserKind::Alias && fdepth++ < 16) {
                    if (!ty_is_user(fu->target)) { ft = fu->target; fu = nullptr; break; }
                    fu = &out_.user_types.get(fu->target);
                }
                if (fu == nullptr) {
                    // alias to a lattice scalar
                    fsz = ty_store_bytes(ft);
                    fal = ty_bits(ft) / 8;
                } else {
                    TypeId base = out_.user_types.struct_of(f.ty);
                    if (base != ty_none()) {
                        UserType& nested = out_.user_types.get_mut(base);
                        if (nested.laying_out) {
                            diag_.error(u.pos, "struct '" +
                                        std::string(syms_.name(u.name)) +
                                        "' is recursive through value fields "
                                        "(infinite size); use '*mut " +
                                        std::string(syms_.name(nested.name)) + "'");
                            ok = false;
                            break;
                        }
                        if (nested.size == 0) layout_struct(base - kUserTyBase, depth + 1);
                        if (nested.size == 0) { ok = false; break; }
                        fsz = nested.size;
                        fal = nested.align;
                    } else if (out_.user_types.pointee_struct(f.ty) != ty_none()) {
                        fsz = 8; fal = 8;
                    } else {
                        TypeId b = out_.user_types.backing_of(f.ty);
                        if (b == ty_none()) { ok = false; break; }
                        fsz = ty_store_bytes(b); fal = ty_bits(b) / 8;
                    }
                }
            } else if (f.ty != ty_none()) {
                fsz = ty_store_bytes(f.ty);
                fal = ty_bits(f.ty) / 8;
            }
            if (fsz == 0) {
                diag_.error(u.pos, "struct '" + std::string(syms_.name(u.name)) +
                            "' field '" + std::string(syms_.name(f.name)) +
                            "' has no layout size (unresolved type)");
                ok = false;
                continue;
            }
            off = (off + fal - 1) & ~(fal - 1);
            f.offset = off;
            f.size = fsz;
            off += fsz;
            align = align > fal ? align : fal;
        }
        u.laying_out = false;
        if (!ok) { u.size = 0; u.align = 1; return false; }
        u.size = off == 0 ? 1 : ((off + align - 1) & ~(align - 1));
        u.align = align;
        return true;
    }

    void register_traits() {
        for (u32 i = 0; i < mod_.traits.size(); ++i) {
            TraitDecl& td = mod_.traits[i];
            if (out_.trait_index.contains(td.name)) {
                diag_.error(td.pos, "duplicate trait '" +
                            std::string(syms_.name(td.name)) + "'");
                continue;
            }
            for (TraitMethod& tm : td.methods) {
                for (auto& [pname, pty] : tm.params) pty = resolve_type(pty, tm.pos);
                tm.ret = resolve_type(tm.ret, tm.pos);
                if (tm.self_kind == 3) {
                    diag_.error(tm.pos, "trait methods take '&self' or '&mut self' "
                                "(by-value receivers are not in the MVP)");
                }
            }
            out_.trait_index.insert(td.name, i);
        }
    }

    // Move impl methods into mod_.fns under mangled names and fill the
    // static-dispatch table. FnId == fns index keeps the driver invariant.
    void move_impl_methods() {
        for (ImplDecl& id : mod_.impls) {
            const TypeId* tptr = out_.user_types.by_name.find(id.type_name);
            if (!tptr) {
                diag_.error(id.pos, "impl for unknown type '" +
                            std::string(syms_.name(id.type_name)) + "'");
                continue;
            }
            TypeId stype = *tptr;
            TypeId base = out_.user_types.struct_of(stype);
            if (base == ty_none()) {
                diag_.error(id.pos, "impl blocks are only for struct types (got '" +
                            user_type_name(stype) + "')");
                continue;
            }
            SymbolId trait_sym = id.trait_name;
            if (trait_sym != kNoSymbol && !out_.trait_index.contains(trait_sym)) {
                diag_.error(id.pos, "impl of unknown trait '" +
                            std::string(syms_.name(trait_sym)) + "'");
                continue;
            }
            std::string tname(syms_.name(out_.user_types.get(base).name));
            for (FnDecl& m : id.methods) {
                // resolve signature
                for (auto& [pname, pty] : m.params) pty = resolve_type(pty, m.pos);
                m.ret = resolve_type(m.ret, m.pos);
                if (m.self_kind != kSelfNone) {
                    if (m.params.empty() || m.params[0].first != "self") {
                        diag_.error(m.pos, "method '" + std::string(syms_.name(m.name)) +
                                    "' must declare 'self' as its first parameter");
                        continue;
                    }
                    if (m.self_kind == 3) {
                        diag_.error(m.pos, "methods take '&self' or '&mut self' "
                                    "(by-value receivers are not in the MVP)");
                        continue;
                    }
                    m.params[0].second = struct_ptr_of(base); // *Point
                    m.self_type = m.params[0].second;
                }
                SymbolId orig = m.name;
                m.name_orig = orig;
                m.name = syms_.intern(tname + "$" + std::string(syms_.name(orig)));
                mod_.fns.push_back(std::move(m));
                MethodKey key{base, trait_sym, orig};
                if (out_.methods.contains(key)) {
                    diag_.error(mod_.fns.back().pos, "duplicate method '" +
                                std::string(syms_.name(key.method)) + "' for type '" +
                                tname + "'");
                    continue;
                }
                out_.methods.insert(key, mod_.fns.size() - 1);
            }
        }
    }

    void check_trait_impl_signatures() {
        for (ImplDecl& id : mod_.impls) {
            if (id.trait_name == kNoSymbol) continue;
            const u32* ti = out_.trait_index.find(id.trait_name);
            if (!ti) continue;
            const TraitDecl& td = mod_.traits[*ti];
            const TypeId* tptr = out_.user_types.by_name.find(id.type_name);
            TypeId base = tptr ? out_.user_types.struct_of(*tptr) : ty_none();
            if (base == ty_none()) continue;
            for (const TraitMethod& tm : td.methods) {
                MethodKey key{base, id.trait_name, tm.name};
                const size_t* mi = out_.methods.find(key);
                if (!mi) {
                    diag_.error(id.pos, "impl is missing trait method '" +
                                std::string(syms_.name(tm.name)) + "'");
                    continue;
                }
                const FnDecl& m = mod_.fns[*mi];
                // compare params (after self) + ret structurally (aliases fold)
                size_t np = (tm.self_kind != kSelfNone && !tm.params.empty())
                    ? tm.params.size() - 1 : tm.params.size();
                size_t mp = (m.self_kind != kSelfNone && !m.params.empty())
                    ? m.params.size() - 1 : m.params.size();
                if (np != mp) {
                    diag_.error(m.pos, "method '" + std::string(syms_.name(tm.name)) +
                                "' arity does not match the trait declaration");
                    continue;
                }
                bool ok = canon_type(m.ret) == canon_type(tm.ret);
                for (size_t i = 0; ok && i < np; ++i) {
                    TypeId a = m.params[m.self_kind != kSelfNone ? i + 1 : i].second;
                    TypeId b = tm.params[tm.self_kind != kSelfNone ? i + 1 : i].second;
                    ok = canon_type(a) == canon_type(b);
                }
                if (!ok)
                    diag_.error(m.pos, "method '" + std::string(syms_.name(tm.name)) +
                                "' signature does not match the trait declaration");
            }
        }
    }

    // Canonical comparison type: aliases fold to their base.
    TypeId canon_type(TypeId t) {
        if (!ty_is_user(t)) return t;
        const UserType* u = &out_.user_types.get(t);
        int depth = 0;
        while (u->kind == UserKind::Alias && depth++ < 16) {
            if (!ty_is_user(u->target)) return u->target;
            u = &out_.user_types.get(u->target);
        }
        return kUserTyBase + static_cast<TypeId>(u - out_.user_types.types.data());
    }

    std::string user_type_name(TypeId t) {
        if (!ty_is_user(t)) return ty_name(t);
        const UserType& u = out_.user_types.get(t);
        if (u.kind == UserKind::Alias) return user_type_name(u.target);
        return std::string(syms_.name(u.name));
    }
    void declare_globals() {
        // functions first: consts may call comptime functions
        for (size_t fi = 0; fi < mod_.fns.size(); ++fi) {
            FnDecl& f = mod_.fns[fi];
            // resolve signature types (user names / pointers) in place
            for (auto& [pname, pty] : f.params) pty = resolve_type(pty, f.pos);
            f.ret = resolve_type(f.ret, f.pos);
            if (f.is_extern) {
                // extern "C": registered in its own table (pseudo-FnIds); never
                // in out_.fns — FnId == index stays contiguous for real fns.
                if (out_.externs.size() >= kExternFnMax) {
                    diag_.error(f.pos, "too many extern declarations (MVP limit)");
                    continue;
                }
                if (out_.extern_by_name.contains(f.name) || fn_index(f.name) != SIZE_MAX) {
                    diag_.error(f.pos, "redeclaration of '" +
                                std::string(syms_.name(f.name)) + "'");
                    continue;
                }
                if (f.name == syms_.intern("main")) {
                    diag_.error(f.pos, "'main' cannot be extern");
                    continue;
                }
                // C ABI: scalar params/ret only (no struct ABI in the MVP)
                for (auto& [pname, pty] : f.params) {
                    if (!ty_is_scalar(pty)) {
                        diag_.error(f.pos, "extern parameter '" + pname +
                                    "' must be a scalar (no struct/call ABI in MVP)");
                    }
                }
                if (!ty_is_scalar(f.ret) && f.ret != ty_void()) {
                    diag_.error(f.pos, "extern return type must be a scalar or void");
                }
                SemaExtern ex;
                ex.name = f.name;
                ex.ret = f.ret;
                for (auto& [pname, pty] : f.params) ex.params.push_back(pty);
                out_.externs.push_back(ex);
                out_.extern_by_name.insert(f.name, out_.externs.size() - 1);
                continue;
            }
            if (fn_index(f.name) != SIZE_MAX || out_.extern_by_name.contains(f.name)) {
                diag_.error(f.pos, "redeclaration of function '" + std::string(syms_.name(f.name)) + "'");
                continue;
            }
            SemaFn sf;
            sf.name = f.name;
            sf.ret = f.ret;
            for (auto& p : f.params) sf.params.push_back(p.second);
            sf.is_comptime = f.is_comptime;
            sf.always_inline = f.always_inline;
            sf.no_inline = f.no_inline;
            sf.ast_index = fi;
            sf.node_estimate = estimate_fn(f);
            out_.fns.push_back(sf);
            out_.fn_by_name.insert(f.name, out_.fns.size() - 1);
            if (f.name == syms_.intern("main")) {
                out_.has_main = true;
                if (!f.params.empty())
                    diag_.error(f.pos, "'main' must take no parameters in the MVP subset");
                if (f.ret != ty_void() && f.ret != ty_i64() && f.ret != ty_i32())
                    diag_.error(f.pos, "'main' must return void, i32 or i64");
            }
        }
        FlatMap<SymbolId, bool> seen_consts;
        for (ConstDecl& c : mod_.consts) {
            if (seen_consts.contains(c.name) || fn_index(c.name) != SIZE_MAX ||
                out_.extern_by_name.contains(c.name)) {
                diag_.error(c.pos, "redeclaration of '" + std::string(syms_.name(c.name)) + "'");
                continue;
            }
            seen_consts.insert(c.name, true);
            c.ty = resolve_type(c.ty, c.pos); // alias/user names
            std::optional<Value> v = eval_const(*c.value, c.ty);
            if (v) {
                c.iv = v->iv; c.fv = v->fv; c.is_fp = val_is_fp(*v);
                if (c.ty == ty_none()) c.ty = v->ty;
            }
        }
        for (size_t fi = 0; fi < mod_.fns.size(); ++fi) {
            // duplicate registration guard (fns registered above for const eval)
            (void)fi;
        }
    }

    size_t const_decl_index(SymbolId s) {
        for (size_t i = 0; i < mod_.consts.size(); ++i)
            if (mod_.consts[i].name == s) return i;
        return SIZE_MAX;
    }
    size_t fn_index(SymbolId s) {
        const size_t* p = out_.fn_by_name.find(s);
        return p ? *p : SIZE_MAX;
    }

    u32 estimate_fn(const FnDecl& f) {
        u32 n = 4;
        for (const StmtP& s : f.body) n += estimate_stmt(*s);
        return n;
    }
    u32 estimate_stmt(const Stmt& s) {
        u32 n = 1;
        if (s.value) n += estimate_expr(*s.value);
        if (s.cond) n += estimate_expr(*s.cond);
        if (s.target) n += estimate_expr(*s.target);
        if (s.to) n += estimate_expr(*s.to);
        for (const StmtP& c : s.body) n += estimate_stmt(*c);
        for (const StmtP& c : s.else_body) n += estimate_stmt(*c);
        return n;
    }
    u32 estimate_expr(const Expr& e) {
        u32 n = 1;
        if (e.lhs) n += estimate_expr(*e.lhs);
        if (e.rhs) n += estimate_expr(*e.rhs);
        for (const ExprP& a : e.args) n += estimate_expr(*a);
        return n;
    }

    // ---- scopes ------------------------------------------------------------
    struct Local {
        SymbolId sym;
        std::string name;
        TypeId ty;
        bool immutable;
        SourcePos pos;
    };
    struct Scope { std::vector<Local> locals; };

    const Local* find_local(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
            for (auto lit = it->locals.rbegin(); lit != it->locals.rend(); ++lit)
                if (lit->name == name) return &*lit;
        return nullptr;
    }
    void declare_local(const std::string& name, TypeId ty, bool immutable, SourcePos pos) {
        if (!scopes_.empty()) {
            for (const Local& l : scopes_.back().locals)
                if (l.name == name) {
                    diag_.error(pos, "redeclaration of '" + name + "' in the same scope");
                    return;
                }
        }
        scopes_.back().locals.push_back(Local{syms_.intern(name), name, ty, immutable, pos});
    }

    // ---- fn body checking ---------------------------------------------------
    struct FnCtx {
        const FnDecl* fn = nullptr;
        const SemaFn* sema = nullptr;
        bool saw_return = false;
    };

    void check_fn(FnDecl& fn) {
        if (fn.is_extern) return; // declaration only — signature checked at registration
        FnCtx ctx;
        size_t fidx = fn_index(fn.name);
        ctx.fn = &fn;
        ctx.sema = fidx != SIZE_MAX ? &out_.fns[fidx] : nullptr;
        scopes_.clear();
        scopes_.push_back(Scope{});
        for (auto& p : fn.params) declare_local(p.first, p.second, false, fn.pos);
        check_stmts(fn.body, ctx);
        if (!fn.body.empty() && !ctx.saw_return && fn.ret != ty_void()) {
            diag_.warn(fn.pos, "non-void function may finish without returning a value");
        }
    }

    std::string type_desc_str(TypeId t) {
        if (!ty_is_user(t)) return ty_name(t);
        return user_type_name(t);
    }

    void check_stmts(std::vector<StmtP>& stmts, FnCtx& ctx) {
        bool unreachable_reported = false;
        for (StmtP& s : stmts) {
            if (ctx.saw_return && !unreachable_reported) {
                diag_.warn(s->pos, "unreachable code after 'return'");
                unreachable_reported = true;
            }
            check_stmt(*s, ctx);
        }
    }

    void check_stmt(Stmt& s, FnCtx& ctx) {
        switch (s.kind) {
            case StmtKind::Let: {
                if (!s.value) {
                    diag_.error(s.pos, "binding has no initializer");
                    return;
                }
                TypeId it = check_expr(*s.value, ctx);
                s.decl_ty = ModuleAst::is_pending(s.decl_ty) ? resolve_type(s.decl_ty, s.pos)
                                                             : s.decl_ty;
                if (s.decl_ty != ty_none()) {
                    if (canon_type(it) != canon_type(s.decl_ty)) {
                        if (!unify_literal(*s.value, s.decl_ty)) {
                            diag_.error(s.pos, "initializer type " + type_desc_str(it) +
                                        " does not match declared type " + type_desc_str(s.decl_ty) +
                                        " (JULES has no implicit conversions; use an explicit 'as' cast)");
                        }
                    }
                } else {
                    s.decl_ty = it;
                }
                if (s.decl_ty == ty_void() || s.decl_ty == ty_none()) {
                    diag_.error(s.pos, "cannot bind a void value");
                    return;
                }
                declare_local(s.name, s.decl_ty, s.immutable, s.pos);
                break;
            }
            case StmtKind::Assign: {
                const Local* l = find_local(s.name);
                if (!l) {
                    diag_.error(s.pos, "assignment to undeclared variable '" + s.name + "'");
                    return;
                }
                if (l->immutable) {
                    diag_.error(s.pos, "cannot assign to immutable 'let' binding '" + s.name +
                                "' (declare it with 'var' to allow mutation)");
                    return;
                }
                TypeId vt = check_expr(*s.value, ctx);
                if (canon_type(vt) != canon_type(l->ty) && vt != ty_none()) {
                    if (!unify_literal(*s.value, l->ty))
                        diag_.error(s.pos, "cannot assign " + type_desc_str(vt) +
                                    " to variable of type " + type_desc_str(l->ty) +
                                    " (use an explicit cast)");
                }
                s.decl_ty = l->ty;
                break;
            }
            case StmtKind::AssignDeref: {
                TypeId pt = check_expr(*s.target, ctx);
                TypeId vt = check_expr(*s.value, ctx);
                // pointer-to-struct: whole-struct store through the pointer
                if (out_.is_struct_ptr(pt)) {
                    TypeId pointee = out_.user_types.pointee_struct(pt);
                    if (canon_type(vt) != canon_type(pointee) && vt != ty_none()) {
                        diag_.error(s.pos, "cannot store " + type_desc_str(vt) +
                                    " through pointer to " + type_desc_str(pointee));
                    }
                    break;
                }
                if (!ty_is_ptr(pt)) {
                    diag_.error(s.pos, "left side of deref-assign must be a pointer");
                    return;
                }
                TypeId pointee = pointee_of(pt);
                if (vt != pointee && vt != ty_none()) {
                    if (!unify_literal(*s.value, pointee))
                        diag_.error(s.pos, "cannot store " + type_desc_str(vt) +
                                    " through pointer to " + std::string(ty_name(pointee)));
                }
                break;
            }
            case StmtKind::AssignField: {
                // target is the struct base expr; s.name is the leaf field
                TypeId bt = check_expr(*s.target, ctx);
                if (bt == ty_none()) return;
                // bitfield segment store: read-modify-write the backing u64
                if (bitfield_kind_of(bt)) {
                    const UserType* u = &out_.user_types.get(bt);
                    int depth = 0;
                    while (u->kind == UserKind::Alias && depth++ < 16)
                        u = &out_.user_types.get(u->target);
                    SymbolId vs = syms_.find(s.name);
                    const BitSeg* seg = nullptr;
                    for (const BitSeg& c : u->segs)
                        if (c.name == vs) { seg = &c; break; }
                    if (!seg) {
                        diag_.error(s.pos, "bitfield '" + user_type_name(bt) +
                                    "' has no segment '" + s.name + "'");
                        return;
                    }
                    TypeId vt = check_expr(*s.value, ctx);
                    if (vt != ty_none() && vt != ty_u64() && !unify_literal(*s.value, ty_u64()))
                        diag_.error(s.pos, "bitfield segment values are u64 (got " +
                                    type_desc_str(vt) + ")");
                    return;
                }
                TypeId base = bt;
                if (out_.is_struct_ptr(bt)) base = out_.user_types.pointee_struct(bt);
                if (!out_.is_struct_type(base)) {
                    diag_.error(s.pos, "field assignment on non-struct type " +
                                type_desc_str(bt));
                    return;
                }
                const StructField* f = find_field(base, s.name);
                if (!f) {
                    report_no_field(s.pos, base, s.name);
                    return;
                }
                TypeId vt = check_expr(*s.value, ctx);
                if (canon_type(vt) != canon_type(f->ty) && vt != ty_none()) {
                    if (!unify_literal(*s.value, f->ty))
                        diag_.error(s.pos, "cannot assign " + type_desc_str(vt) +
                                    " to field '" + s.name + "' of type " +
                                    type_desc_str(f->ty));
                }
                break;
            }
            case StmtKind::AssignIndex: {
                TypeId pt = check_expr(*s.target, ctx);
                TypeId it = check_expr(*s.to, ctx);
                TypeId vt = check_expr(*s.value, ctx);
                if (!ty_is_ptr(pt) && !out_.is_struct_ptr(pt)) {
                    diag_.error(s.pos, "indexed assignment requires a pointer base");
                    return;
                }
                if (out_.is_struct_ptr(pt)) {
                    // array of structs: base[i] = struct value
                    TypeId pointee = out_.user_types.pointee_struct(pt);
                    if (!ty_is_int(it) || ty_is_bool(it)) {
                        diag_.error(s.pos, "array index must be an integer");
                        return;
                    }
                    if (canon_type(vt) != canon_type(pointee) && vt != ty_none()) {
                        diag_.error(s.pos, "cannot store " + type_desc_str(vt) +
                                    " into array of " + type_desc_str(pointee));
                    }
                    break;
                }
                TypeId pointee = pointee_of(pt);
                if (pointee == ty_none()) {
                    diag_.error(s.pos, "indexed assignment requires a scalar pointee");
                    return;
                }
                if (!ty_is_int(it) || ty_is_bool(it)) {
                    diag_.error(s.pos, "array index must be an integer");
                    return;
                }
                if (vt != pointee && vt != ty_none()) {
                    if (!unify_literal(*s.value, pointee))
                        diag_.error(s.pos, "cannot store " + std::string(ty_name(vt)) +
                                    " into array of " + std::string(ty_name(pointee)));
                }
                break;
            }
            case StmtKind::Return: {
                TypeId vt = s.value ? check_expr(*s.value, ctx) : ty_void();
                if (vt == ty_none()) return;
                if (canon_type(vt) != canon_type(ctx.fn->ret) &&
                    !(s.value && unify_literal(*s.value, ctx.fn->ret))) {
                    diag_.error(s.pos, "return type " + type_desc_str(vt) +
                                " does not match function return type " +
                                type_desc_str(ctx.fn->ret));
                }
                ctx.saw_return = true;
                break;
            }
            case StmtKind::If: {
                TypeId ct = check_expr(*s.cond, ctx);
                if (ct != ty_none() && !ty_is_bool(ct))
                    diag_.error(s.pos, "'if' condition must be bool, found " + std::string(ty_name(ct)));
                scopes_.push_back(Scope{});
                check_stmts(s.body, ctx);
                scopes_.pop_back();
                scopes_.push_back(Scope{});
                check_stmts(s.else_body, ctx);
                scopes_.pop_back();
                break;
            }
            case StmtKind::While: {
                TypeId ct = check_expr(*s.cond, ctx);
                if (ct != ty_none() && !ty_is_bool(ct))
                    diag_.error(s.pos, "'while' condition must be bool, found " + std::string(ty_name(ct)));
                scopes_.push_back(Scope{});
                check_stmts(s.body, ctx);
                scopes_.pop_back();
                break;
            }
            case StmtKind::For: {
                TypeId ft = check_expr(*s.target, ctx);
                TypeId tt = check_expr(*s.to, ctx);
                if (ft != tt && ft != ty_none() && tt != ty_none()) {
                    diag_.error(s.pos, "range bounds must have the same type (" +
                                std::string(ty_name(ft)) + " vs " + std::string(ty_name(tt)) + ")");
                } else if (ft == ty_none() || tt == ty_none()) {
                    return;
                }
                if (!ty_is_int(ft) || ty_is_bool(ft)) {
                    diag_.error(s.pos, "range loop requires an integer type");
                    return;
                }
                s.decl_ty = ft;
                scopes_.push_back(Scope{});
                declare_local(s.name, ft, false, s.pos);
                check_stmts(s.body, ctx);
                scopes_.pop_back();
                break;
            }
            case StmtKind::ExprStmt: {
                TypeId vt = check_expr(*s.value, ctx);
                if (vt != ty_none() && vt != ty_void() && s.value->kind == ExprKind::Binary)
                    diag_.info(s.pos, "computed value is unused");
                break;
            }
            case StmtKind::Block: {
                scopes_.push_back(Scope{});
                check_stmts(s.body, ctx);
                scopes_.pop_back();
                break;
            }
            case StmtKind::Defer: {
                // Checked in the current scope: names resolve as at declaration
                // (execution-time values, declaration-time names — documented).
                // Control flow inside defer bodies is rejected: the body is
                // DUPLICATED at every scope exit, so break/continue/return
                // would have no single meaning (Zig's rule, same reasoning).
                if (defer_has_control_flow(s.body)) {
                    diag_.error(s.pos, "defer bodies must not contain 'break', 'continue' "
                                "or 'return' (the body runs at every scope exit)");
                }
                if (defer_has_nested_defer(s.body)) {
                    diag_.error(s.pos, "defer bodies must not contain 'defer' (the body "
                                "is duplicated at every scope exit; a nested defer "
                                "would have no single run point)");
                }
                scopes_.push_back(Scope{});
                check_stmts(s.body, ctx);
                scopes_.pop_back();
                break;
            }
            case StmtKind::Break:
            case StmtKind::Continue:
                break;
        }
    }

    static bool defer_has_nested_defer(const std::vector<StmtP>& body) {
        for (const StmtP& s : body) {
            if (s->kind == StmtKind::Defer) return true;
            if (defer_has_nested_defer(s->body) || defer_has_nested_defer(s->else_body))
                return true;
        }
        return false;
    }

    static bool defer_has_control_flow(const std::vector<StmtP>& body) {
        for (const StmtP& s : body) {
            switch (s->kind) {
                case StmtKind::Break:
                case StmtKind::Continue:
                case StmtKind::Return:
                    return true;
                default: break;
            }
            if (defer_has_control_flow(s->body) || defer_has_control_flow(s->else_body))
                return true;
        }
        return false;
    }

    const StructField* find_field(TypeId struct_ty, const std::string& name) {
        TypeId base = out_.user_types.struct_of(struct_ty);
        if (base == ty_none()) return nullptr;
        UserType& u = out_.user_types.get_mut(base);
        SymbolId sym = syms_.find(name);
        if (sym == kNoSymbol) return nullptr;
        for (StructField& f : u.fields)
            if (f.name == sym) return &f;
        return nullptr;
    }
    void report_no_field(SourcePos pos, TypeId struct_ty, const std::string& name) {
        TypeId base = out_.user_types.struct_of(struct_ty);
        diag_.error(pos, "struct '" + user_type_name(base) + "' has no field '" + name + "'");
    }

    TypeId pointee_of(TypeId ptr) { return pointee_scalar(ptr); }
    static TypeId pointee_scalar(TypeId ptr) {
        TypeDesc d = type_desc(ptr);
        switch (d.pointee) {
            case Ty::I64: return ty_i64();
            case Ty::I32: return ty_i32();
            case Ty::U64: return ty_u64();
            case Ty::U32: return ty_u32();
            case Ty::F32: return ty_f32();
            case Ty::F64: return ty_f64();
            case Ty::I1:  return ty_i1();
            default: return ty_i64();
        }
    }

    // ---- literal typing --------------------------------------------------------------
    // Integer literals are typed by context (when the value fits): this is
    // literal inference, NOT implicit conversion of computed values — the
    // strict rule still applies to variables and expressions.
    static bool fits(u64 v, TypeId t) {
        switch (type_desc(t).ty) {
            case Ty::I32: return v <= static_cast<u64>(INT32_MAX);
            case Ty::U32: return v <= 0xFFFFFFFFull;
            case Ty::I64: return v <= static_cast<u64>(INT64_MAX);
            case Ty::U64: return true;
            default: return false;
        }
    }
    bool unify_literal(Expr& e, TypeId expected) {
        if (ty_is_user(expected)) expected = out_.ir_ty(expected); // aliases fold
        if (e.kind == ExprKind::IntLit && ty_is_int(expected) && !ty_is_bool(expected) &&
            fits(e.iv, expected)) {
            e.ty = expected;
            return true;
        }
        // negative literals: -(lit) unifies when the magnitude fits
        if (e.kind == ExprKind::Unary && e.uop == UnKind::Neg && e.lhs &&
            e.lhs->kind == ExprKind::IntLit && ty_is_int(expected) &&
            !ty_is_bool(expected) && fits(e.lhs->iv, expected)) {
            e.lhs->ty = expected;
            e.ty = expected;
            return true;
        }
        return false;
    }
    // ---- expressions ---------------------------------------------------------
    TypeId check_expr(Expr& e, FnCtx& ctx) {
        switch (e.kind) {
            case ExprKind::IntLit:
                return e.ty;
            case ExprKind::FloatLit:
                return e.ty;
            case ExprKind::BoolLit:
                return ty_i1();
            case ExprKind::Ident: {
                size_t ci = const_decl_index(e.sym);
                if (ci != SIZE_MAX) {
                    const ConstDecl& c = mod_.consts[ci];
                    e.comptime_value = true;
                    e.iv = c.is_fp ? 0 : c.iv;
                    e.fv = c.fv;
                    e.ty = c.ty;
                    return e.ty;
                }
                const Local* l = find_local(e.name);
                if (l) {
                    e.ty = l->ty;
                    return e.ty;
                }
                diag_.error(e.pos, "unknown name '" + e.name + "'");
                return ty_none();
            }
            case ExprKind::StructLit:
                return check_struct_lit(e, ctx);
            case ExprKind::Field:
                return check_field(e, ctx);
            case ExprKind::MethodCall:
                return check_method_call(e, ctx);
            case ExprKind::AddrOf:
                return check_addr_of(e, ctx);
            case ExprKind::Unary: {
                TypeId t = check_expr(*e.lhs, ctx);
                if (t == ty_none()) return ty_none();
                switch (e.uop) {
                    case UnKind::Neg:
                        if (!ty_is_int(t) && !ty_is_float(t)) {
                            diag_.error(e.pos, "unary '-' requires a numeric operand");
                            return ty_none();
                        }
                        break;
                    case UnKind::Not:
                        if (!ty_is_bool(t)) {
                            diag_.error(e.pos, "'!' requires a bool operand");
                            return ty_none();
                        }
                        break;
                    case UnKind::BNot:
                        // bitmasks: backing-width complement; plain ints as-is
                        if ((!ty_is_int(t) || ty_is_bool(t)) && !out_.is_int_backed(t)) {
                            diag_.error(e.pos, "'~' requires an integer operand");
                            return ty_none();
                        }
                        break;
                }
                e.ty = t;
                return t;
            }
            case ExprKind::Binary: {
                TypeId lt = check_expr(*e.lhs, ctx);
                TypeId rt = check_expr(*e.rhs, ctx);
                if (lt == ty_none() || rt == ty_none()) return ty_none();
                // aliases fold to their base for operator typing (a Scalar
                // field IS an f64 in every operator)
                if (ty_is_user(lt) && out_.user_types.get(lt).kind == UserKind::Alias)
                    lt = canon_type(lt);
                if (ty_is_user(rt) && out_.user_types.get(rt).kind == UserKind::Alias)
                    rt = canon_type(rt);
                // literal adoption: unify an integer literal with the other side
                if (canon_type(lt) != canon_type(rt)) {
                    if (unify_literal(*e.lhs, rt)) lt = rt;
                    else if (unify_literal(*e.rhs, lt)) rt = lt;
                }
                if (canon_type(lt) != canon_type(rt)) {
                    // bitmask | int-0 comparison edge: m == 0 is allowed
                    bool zero_lit = e.bop == BinKind::Eq || e.bop == BinKind::Ne;
                    bool lhs_zero = e.lhs->kind == ExprKind::IntLit && e.lhs->iv == 0;
                    bool rhs_zero = e.rhs->kind == ExprKind::IntLit && e.rhs->iv == 0;
                    bool int_backed = out_.is_int_backed(lt) || out_.is_int_backed(rt);
                    if (!(zero_lit && int_backed && (lhs_zero || rhs_zero))) {
                        diag_.error(e.pos, "mixed-type operands (" + type_desc_str(lt) + " vs " +
                                    type_desc_str(rt) + "); JULES has no implicit conversions — cast one side with 'as'");
                        return ty_none();
                    }
                    // normalize: make the bitmask side the typed one
                    if (out_.is_int_backed(rt)) { lt = rt; } else { rt = lt; }
                }
                bool is_cmp = e.bop == BinKind::Eq || e.bop == BinKind::Ne || e.bop == BinKind::Lt ||
                              e.bop == BinKind::Le || e.bop == BinKind::Gt || e.bop == BinKind::Ge;
                bool is_logic = e.bop == BinKind::LogicAnd || e.bop == BinKind::LogicOr;
                if (is_logic) {
                    if (!ty_is_bool(lt) || !ty_is_bool(rt)) {
                        diag_.error(e.pos, "'&&'/'||' require bool operands");
                        return ty_none();
                    }
                    e.ty = ty_i1();
                    return e.ty;
                }
                // user integer-backed types: the operator set each kind allows
                if (out_.is_int_backed(lt)) {
                    const UserType* u = &out_.user_types.get(lt);
                    int depth = 0;
                    while (u->kind == UserKind::Alias && depth++ < 16)
                        u = &out_.user_types.get(u->target);
                    bool eq_only = u->kind == UserKind::Enum || u->kind == UserKind::Bitfield;
                    if (is_cmp) {
                        if (u->kind == UserKind::Bitmask &&
                            !(e.bop == BinKind::Eq || e.bop == BinKind::Ne)) {
                            diag_.error(e.pos, "bitmasks compare with == / != only");
                            return ty_none();
                        }
                        if (eq_only || u->kind == UserKind::Bitmask) { e.ty = ty_i1(); return e.ty; }
                    } else {
                        bool ok = u->kind == UserKind::Bitmask &&
                                  (e.bop == BinKind::And || e.bop == BinKind::Or ||
                                   e.bop == BinKind::Xor);
                        if (!ok) {
                            diag_.error(e.pos, "arithmetic is not defined for '" +
                                        user_type_name(lt) + "' values (use 'as' to the "
                                        "backing integer, or the allowed set operators)");
                            return ty_none();
                        }
                        e.ty = lt;
                        return e.ty;
                    }
                }
                if (out_.is_struct_type(lt) || out_.is_struct_ptr(lt)) {
                    diag_.error(e.pos, "operators are not defined for struct values");
                    return ty_none();
                }
                if (e.bop == BinKind::Shl || e.bop == BinKind::Shr) {
                    if (!ty_is_int(lt) || ty_is_bool(lt)) {
                        diag_.error(e.pos, "shift requires integer left operand");
                        return ty_none();
                    }
                    if (!ty_is_int(rt) || ty_is_bool(rt)) {
                        diag_.error(e.pos, "shift requires integer right operand");
                        return ty_none();
                    }
                    e.ty = lt;
                    return e.ty;
                }
                // (mixed-type + user-type cases handled above)
                if (is_cmp) {
                    if (ty_is_ptr(lt) && !(e.bop == BinKind::Eq || e.bop == BinKind::Ne)) {
                        diag_.error(e.pos, "pointers support only ==/!= comparison in MVP");
                        return ty_none();
                    }
                    e.ty = ty_i1();
                    return e.ty;
                }
                if (ty_is_ptr(lt)) {
                    diag_.error(e.pos, "arithmetic on raw pointers is not in the MVP subset");
                    return ty_none();
                }
                switch (e.bop) {
                    case BinKind::And: case BinKind::Or: case BinKind::Xor:
                        if (!ty_is_int(lt) && !ty_is_bool(lt)) {
                            diag_.error(e.pos, "bitwise operator requires integer or bool operands");
                            return ty_none();
                        }
                        break;
                    default:
                        if (!ty_is_int(lt) && !ty_is_float(lt)) {
                            diag_.error(e.pos, "arithmetic requires numeric operands");
                            return ty_none();
                        }
                        break;
                }
                e.ty = lt;
                return e.ty;
            }
            case ExprKind::Cast: {
                TypeId st = check_expr(*e.lhs, ctx);
                e.cast_target = ModuleAst::is_pending(e.cast_target)
                                    ? resolve_type(e.cast_target, e.pos)
                                    : e.cast_target;
                TypeId target = e.cast_target;
                if (st == ty_none()) return ty_none();
                // user int-backed types <-> their backing integer (both ways)
                if (out_.is_int_backed(st) || out_.is_int_backed(target)) {
                    bool ok = false;
                    if (out_.is_int_backed(st) && !ty_is_user(target) && ty_is_int(target))
                        ok = true; // enum/bitmask/bitfield -> integer
                    else if (!ty_is_user(st) && ty_is_int(st) && out_.is_int_backed(target))
                        ok = true; // integer -> enum/bitmask/bitfield (unchecked, documented)
                    if (!ok) {
                        diag_.error(e.pos, "cast between " + type_desc_str(st) + " and " +
                                    type_desc_str(target) +
                                    " is not allowed (user integer types cast only to/from "
                                    "integers; use the exact backing type)");
                        return ty_none();
                    }
                    e.ty = target;
                    return e.ty;
                }
                if (out_.is_struct_type(st) || out_.is_struct_type(target) ||
                    out_.is_struct_ptr(st) || out_.is_struct_ptr(target)) {
                    diag_.error(e.pos, "casting struct values or struct pointers is not in "
                                "the MVP (no subtyping, no struct ABI)");
                    return ty_none();
                }
                if (!ty_is_scalar(st) || !ty_is_scalar(target)) {
                    diag_.error(e.pos, "cast requires scalar types");
                    return ty_none();
                }
                if (ty_is_ptr(st) != ty_is_ptr(target)) {
                    diag_.error(e.pos, "casting between pointers and non-pointers is not in the MVP subset");
                    return ty_none();
                }
                e.ty = target;
                return target;
            }
            case ExprKind::Deref: {
                TypeId t = check_expr(*e.lhs, ctx);
                if (t == ty_none()) return ty_none();
                // pointer-to-struct: *p yields the struct value at p
                if (out_.is_struct_ptr(t)) {
                    e.ty = out_.user_types.pointee_struct(t);
                    return e.ty;
                }
                if (!ty_is_ptr(t)) {
                    diag_.error(e.pos, "cannot dereference non-pointer type " + std::string(ty_name(t)));
                    return ty_none();
                }
                e.ty = pointee_scalar(t);
                return e.ty;
            }
            case ExprKind::Index: {
                TypeId b = check_expr(*e.lhs, ctx);
                TypeId i = check_expr(*e.rhs, ctx);
                if (b == ty_none() || i == ty_none()) return ty_none();
                if (!ty_is_ptr(b) && !out_.is_struct_ptr(b)) {
                    diag_.error(e.pos, "cannot index non-pointer type " + std::string(ty_name(b)) +
                                " (arrays are pointers from alloc(T, n))");
                    return ty_none();
                }
                if (!ty_is_int(i) || ty_is_bool(i)) {
                    diag_.error(e.pos, "array index must be an integer (got " +
                                std::string(ty_name(i)) + ")");
                    return ty_none();
                }
                // array of structs: base[i] yields the element struct value
                if (out_.is_struct_ptr(b)) {
                    e.ty = out_.user_types.pointee_struct(b);
                    return e.ty;
                }
                e.ty = pointee_scalar(b);
                if (e.ty == ty_none()) {
                    diag_.error(e.pos, "cannot index a pointer to a non-scalar element");
                    return ty_none();
                }
                return e.ty;
            }
            case ExprKind::ComptimeBlock: {
                FlatMap<std::string, Value> env;
                steps_ = 0;
                std::optional<Value> v = interpret_stmts(e.block_body, env, ty_none());
                if (!v) {
                    diag_.error(e.pos, "comptime block must return a value at compile time");
                    return ty_none();
                }
                rewrite_to_literal(e, *v);
                return e.ty;
            }
            case ExprKind::Call:
                return check_call(e, ctx);
        }
        return ty_none();
    }

    // Does this base Ident name a user TYPE (for qualified constants like
    // Color.Red)? Locals/consts shadow type names.
    TypeId base_names_type(const Expr& base) {
        if (base.kind != ExprKind::Ident) return ty_none();
        if (find_local(base.name) || const_decl_index(base.sym) != SIZE_MAX) return ty_none();
        const TypeId* t = out_.user_types.by_name.find(base.sym);
        return t ? *t : ty_none();
    }

    // fold `Type.Variant` for enums / bitmasks into a comptime constant
    bool bitfield_kind_of(TypeId t) {
        const UserType* u = &out_.user_types.get(t);
        int depth = 0;
        while (u->kind == UserKind::Alias && depth++ < 16)
            u = &out_.user_types.get(u->target);
        return u->kind == UserKind::Bitfield;
    }

    bool fold_qualified_const(Expr& e, TypeId t) {
        const UserType* u = &out_.user_types.get(t);
        int depth = 0;
        while (u->kind == UserKind::Alias && depth++ < 16)
            u = &out_.user_types.get(u->target);
        if (u->kind != UserKind::Enum && u->kind != UserKind::Bitmask) return false;
        SymbolId vs = syms_.find(e.name);
        if (vs == kNoSymbol) return false;
        for (const auto& [vname, vval] : u->variants) {
            if (vname == vs) {
                e.iv = vval;
                e.ty = t;
                e.comptime_value = true;
                return true;
            }
        }
        return false;
    }

    TypeId check_struct_lit(Expr& e, FnCtx& ctx) {
        const TypeId* t = out_.user_types.by_name.find(syms_.find(e.name));
        // bitfield literal: Packed { kind: 5, flag: 1 } folds to a u64 constant
        if (t && bitfield_kind_of(*t)) {
            const UserType* u = &out_.user_types.get(*t);
            int depth = 0;
            while (u->kind == UserKind::Alias && depth++ < 16)
                u = &out_.user_types.get(u->target);
            std::vector<char> seen(u->segs.size(), 0);
            u64 acc = 0;
            for (size_t i = 0; i < e.field_names.size(); ++i) {
                const BitSeg* seg = nullptr;
                for (const BitSeg& c : u->segs)
                    if (std::string(syms_.name(c.name)) == e.field_names[i]) { seg = &c; break; }
                if (!seg) {
                    diag_.error(e.pos, "bitfield '" + e.name + "' has no segment '" +
                                e.field_names[i] + "'");
                    continue;
                }
                size_t si = static_cast<size_t>(seg - u->segs.data());
                if (si < seen.size()) {
                    if (seen[si]) diag_.error(e.pos, "duplicate segment '" + e.field_names[i] + "'");
                    seen[si] = 1;
                }
                TypeId vt = check_expr(*e.args[i], ctx);
                if (vt == ty_none()) continue;
                const Expr& sv = *e.args[i];
                bool is_ct = sv.comptime_value || sv.kind == ExprKind::IntLit;
                u64 v = is_ct ? sv.iv : ~u64(0);
                if (!is_ct) {
                    diag_.error(e.pos, "bitfield segment '" + e.field_names[i] +
                                "' must be a compile-time integer");
                    continue;
                }
                u64 mask = (seg->width >= 64) ? ~u64(0) : ((u64(1) << seg->width) - 1);
                acc |= (v & mask) << seg->shift;
            }
            for (size_t i = 0; i < seen.size(); ++i)
                if (!seen[i])
                    diag_.error(e.pos, "bitfield literal is missing segment '" +
                                std::string(syms_.name(u->segs[i].name)) + "'");
            e.iv = acc;
            e.ty = *t;
            e.comptime_value = true;
            return e.ty;
        }
        if (!t || out_.user_types.struct_of(*t) == ty_none()) {
            diag_.error(e.pos, "'" + e.name + "' is not a struct type");
            return ty_none();
        }
        TypeId base = out_.user_types.struct_of(*t);
        UserType& u = out_.user_types.get_mut(base);
        if (e.field_names.size() != u.fields.size()) {
            diag_.error(e.pos, "struct literal for '" + e.name + "' has " +
                        std::to_string(e.field_names.size()) + " field(s) but '" + e.name +
                        "' has " + std::to_string(u.fields.size()) +
                        " (all fields, exactly once, named)");
            // continue checking what is present for better diagnostics
        }
        std::vector<char> seen(u.fields.size(), 0);
        for (size_t i = 0; i < e.field_names.size(); ++i) {
            const StructField* f = find_field(base, e.field_names[i]);
            if (!f) {
                report_no_field(e.pos, base, e.field_names[i]);
                continue;
            }
            size_t fi = static_cast<size_t>(f - u.fields.data());
            if (fi < seen.size()) {
                if (seen[fi])
                    diag_.error(e.pos, "duplicate field '" + e.field_names[i] +
                                "' in struct literal");
                seen[fi] = true;
            }
            TypeId vt = check_expr(*e.args[i], ctx);
            if (vt == ty_none()) continue;
            if (canon_type(vt) != canon_type(f->ty) && !unify_literal(*e.args[i], f->ty)) {
                diag_.error(e.args[i]->pos, "field '" + e.field_names[i] + "' has type " +
                            type_desc_str(f->ty) + " but the value has type " +
                            type_desc_str(vt));
            }
        }
        for (size_t i = 0; i < seen.size(); ++i)
            if (!seen[i])
                diag_.error(e.pos, "struct literal is missing field '" +
                            std::string(syms_.name(u.fields[i].name)) + "'");
        e.ty = base;
        return e.ty;
    }

    TypeId check_field(Expr& e, FnCtx& ctx) {
        // qualified constant `Color.Red` / `Perm.Read`?
        if (e.lhs && e.lhs->kind == ExprKind::Ident) {
            TypeId t = base_names_type(*e.lhs);
            if (t != ty_none()) {
                if (fold_qualified_const(e, t)) return e.ty;
                const UserType* u = &out_.user_types.get(t);
                int depth = 0;
                while (u->kind == UserKind::Alias && depth++ < 16)
                    u = &out_.user_types.get(u->target);
                if (u->kind == UserKind::Struct) {
                    diag_.error(e.pos, "'" + e.lhs->name + "' is a struct TYPE; construct one "
                                "with a literal: " + e.lhs->name + " { ... }");
                } else {
                    diag_.error(e.pos, "type '" + e.lhs->name + "' has no constant '" +
                                e.name + "'");
                }
                return ty_none();
            }
        }
        TypeId bt = check_expr(*e.lhs, ctx);
        if (bt == ty_none()) return ty_none();
        // bitfield segment read: (v >> shift) & mask, typed u64
        if (bitfield_kind_of(bt)) {
            const UserType* u = &out_.user_types.get(bt);
            int depth = 0;
            while (u->kind == UserKind::Alias && depth++ < 16)
                u = &out_.user_types.get(u->target);
            SymbolId vs = syms_.find(e.name);
            for (const BitSeg& seg : u->segs) {
                if (seg.name == vs) {
                    e.ty = ty_u64();
                    return e.ty;
                }
            }
            diag_.error(e.pos, "bitfield '" + user_type_name(bt) + "' has no segment '" +
                        e.name + "'");
            return ty_none();
        }
        TypeId base = bt;
        if (out_.is_struct_ptr(bt)) base = out_.user_types.pointee_struct(bt); // auto-deref
        if (!out_.is_struct_type(base)) {
            diag_.error(e.pos, "field access '" + e.name + "' on non-struct type " +
                        type_desc_str(bt));
            return ty_none();
        }
        const StructField* f = find_field(base, e.name);
        if (!f) {
            report_no_field(e.pos, base, e.name);
            return ty_none();
        }
        e.ty = f->ty;
        return e.ty;
    }

    TypeId check_method_call(Expr& e, FnCtx& ctx) {
        TypeId rt = check_expr(*e.lhs, ctx); // receiver (may force memory)
        if (rt == ty_none()) return ty_none();
        // bitmask `m.has(Flag)` — sugar for (m & Flag) != 0
        if (ty_is_user(rt)) {
            const UserType* u = &out_.user_types.get(rt);
            int depth = 0;
            while (u->kind == UserKind::Alias && depth++ < 16)
                u = &out_.user_types.get(u->target);
            if (u->kind == UserKind::Bitmask && e.name == "has") {
                if (e.args.size() != 1) {
                    diag_.error(e.pos, "has() takes exactly one bitmask value");
                    return ty_none();
                }
                TypeId at = check_expr(*e.args[0], ctx);
                if (canon_type(at) != canon_type(rt) && at != ty_none()) {
                    diag_.error(e.pos, "has() expects a '" + user_type_name(rt) +
                                "' value (got " + type_desc_str(at) + ")");
                }
                e.ty = ty_i1();
                return e.ty;
            }
        }
        TypeId base;
        if (out_.is_struct_ptr(rt)) {
            base = out_.user_types.pointee_struct(rt);
        } else if (out_.is_struct_type(rt)) {
            base = out_.user_types.struct_of(rt);
        } else {
            diag_.error(e.pos, "no method '" + e.name + "' for type " + type_desc_str(rt) +
                        " (methods are declared in impl blocks)");
            return ty_none();
        }
        // inherent method first, then trait impls (ambiguity if two traits
        // provide the same method name for the type)
        MethodKey key{base, kNoSymbol, syms_.find(e.name)};
        size_t m_idx = SIZE_MAX;
        if (const size_t* mi = out_.methods.find(key)) {
            m_idx = *mi;
        } else {
            SymbolId found_trait = kNoSymbol;
            for (const auto& [k, v] : out_.methods.entries()) {
                if (k.type == base && k.method == key.method && k.trait != kNoSymbol) {
                    if (m_idx != SIZE_MAX && found_trait != k.trait) {
                        diag_.error(e.pos, "ambiguous method '" + e.name + "': implemented "
                                    "for two traits; call it through a specific impl");
                        return ty_none();
                    }
                    m_idx = v;
                    found_trait = k.trait;
                }
            }
        }
        if (m_idx == SIZE_MAX) {
            diag_.error(e.pos, "no method '" + e.name + "' for struct '" +
                        user_type_name(base) + "'");
            return ty_none();
        }
        size_t out_idx = m_idx;
        if (const size_t* t = ast_to_out_.find(m_idx)) out_idx = *t;
        const SemaFn& m = out_.fns[out_idx];
        // self already bound: the receiver is not an explicit argument
        bool has_self = mod_.fns[m.ast_index].self_kind != kSelfNone;
        size_t want = has_self ? m.params.size() - 1 : m.params.size();
        if (e.args.size() != want) {
            diag_.error(e.pos, "method '" + e.name + "' expects " + std::to_string(want) +
                        " argument(s) besides the receiver, got " +
                        std::to_string(e.args.size()));
            return ty_none();
        }
        for (size_t i = 0; i < e.args.size(); ++i) {
            TypeId at = check_expr(*e.args[i], ctx);
            TypeId pt = m.params[has_self ? i + 1 : i];
            if (at == ty_none()) return ty_none();
            if (canon_type(at) != canon_type(pt) && !unify_literal(*e.args[i], pt)) {
                diag_.error(e.pos, "argument " + std::to_string(i + 1) + " of '" + e.name +
                            "' has type " + type_desc_str(at) + " but " +
                            type_desc_str(pt) + " is expected");
                return ty_none();
            }
        }
        e.callee = m.name; // mangled symbol; the builder resolves it
        e.ty = m.ret;
        return e.ty;
    }

    TypeId check_addr_of(Expr& e, FnCtx& ctx) {
        if (!e.lhs || (e.lhs->kind != ExprKind::Ident && e.lhs->kind != ExprKind::Field)) {
            diag_.error(e.pos, "'&' applies to a struct lvalue (a local or a field chain)");
            return ty_none();
        }
        TypeId t = check_expr(*e.lhs, ctx);
        if (t == ty_none()) return ty_none();
        if (out_.is_struct_ptr(t)) {
            diag_.error(e.pos, "operand of '&' is already a pointer");
            return ty_none();
        }
        if (!out_.is_struct_type(t)) {
            diag_.error(e.pos, "'&' requires a struct value (scalars have no address "
                        "identity in the MVP; use a struct or alloc)");
            return ty_none();
        }
        e.ty = struct_ptr_of(t);
        return e.ty;
    }

    TypeId check_call(Expr& e, FnCtx& ctx) {
        std::string callee = e.name;
        if (callee == "alloc") return check_alloc(e, ctx);
        if (callee == "free") return check_free(e, ctx);
        if (callee == "print") return check_print(e, ctx);

        size_t fi = fn_index(e.callee);
        if (fi == SIZE_MAX) {
            const size_t* xi = out_.extern_by_name.find(e.callee);
            if (xi != nullptr) {
                const SemaExtern& ex = out_.externs[*xi];
                if (e.args.size() != ex.params.size()) {
                    diag_.error(e.pos, "extern function '" + callee + "' expects " +
                                std::to_string(ex.params.size()) + " argument(s), got " +
                                std::to_string(e.args.size()));
                    return ty_none();
                }
                for (size_t i = 0; i < e.args.size(); ++i) {
                    TypeId at = check_expr(*e.args[i], ctx);
                    if (at == ty_none()) return ty_none();
                    if (at != ex.params[i] && !unify_literal(*e.args[i], ex.params[i])) {
                        diag_.error(e.pos, "argument " + std::to_string(i + 1) + " of extern '" +
                                    callee + "' has type " + type_desc_str(at) + " but " +
                                    std::string(ty_name(ex.params[i])) +
                                    " is expected (C ABI, no implicit conversions)");
                        return ty_none();
                    }
                }
                e.ty = ex.ret;
                return e.ty;
            }
            diag_.error(e.pos, "call to unknown function '" + callee + "'");
            return ty_none();
        }
        SemaFn& sf = out_.fns[fi];
        if (e.args.size() != sf.params.size()) {
            diag_.error(e.pos, "function '" + callee + "' expects " +
                        std::to_string(sf.params.size()) + " argument(s), got " +
                        std::to_string(e.args.size()));
            return ty_none();
        }
        for (size_t i = 0; i < e.args.size(); ++i) {
            TypeId at = check_expr(*e.args[i], ctx);
            if (at == ty_none()) return ty_none();
            if (canon_type(at) != canon_type(sf.params[i])) {
                if (!unify_literal(*e.args[i], sf.params[i])) {
                    diag_.error(e.pos, "argument " + std::to_string(i + 1) + " of '" + callee + "' has type " +
                                type_desc_str(at) + " but " + type_desc_str(sf.params[i]) +
                                " is expected (no implicit conversions)");
                    return ty_none();
                }
            }
        }
        if ((sf.is_comptime || e.comptime_call) && !ctx.fn->is_comptime) {
            // Inside a comptime fn body, calls (including self-recursion) are
            // evaluated by the interpreter when an outer comptime call runs.
            std::optional<Value> v = eval_comptime_call(e, fi);
            if (v) {
                rewrite_to_literal(e, *v);
                return e.ty;
            }
            diag_.error(e.pos, std::string(e.comptime_call ? "comptime call '" : "comptime function '") +
                        callee + "' could not be evaluated at compile time (arguments must be comptime-known)");
            return ty_none();
        }
        e.ty = sf.ret;
        return e.ty;
    }

    TypeId check_alloc(Expr& e, FnCtx& ctx) {
        if (e.args.empty() || e.args[0]->kind != ExprKind::Ident) {
            diag_.error(e.pos, "alloc expects a type argument, e.g. alloc(i64) or alloc(i64, n)");
            return ty_none();
        }
        TypeId pointee = ty_none();
        std::string tn = e.args[0]->name;
        if (tn == "i32") pointee = ty_i32();
        else if (tn == "i64" || tn == "usize") pointee = ty_i64();
        else if (tn == "u32") pointee = ty_u32();
        else if (tn == "u64") pointee = ty_u64();
        else if (tn == "f32") pointee = ty_f32();
        else if (tn == "f64") pointee = ty_f64();
        else if (tn == "bool") pointee = ty_i1();
        if (pointee == ty_none()) {
            // user struct type: alloc(Node) / alloc(Node, n)
            const TypeId* t = out_.user_types.by_name.find(e.args[0]->sym);
            if (t && out_.user_types.struct_of(*t) != ty_none()) {
                TypeId base = out_.user_types.struct_of(*t);
                e.args[0]->cast_target = base; // builder reads this (struct id)
                if (e.args.size() == 2) {
                    TypeId ct = check_expr(*e.args[1], ctx);
                    if (ct == ty_none()) return ty_none();
                    if (!ty_is_int(ct) || ty_is_bool(ct)) {
                        diag_.error(e.args[1]->pos, "alloc element count must be an integer (got " +
                                    std::string(ty_name(ct)) + ")");
                        return ty_none();
                    }
                } else if (e.args.size() > 2) {
                    diag_.error(e.pos, "alloc expects a type and an optional count: alloc(T) or alloc(T, n)");
                    return ty_none();
                }
                e.ty = struct_ptr_of(base);
                return e.ty;
            }
            diag_.error(e.pos, "alloc requires a scalar or struct type argument (got '" + tn + "')");
            return ty_none();
        }
        e.args[0]->cast_target = pointee; // builder reads this
        if (e.args.size() == 2) {
            // Sized allocation: alloc(T, n) -> array of n elements.
            TypeId ct = check_expr(*e.args[1], ctx);
            if (ct == ty_none()) return ty_none();
            if (!ty_is_int(ct) || ty_is_bool(ct)) {
                diag_.error(e.args[1]->pos, "alloc element count must be an integer (got " +
                            std::string(ty_name(ct)) + ")");
                return ty_none();
            }
        } else if (e.args.size() > 2) {
            diag_.error(e.pos, "alloc expects a type and an optional count: alloc(T) or alloc(T, n)");
            return ty_none();
        }
        e.ty = ty_ptr(pointee);
        return e.ty;
    }

    TypeId check_free(Expr& e, FnCtx& ctx) {
        if (e.args.size() != 1) {
            diag_.error(e.pos, "free expects exactly one pointer argument");
            return ty_none();
        }
        TypeId at = check_expr(*e.args[0], ctx);
        if (at == ty_none()) return ty_none();
        if (!ty_is_ptr(at) && !out_.is_struct_ptr(at)) {
            diag_.error(e.pos, "free expects a pointer, got " + std::string(ty_name(at)));
            return ty_none();
        }
        e.ty = ty_void();
        return e.ty;
    }

    TypeId check_print(Expr& e, FnCtx& ctx) {
        if (e.args.size() != 1) {
            diag_.error(e.pos, "print expects exactly one value in the MVP subset");
            return ty_none();
        }
        TypeId at = check_expr(*e.args[0], ctx);
        if (at == ty_none()) return ty_none();
        // user integer-backed types and aliases print their backing value
        // (documented); struct values still print field by field
        TypeId eff = out_.ir_ty(at);
        if (!ty_is_scalar(eff) || ty_is_ptr(eff)) {
            diag_.error(e.pos, "print supports numeric/bool values in MVP (got " +
                        type_desc_str(at) + "); structs print field by field");
            return ty_none();
        }
        e.ty = ty_void();
        return e.ty;
    }

    static void rewrite_to_literal(Expr& e, const Value& v) {
        if (ty_is_float(v.ty)) {
            e.kind = ExprKind::FloatLit;
            e.fv = v.fv;
        } else {
            e.kind = ExprKind::IntLit;
            e.iv = v.iv;
        }
        e.ty = v.ty;
        e.comptime_value = true;
        e.lhs.reset(); e.rhs.reset(); e.args.clear();
    }

    // ---- escape analysis (struct locals: decomposed vs contiguous) --------
    // A struct local is DECOMPOSED into per-field slots (SROA-by-construction,
    // the fast path) unless its address is materialized somewhere: '&x', a
    // method receiver, or a by-value pass. Then it lives in one contiguous
    // allocation. Two effects: (1) forces collected over the checked body;
    // (2) flagged on the Let statements for the builder.
    struct EscBinding {
        const std::string* name;
        Stmt* let;   // null for params / loop vars
        TypeId ty;
    };
    std::vector<std::vector<EscBinding>> esc_scopes_;
    std::vector<Stmt*> forced_;
    FlatMap<size_t, size_t> ast_to_out_; // mod_.fns index -> out_.fns index

    const EscBinding* esc_find(const std::string& n) {
        for (auto it = esc_scopes_.rbegin(); it != esc_scopes_.rend(); ++it)
            for (auto b = it->rbegin(); b != it->rend(); ++b)
                if (*b->name == n) return &*b;
        return nullptr;
    }
    void esc_push() { esc_scopes_.emplace_back(); }
    void esc_pop() { if (!esc_scopes_.empty()) esc_scopes_.pop_back(); }

    void esc_force_root(Expr& e) {
        if (e.kind == ExprKind::Ident) {
            const EscBinding* b = esc_find(e.name);
            if (b && b->let && out_.is_struct_type(b->ty)) forced_.push_back(b->let);
        } else if (e.kind == ExprKind::Field || e.kind == ExprKind::Deref) {
            if (e.lhs) esc_force_root(*e.lhs);
        }
    }

    void run_escape_analysis(FnDecl& fn) {
        forced_.clear();
        esc_scopes_.clear();
        esc_push();
        for (auto& p : fn.params)
            esc_scopes_.back().push_back(EscBinding{&p.first, nullptr, p.second});
        esc_walk_stmts(fn.body);
        esc_pop();
        // flip: every forced Let now lives in one contiguous allocation
        for (Stmt* s : forced_) s->struct_in_mem = true;
    }

    void esc_walk_stmts(std::vector<StmtP>& stmts) {
        for (StmtP& s : stmts) esc_walk_stmt(*s);
    }

    void esc_walk_stmt(Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let:
                if (s.value) esc_walk_expr(*s.value);
                esc_scopes_.back().push_back(
                    EscBinding{&s.name, &s, s.decl_ty});
                break;
            case StmtKind::Assign:
                if (s.value) esc_walk_expr(*s.value);
                break;
            case StmtKind::AssignDeref:
            case StmtKind::AssignIndex:
                if (s.target) esc_walk_expr(*s.target);
                if (s.to) esc_walk_expr(*s.to);
                if (s.value) esc_walk_expr(*s.value);
                break;
            case StmtKind::AssignField:
                if (s.target) esc_walk_expr(*s.target); // reads only
                if (s.value) esc_walk_expr(*s.value);
                break;
            case StmtKind::Return:
                if (s.value) esc_walk_expr(*s.value);
                break;
            case StmtKind::If:
                if (s.cond) esc_walk_expr(*s.cond);
                esc_push();
                esc_walk_stmts(s.body);
                esc_pop();
                esc_push();
                esc_walk_stmts(s.else_body);
                esc_pop();
                break;
            case StmtKind::While:
                if (s.cond) esc_walk_expr(*s.cond);
                esc_push();
                esc_walk_stmts(s.body);
                esc_pop();
                break;
            case StmtKind::For:
                if (s.target) esc_walk_expr(*s.target);
                if (s.to) esc_walk_expr(*s.to);
                esc_push();
                // loop var is an integer — never struct
                esc_walk_stmts(s.body);
                esc_pop();
                break;
            case StmtKind::ExprStmt:
                if (s.value) esc_walk_expr(*s.value);
                break;
            case StmtKind::Defer:
                esc_push();
                esc_walk_stmts(s.body);
                esc_pop();
                break;
            case StmtKind::Block:
                esc_push();
                esc_walk_stmts(s.body);
                esc_pop();
                break;
            case StmtKind::Break:
            case StmtKind::Continue:
                break;
        }
    }

    void esc_walk_expr(Expr& e) {
        if (e.lhs) esc_walk_expr(*e.lhs);
        if (e.rhs) esc_walk_expr(*e.rhs);
        for (ExprP& a : e.args) esc_walk_expr(*a);
        switch (e.kind) {
            case ExprKind::AddrOf:
                if (e.lhs) esc_force_root(*e.lhs);
                break;
            case ExprKind::MethodCall:
                if (e.lhs) esc_force_root(*e.lhs); // receiver needs &self
                // method args: by-value struct params force too
                {
                    size_t fi = fn_index(e.callee);
                    if (fi != SIZE_MAX) {
                        SemaFn& m = out_.fns[fi];
                        bool has_self = mod_.fns[m.ast_index].self_kind != kSelfNone;
                        for (size_t i = 0; i < e.args.size(); ++i) {
                            size_t pi = has_self ? i + 1 : i;
                            if (pi < m.params.size() && out_.is_struct_type(m.params[pi]) &&
                                e.args[i]->kind == ExprKind::Ident) {
                                const EscBinding* b = esc_find(e.args[i]->name);
                                if (b && b->let) forced_.push_back(b->let);
                            }
                        }
                    }
                }
                break;
            case ExprKind::Call: {
                size_t fi = fn_index(e.callee);
                if (fi != SIZE_MAX) {
                    SemaFn& m = out_.fns[fi];
                    for (size_t i = 0; i < e.args.size() && i < m.params.size(); ++i) {
                        if (out_.is_struct_type(m.params[i]) &&
                            e.args[i]->kind == ExprKind::Ident) {
                            const EscBinding* b = esc_find(e.args[i]->name);
                            if (b && b->let) forced_.push_back(b->let);
                        }
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    // ---- comptime evaluation -------------------------------------------------
    std::optional<Value> eval_comptime_call(Expr& e, size_t fi) {
        FnDecl& fn = mod_.fns[out_.fns[fi].ast_index];
        FlatMap<std::string, Value> env;
        for (size_t i = 0; i < e.args.size(); ++i) {
            std::optional<Value> av = eval_const(*e.args[i], ty_none());
            if (!av) return std::nullopt;
            env.insert(fn.params[i].first, *av);
        }
        steps_ = 0;
        return interpret_fn(fn, env);
    }

    std::optional<Value> eval_const(const Expr& e, TypeId expect) {
        (void)expect;
        switch (e.kind) {
            case ExprKind::IntLit:  return Value{e.ty, e.iv, 0};
            case ExprKind::FloatLit: return Value{e.ty, 0, e.fv};
            case ExprKind::BoolLit: return Value{ty_i1(), e.iv, 0};
            case ExprKind::Ident: {
                size_t ci = const_decl_index(e.sym);
                if (ci == SIZE_MAX) return std::nullopt;
                const ConstDecl& c = mod_.consts[ci];
                return Value{c.ty, c.iv, c.fv};
            }
            case ExprKind::Unary: {
                auto v = eval_const(*e.lhs, ty_none());
                if (!v) return std::nullopt;
                switch (e.uop) {
                    case UnKind::Neg:
                        if (val_is_fp(*v)) return Value{v->ty, 0, -v->fv};
                        return Value{v->ty, static_cast<u64>(-static_cast<i64>(v->iv)), 0};
                    case UnKind::Not:  return Value{ty_i1(), v->iv ? u64(0) : u64(1), 0};
                    case UnKind::BNot: return Value{v->ty, ~v->iv, 0};
                }
                return std::nullopt;
            }
            case ExprKind::Cast: {
                auto v = eval_const(*e.lhs, ty_none());
                if (!v) return std::nullopt;
                return cast_value(*v, e.cast_target);
            }
            case ExprKind::Binary: {
                auto l = eval_const(*e.lhs, ty_none());
                auto r = eval_const(*e.rhs, ty_none());
                if (!l || !r) return std::nullopt;
                return eval_binop(*l, *r, e.bop, e.pos);
            }
            case ExprKind::Field: {
                // qualified constant `Color.Red` / `Perm.Read` in comptime
                if (e.lhs && e.lhs->kind == ExprKind::Ident) {
                    const TypeId* t = out_.user_types.by_name.find(e.lhs->sym);
                    if (t) {
                        SymbolId vs = syms_.find(e.name);
                        const UserType* u = &out_.user_types.get(*t);
                        int depth = 0;
                        while (u->kind == UserKind::Alias && depth++ < 16)
                            u = &out_.user_types.get(u->target);
                        for (const auto& [vname, vval] : u->variants)
                            if (vname == vs) return Value{*t, vval, 0};
                    }
                }
                return std::nullopt;
            }
            case ExprKind::StructLit:
            case ExprKind::MethodCall:
            case ExprKind::AddrOf:
                return std::nullopt; // no struct/comptime interplay in MVP
            case ExprKind::Call: {
                size_t fi = fn_index(e.callee);
                if (fi == SIZE_MAX) return std::nullopt;
                FnDecl& fn = mod_.fns[out_.fns[fi].ast_index];
                FlatMap<std::string, Value> env;
                for (size_t i = 0; i < e.args.size(); ++i) {
                    auto av = eval_const(*e.args[i], ty_none());
                    if (!av) return std::nullopt;
                    env.insert(fn.params[i].first, *av);
                }
                u64 saved = steps_;
                steps_ = 0;
                auto res = interpret_fn(fn, env);
                steps_ = saved;
                return res;
            }
            case ExprKind::ComptimeBlock: {
                FlatMap<std::string, Value> env;
                u64 saved = steps_;
                steps_ = 0;
                auto res = interpret_stmts(e.block_body, env, ty_none());
                steps_ = saved;
                return res;
            }
            default: return std::nullopt;
        }
    }

    std::optional<Value> eval_binop(const Value& l, const Value& r, BinKind op, SourcePos pos) {
        // user integer-backed types: signedness/width from the backing lattice
        TypeId lt = out_.ir_ty(l.ty);
        TypeId rt = out_.ir_ty(r.ty);
        bool fp = ty_is_float(lt) || ty_is_float(rt);
        bool signed_cmp = ty_is_signed(lt) || ty_is_signed(rt);
        TypeId res_ty = out_.is_int_backed(l.ty) ? l.ty : lt;
        if (fp) {
            f64 a = ty_is_float(lt) ? l.fv : static_cast<f64>(static_cast<i64>(l.iv));
            f64 b = ty_is_float(rt) ? r.fv : static_cast<f64>(static_cast<i64>(r.iv));
            switch (op) {
                case BinKind::Add: return Value{ty_f64(), 0, a + b};
                case BinKind::Sub: return Value{ty_f64(), 0, a - b};
                case BinKind::Mul: return Value{ty_f64(), 0, a * b};
                case BinKind::Div:
                    if (b == 0.0) { diag_.error(pos, "comptime division by zero"); return std::nullopt; }
                    return Value{ty_f64(), 0, a / b};
                case BinKind::Eq: return Value{ty_i1(), a == b ? u64(1) : u64(0), 0};
                case BinKind::Ne: return Value{ty_i1(), a != b ? u64(1) : u64(0), 0};
                case BinKind::Lt: return Value{ty_i1(), a <  b ? u64(1) : u64(0), 0};
                case BinKind::Le: return Value{ty_i1(), a <= b ? u64(1) : u64(0), 0};
                case BinKind::Gt: return Value{ty_i1(), a >  b ? u64(1) : u64(0), 0};
                case BinKind::Ge: return Value{ty_i1(), a >= b ? u64(1) : u64(0), 0};
                default: return std::nullopt;
            }
        }
        // (signedness/width decided above from the backing lattice types)
        switch (op) {
            case BinKind::Add: return Value{res_ty, l.iv + r.iv, 0};
            case BinKind::Sub: return Value{res_ty, l.iv - r.iv, 0};
            case BinKind::Mul: return Value{res_ty, l.iv * r.iv, 0};
            case BinKind::Div:
            case BinKind::Mod: {
                if (r.iv == 0) { diag_.error(pos, "comptime division by zero"); return std::nullopt; }
                if (signed_cmp) {
                    i64 a = static_cast<i64>(l.iv), b = static_cast<i64>(r.iv);
                    if (op == BinKind::Div) return Value{res_ty, static_cast<u64>(a / b), 0};
                    return Value{res_ty, static_cast<u64>(a % b), 0};
                }
                if (op == BinKind::Div) return Value{res_ty, l.iv / r.iv, 0};
                return Value{res_ty, l.iv % r.iv, 0};
            }
            case BinKind::And: return Value{res_ty, l.iv & r.iv, 0};
            case BinKind::Or:  return Value{res_ty, l.iv | r.iv, 0};
            case BinKind::Xor: return Value{res_ty, l.iv ^ r.iv, 0};
            case BinKind::Shl: return Value{res_ty, l.iv << (r.iv & 63), 0};
            case BinKind::Shr: {
                if (signed_cmp) return Value{res_ty, static_cast<u64>(static_cast<i64>(l.iv) >> (r.iv & 63)), 0};
                return Value{res_ty, l.iv >> (r.iv & 63), 0};
            }
            case BinKind::LogicAnd: return Value{ty_i1(), (l.iv != 0 && r.iv != 0) ? u64(1) : u64(0), 0};
            case BinKind::LogicOr:  return Value{ty_i1(), (l.iv != 0 || r.iv != 0) ? u64(1) : u64(0), 0};
            case BinKind::Eq: return Value{ty_i1(), l.iv == r.iv ? u64(1) : u64(0), 0};
            case BinKind::Ne: return Value{ty_i1(), l.iv != r.iv ? u64(1) : u64(0), 0};
            case BinKind::Lt: return Value{ty_i1(), cmp_lt(l.iv, r.iv, signed_cmp) ? u64(1) : u64(0), 0};
            case BinKind::Le: return Value{ty_i1(), cmp_le(l.iv, r.iv, signed_cmp) ? u64(1) : u64(0), 0};
            case BinKind::Gt: return Value{ty_i1(), cmp_lt(r.iv, l.iv, signed_cmp) ? u64(1) : u64(0), 0};
            case BinKind::Ge: return Value{ty_i1(), cmp_le(r.iv, l.iv, signed_cmp) ? u64(1) : u64(0), 0};
        }
        return std::nullopt;
    }
    static bool cmp_lt(u64 a, u64 b, bool is_signed) {
        return is_signed ? static_cast<i64>(a) < static_cast<i64>(b) : a < b;
    }
    static bool cmp_le(u64 a, u64 b, bool is_signed) {
        return is_signed ? static_cast<i64>(a) <= static_cast<i64>(b) : a <= b;
    }

    std::optional<Value> cast_value(const Value& v, TypeId target) {
        // user types degrade to their backing lattice type for value math;
        // the RESULT carries the requested (possibly user) type
        TypeId in_t = out_.ir_ty(v.ty);
        TypeId out_t = out_.ir_ty(target);
        if (ty_is_float(in_t) && ty_is_float(out_t)) {
            return Value{target, 0, ty_bits(out_t) == 32 ? static_cast<f64>(static_cast<f32>(v.fv)) : v.fv};
        }
        if (ty_is_float(in_t) && ty_is_int(out_t)) {
            i64 iv = static_cast<i64>(v.fv);
            if (ty_bits(out_t) == 32) iv = static_cast<i32>(iv);
            return Value{target, static_cast<u64>(iv), 0};
        }
        if (ty_is_int(in_t) && ty_is_float(out_t)) {
            f64 fv = ty_is_signed(in_t) ? static_cast<f64>(static_cast<i64>(v.iv))
                                        : static_cast<f64>(v.iv);
            if (ty_bits(out_t) == 32) fv = static_cast<f64>(static_cast<f32>(fv));
            return Value{target, 0, fv};
        }
        if (ty_is_ptr(in_t) && ty_is_ptr(out_t)) return Value{target, v.iv, 0};
        // int -> int truncate/extend
        u64 raw = v.iv;
        if (ty_bits(out_t) == 32) {
            if (ty_is_signed(out_t))
                raw = static_cast<u64>(static_cast<i64>(static_cast<i32>(static_cast<u32>(raw))));
            else
                raw = static_cast<u32>(raw);
        } else if (ty_bits(in_t) == 32 && ty_is_signed(in_t)) {
            raw = static_cast<u64>(static_cast<i64>(static_cast<i32>(static_cast<u32>(raw))));
        }
        return Value{target, raw, 0};
    }

    // ---- comptime interpreter (pure subset) ------------------------------------
    std::optional<Value> interpret_fn(const FnDecl& fn, const FlatMap<std::string, Value>& env) {
        FlatMap<std::string, Value> locals = env;
        return interpret_stmts(fn.body, locals, fn.ret);
    }

    std::optional<Value> interpret_stmts(const std::vector<StmtP>& stmts,
                                         FlatMap<std::string, Value>& locals, TypeId ret_ty) {
        for (const StmtP& s : stmts) {
            auto r = interpret_stmt(*s, locals, ret_ty);
            if (r) return r; // return value propagated
        }
        if (ret_ty == ty_void()) return Value{ty_void(), 0, 0};
        return std::nullopt; // fell off the end (sema warns separately)
    }

    std::optional<Value> interpret_stmt(const Stmt& s, FlatMap<std::string, Value>& locals, TypeId ret_ty) {
        if (++steps_ > kComptimeStepLimit) {
            diag_.error(s.pos, "comptime evaluation exceeded the step limit (infinite loop?)");
            return Value{ty_void(), 0, 0};
        }
        switch (s.kind) {
            case StmtKind::Return:
                if (!s.value) return Value{ty_void(), 0, 0};
                return eval_env(*s.value, locals);
            case StmtKind::Let: {
                auto v = eval_env(*s.value, locals);
                if (!v) return std::nullopt;
                locals.insert(s.name, *v);
                return std::nullopt;
            }
            case StmtKind::Assign: {
                auto v = eval_env(*s.value, locals);
                if (!v) return std::nullopt;
                locals.insert(s.name, *v);
                return std::nullopt;
            }
            case StmtKind::If: {
                auto c = eval_cond(*s.cond, locals);
                if (!c) return std::nullopt;
                if (*c) return interpret_stmts(s.body, locals, ret_ty);
                return interpret_stmts(s.else_body, locals, ret_ty);
            }
            case StmtKind::While: {
                for (;;) {
                    if (++steps_ > kComptimeStepLimit) {
                        diag_.error(s.pos, "comptime evaluation exceeded the step limit (infinite loop?)");
                        return Value{ty_void(), 0, 0};
                    }
                    auto c = eval_cond(*s.cond, locals);
                    if (!c) return std::nullopt;
                    if (!*c) break;
                    auto r = interpret_stmts(s.body, locals, ret_ty);
                    if (r) return r;
                }
                return std::nullopt;
            }
            case StmtKind::For: {
                auto from = eval_env(*s.target, locals);
                auto to = eval_env(*s.to, locals);
                if (!from || !to) return std::nullopt;
                for (u64 i = from->iv; i < to->iv; ++i) {
                    if (++steps_ > kComptimeStepLimit) {
                        diag_.error(s.pos, "comptime evaluation exceeded the step limit (infinite loop?)");
                        return Value{ty_void(), 0, 0};
                    }
                    locals.insert(s.name, Value{from->ty, i, 0});
                    auto r = interpret_stmts(s.body, locals, ret_ty);
                    if (r) return r;
                }
                return std::nullopt;
            }
            default:
                return std::nullopt; // break/continue/expr-stmt have no comptime semantics
        }
    }

    std::optional<bool> eval_cond(const Expr& e, FlatMap<std::string, Value>& locals) {
        auto v = eval_env(e, locals);
        if (!v) return std::nullopt;
        return v->iv != 0;
    }

    // Environment-aware evaluator: like eval_const but let-bound locals are
    // visible at any depth (recursive comptime functions need this).
    std::optional<Value> eval_env(const Expr& e, const FlatMap<std::string, Value>& locals) {
        switch (e.kind) {
            case ExprKind::Ident: {
                if (const Value* v = locals.find(e.name)) return *v;
                return eval_const(e, ty_none());
            }
            case ExprKind::Unary: {
                auto a = eval_env(*e.lhs, locals);
                if (!a) return std::nullopt;
                switch (e.uop) {
                    case UnKind::Neg:
                        if (val_is_fp(*a)) return Value{a->ty, 0, -a->fv};
                        return Value{a->ty, static_cast<u64>(-static_cast<i64>(a->iv)), 0};
                    case UnKind::Not: return Value{ty_i1(), a->iv ? u64(0) : u64(1), 0};
                    case UnKind::BNot: return Value{a->ty, ~a->iv, 0};
                }
                return std::nullopt;
            }
            case ExprKind::Binary: {
                auto l = eval_env(*e.lhs, locals);
                auto r = eval_env(*e.rhs, locals);
                if (!l || !r) return std::nullopt;
                return eval_binop(*l, *r, e.bop, e.pos);
            }
            case ExprKind::Cast: {
                auto v = eval_env(*e.lhs, locals);
                if (!v) return std::nullopt;
                return cast_value(*v, e.cast_target);
            }
            case ExprKind::Call: {
                size_t fi = fn_index(e.callee);
                if (fi == SIZE_MAX) return std::nullopt;
                FnDecl& fn = mod_.fns[out_.fns[fi].ast_index];
                FlatMap<std::string, Value> env;
                for (size_t i = 0; i < e.args.size(); ++i) {
                    auto av = eval_env(*e.args[i], locals);
                    if (!av) return std::nullopt;
                    env.insert(fn.params[i].first, *av);
                }
                u64 saved = steps_;
                steps_ = 0;
                auto res = interpret_fn(fn, env);
                steps_ = saved;
                return res;
            }
            case ExprKind::ComptimeBlock:
                return eval_const(e, ty_none());
            default:
                return eval_const(e, ty_none());
        }
    }

    ModuleAst& mod_;
    SemaModule& out_;
    Diagnostics& diag_;
    SymbolTable& syms_;
    std::vector<Scope> scopes_;
    u64 steps_ = 0;
};

} // namespace

bool run_sema(ModuleAst& ast, SemaModule& out, Diagnostics& diag, SymbolTable& syms) {
    Sema s(ast, out, diag, syms);
    return s.run();
}

const char* ty_name(TypeId t) {
    if (t >= kUserTyBase) return "<user-type>"; // named in diagnostics via Sema
    switch (t) {
        case 0: return "<none>";
        case 1: return "void";
        case 2: return "bool";
        case 3: return "i32";
        case 4: return "i64";
        case 5: return "u32";
        case 6: return "u64";
        case 7: return "f32";
        case 8: return "f64";
        default: break;
    }
    const TypeDesc& d = kTypeTable[t < kTypeCount ? t : 0];
    if (d.ty == Ty::Ptr) {
        switch (d.pointee) {
            case Ty::I64: return "*i64";   // pointee only; constness is frontend-only
            case Ty::I32: return "*i32";
            case Ty::U64: return "*u64";
            case Ty::U32: return "*u32";
            case Ty::F32: return "*f32";
            case Ty::F64: return "*f64";
            case Ty::I1:  return "*bool";
            default: return "ptr";
        }
    }
    if (d.ty == Ty::Mem) return "<mem>";
    if (d.ty == Ty::Ctrl) return "<ctrl>";
    switch (d.ty) {
        case Ty::V2F64: return "v2f64";
        case Ty::V2I64: return "v2i64";
        case Ty::V4I32: return "v4i32";
        case Ty::V4F32: return "v4f32";
        default: break;
    }
    return "<bad>";
}

} // namespace jules
