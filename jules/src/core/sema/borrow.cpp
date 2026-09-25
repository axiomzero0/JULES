// Strict borrow checker implementation (see borrow.h for the contract).
//
// Execution model: a flow-sensitive abstract walk over the checked AST of
// every non-extern function, twice:
//   pass 1 (silent): computes each function's return-flow summary — the
//          bitmask of parameters whose values can reach the return value
//          (through bindings, struct fields, and moves);
//   pass 2 (reporting): enforces the ownership/borrow rules; call results
//          are classified through the summaries (owned fresh value vs.
//          alias of the argument that flows out).
//
// State discipline: `State` is a full snapshot (scope stack + origin table);
// branches walk copies and join; loops run a bounded fixpoint over the join
// (errors are only reported on the final, stabilized round). Origin ids are
// allocated per SYNTACTIC site (deterministic across rounds), which makes
// the fixpoint converge: `loop { let p = alloc(i64); free(p); }` resets the
// site's origin each round instead of poisoning it forever.
//
// comptime functions and comptime blocks are interpreter-managed (compiler
// memory, no runtime code) and are skipped by design.
#include "core/sema/borrow.h"

#include <set>

namespace jules {
namespace {

constexpr u32 kNoOrigin = 0;
constexpr u32 kMaxLoopRounds = 6;
constexpr u64 kAllBits = ~0ull;

// A tracked value. origin==0 means "untracked" (scalar, or a view whose
// provenance is an anonymous copy out of a heap-array slot).
struct FreezeRef { // what a &-view unfreezes when it dies
    u32 scope = 0, slot = 0;           // binding target
    u32 origin = kNoOrigin, field = 0; // heap-field target
    bool is_bind = true;
    bool valid = false;
    bool operator==(const FreezeRef& o) const {
        return valid == o.valid && is_bind == o.is_bind && scope == o.scope &&
               slot == o.slot && origin == o.origin && field == o.field;
    }
};

struct V {
    u32 origin = kNoOrigin;
    bool shared = false;   // read-only view (*const-derived / heap-slot copy)
    bool mut_view = false; // unique view (&x / call result aliasing an arg)
    bool fuzzy = false;    // joined from paths with different origins
    u64 pset = 0;          // parameter-derivation bitmask
    u32 borrowed = 0;      // live &-borrows targeting this (struct) value
    bool dead = false;     // moved out / freed (field slots only)
    FreezeRef freeze;      // &-view: what to unfreeze when it dies
    FlatMap<u32, V> fields; // struct payload, keyed by field index
};

struct Bind {
    std::string name;
    TypeId ty = ty_none();
    V val;
    bool dead = false;
    bool is_param = false;
    u32 frozen = 0;        // live &-borrows targeting this binding
    FreezeRef freeze_src; // set when this binding IS a &-view
};

struct Scope {
    std::vector<Bind> binds;
    std::vector<Stmt*> defers;
};

struct Origin {
    bool freed = false;
    u32 shared_live = 0;
    FlatMap<u32, V> heap_fields; // heap struct fields (per field index)
};

struct State {
    std::vector<Scope> scopes;
    FlatMap<u32, Origin> origins;
    std::set<u32> array_stored; // origins with views copied into heap-array slots
};

// ---- value helpers ----------------------------------------------------------

void acquire(State& st, const V& v) {
    if (v.dead) return;
    if (v.shared && v.origin != kNoOrigin) st.origins[v.origin].shared_live++;
    for (const auto& kv : v.fields.entries()) acquire(st, kv.second);
}
void release(State& st, const V& v) {
    if (v.dead) return;
    if (v.shared && v.origin != kNoOrigin) {
        Origin* o = st.origins.find(v.origin);
        if (o && o->shared_live > 0) o->shared_live--;
    }
    for (const auto& kv : v.fields.entries()) release(st, kv.second);
}
u64 full_pset(const V& v) {
    if (v.dead) return 0;
    u64 m = v.pset;
    for (const auto& kv : v.fields.entries()) m |= full_pset(kv.second);
    return m;
}
bool has_owned_field(const V& v) {
    for (const auto& kv : v.fields.entries()) {
        const V& f = kv.second;
        if (f.dead) continue;
        if (!f.shared && !f.mut_view && f.origin != kNoOrigin) return true;
        if (has_owned_field(f)) return true;
    }
    return false;
}
bool same_v(const V& a, const V& b) {
    if (a.origin != b.origin || a.shared != b.shared || a.mut_view != b.mut_view ||
        a.fuzzy != b.fuzzy || a.pset != b.pset || a.dead != b.dead ||
        a.borrowed != b.borrowed || !(a.freeze == b.freeze))
        return false;
    if (a.fields.size() != b.fields.size()) return false;
    for (size_t i = 0; i < a.fields.size(); ++i)
        if (a.fields.entries()[i].first != b.fields.entries()[i].first ||
            !same_v(a.fields.entries()[i].second, b.fields.entries()[i].second))
            return false;
    return true;
}

class Checker {
public:
    Checker(ModuleAst& mod, SemaModule& out, Diagnostics& diag, SymbolTable& syms)
        : mod_(mod), out_(out), diag_(diag), syms_(syms) {}

    bool run() {
        report_ = false;
        for (FnDecl& fn : mod_.fns) {
            if (fn.is_extern || fn.is_comptime) continue;
            u64 flow = 0;
            ret_union_ = &flow;
            enter_fn(fn);
            ret_flow_.insert(fn.name, flow);
        }
        report_ = true;
        ret_union_ = nullptr;
        for (FnDecl& fn : mod_.fns) {
            if (fn.is_extern || fn.is_comptime) continue;
            enter_fn(fn);
        }
        return !diag_.has_errors();
    }

private:
    ModuleAst& mod_;
    SemaModule& out_;
    Diagnostics& diag_;
    SymbolTable& syms_;
    FlatMap<SymbolId, u64> ret_flow_;
    FlatMap<const void*, u32> site_origin_;
    u64* ret_union_ = nullptr;
    bool report_ = true;
    bool unreachable_ = false;
    FnDecl* fn_ = nullptr;

    struct LoopCtx {
        std::vector<State> breaks, conts;
        size_t scope_depth = 0;
    };
    std::vector<LoopCtx*> loops_;

    void err(SourcePos pos, const std::string& msg) {
        if (report_) diag_.error(pos, msg);
    }


    // ---- type helpers ------------------------------------------------------
    bool is_ptr_like(TypeId t) const { return ty_is_ptr(t) || out_.is_struct_ptr(t); }
    TypeId struct_of_ty(TypeId t) const {
        if (!ty_is_user(t)) return ty_none(); // user_types.get() is unguarded
        TypeId s = out_.user_types.struct_of(t);
        if (s == ty_none()) s = out_.user_types.pointee_struct(t);
        return s;
    }
    size_t field_index(TypeId struct_ty, const std::string& fname) const {
        TypeId s = struct_of_ty(struct_ty);
        if (s == ty_none()) return SIZE_MAX;
        const UserType& u = out_.user_types.get(s);
        for (size_t i = 0; i < u.fields.size(); ++i)
            if (std::string(syms_.name(u.fields[i].name)) == fname) return i;
        return SIZE_MAX;
    }
    const StructField* field_at(TypeId struct_ty, size_t idx) const {
        TypeId s = struct_of_ty(struct_ty);
        if (s == ty_none()) return nullptr;
        const UserType& u = out_.user_types.get(s);
        return idx < u.fields.size() ? &u.fields[idx] : nullptr;
    }

    u32 site_origin(const void* site) {
        if (const u32* p = site_origin_.find(site)) return *p;
        u32 id = static_cast<u32>(site_origin_.size() + 1);
        site_origin_.insert(site, id);
        return id;
    }

    void seed_fields(V& v, TypeId struct_ty, bool shared, u64 pset) {
        TypeId s = struct_of_ty(struct_ty);
        if (s == ty_none()) return;
        const UserType& u = out_.user_types.get(s);
        for (size_t i = 0; i < u.fields.size(); ++i) {
            const StructField& f = u.fields[i];
            if (!is_ptr_like(f.ty) && struct_of_ty(f.ty) == ty_none()) continue;
            V sub;
            sub.pset = pset;
            if (is_ptr_like(f.ty)) {
                if (shared) sub.shared = true;
                else sub.origin = site_origin(&f);
            } else if (!shared) {
                sub.origin = site_origin(&f);
            }
            seed_fields(sub, f.ty, shared, pset);
            v.fields.insert(static_cast<u32>(i), sub);
        }
    }
    void seed_heap_fields(State& st, u32 origin, TypeId struct_ty) {
        Origin& o = st.origins[origin];
        if (!o.heap_fields.empty()) return;
        TypeId s = struct_of_ty(struct_ty);
        if (s == ty_none()) return;
        const UserType& u = out_.user_types.get(s);
        for (size_t i = 0; i < u.fields.size(); ++i) {
            const StructField& f = u.fields[i];
            if (!is_ptr_like(f.ty) && struct_of_ty(f.ty) == ty_none()) continue;
            V sub;
            sub.shared = true; // unwritten heap field reads as a view
            seed_fields(sub, f.ty, true, 0);
            o.heap_fields.insert(static_cast<u32>(i), sub);
        }
    }

    // ---- bindings ----------------------------------------------------------
    Bind* find_bind(State& st, const std::string& n) {
        for (auto it = st.scopes.rbegin(); it != st.scopes.rend(); ++it)
            for (auto b = it->binds.rbegin(); b != it->binds.rend(); ++b)
                if (b->name == n) return &*b;
        return nullptr;
    }
    Bind* declare(State& st, const std::string& n, TypeId ty, V v, bool is_param) {
        (void)st;
        Scope& sc = st.scopes.back();
        sc.binds.emplace_back();
        Bind& b = sc.binds.back();
        b.name = n;
        b.ty = ty;
        b.val = std::move(v);
        b.is_param = is_param;
        return &b;
    }
    void ensure_struct_origin(State& st, Bind& b) {
        if (struct_of_ty(b.ty) == ty_none()) return;
        if (b.val.origin == kNoOrigin) b.val.origin = site_origin(&b);
        if (b.val.fields.empty()) seed_fields(b.val, b.ty, b.is_param, b.val.pset);
        (void)st;
    }
    FreezeRef bind_ref(State& st, Bind* b) {
        FreezeRef r;
        r.valid = b != nullptr;
        if (!r.valid) return r;
        for (size_t si = 0; si < st.scopes.size(); ++si)
            for (size_t bi = 0; bi < st.scopes[si].binds.size(); ++bi)
                if (&st.scopes[si].binds[bi] == b) {
                    r.scope = static_cast<u32>(si);
                    r.slot = static_cast<u32>(bi);
                    return r;
                }
        r.valid = false;
        return r;
    }
    Bind* deref_bind_ref(State& st, const FreezeRef& r) {
        if (!r.valid || !r.is_bind) return nullptr;
        if (r.scope >= st.scopes.size()) return nullptr;
        if (r.slot >= st.scopes[r.scope].binds.size()) return nullptr;
        return &st.scopes[r.scope].binds[r.slot];
    }

    // apply / release the freeze a &-view carries when stored into a binding
    void store_view_freeze(State& st, V& v, Bind* receiver) {
        if (!v.freeze.valid || !receiver) return;
        if (Bind* target = deref_bind_ref(st, v.freeze))
            target->frozen++;
        if (v.freeze.is_bind) receiver->freeze_src = v.freeze;
    }
    void unfreeze(State& st, const FreezeRef& f) {
        if (!f.valid) return;
        if (f.is_bind) {
            if (Bind* b = deref_bind_ref(st, f))
                if (b->frozen > 0) b->frozen--;
        } else {
            Origin* o = st.origins.find(f.origin);
            if (o)
                if (V* fld = o->heap_fields.find(f.field))
                    if (fld->borrowed > 0) fld->borrowed--;
        }
    }

    // ---- function entry ----------------------------------------------------
    void enter_fn(FnDecl& fn) {
        fn_ = &fn;
        unreachable_ = false;
        State st;
        st.scopes.push_back(Scope{});
        for (size_t i = 0; i < fn.params.size(); ++i) {
            auto& [pname, pty] = fn.params[i];
            V v;
            if (is_ptr_like(pty)) {
                v.origin = site_origin(&fn.params[i]);
                v.pset = 1ull << i;
            } else if (struct_of_ty(pty) != ty_none()) {
                v.pset = 1ull << i;
                seed_fields(v, pty, true, v.pset);
            }
            declare(st, pname, pty, std::move(v), true);
        }
        walk_stmts(fn.body, st);
        // fall-off-the-end path: fn-scope defers still run (return pops all
        // scopes itself; this covers the implicit-return shape)
        while (!st.scopes.empty()) exit_scope(st);
    }

    // ---- statement walk ----------------------------------------------------
    // Returns true if control falls through the whole list.
    bool walk_stmts(std::vector<StmtP>& list, State& st) {
        bool saved = unreachable_;
        unreachable_ = false;
        bool fell = true;
        for (StmtP& s : list) {
            if (unreachable_) { fell = false; continue; }
            walk_stmt(*s, st);
        }
        if (unreachable_) fell = false;
        // B4 fix: a list whose paths all diverged (return/break/continue)
        // must leave the caller's position marked unreachable — the outer
        // list's remaining statements (and scope machinery) must not walk
        // into a state whose scopes were already popped.
        unreachable_ = saved || !fell;
        return fell;
    }

    void walk_stmt(Stmt& s, State& st) {
        switch (s.kind) {
            case StmtKind::Let: {
                if (!s.value) break;
                V v = eval_take(*s.value, st);
                acquire(st, v);
                Bind* b = declare(st, s.name, s.decl_ty, std::move(v), false);
                store_view_freeze(st, b->val, b);
                break;
            }
            case StmtKind::Assign: {
                Bind* b = find_bind(st, s.name);
                if (!b) break;
                if (b->frozen) {
                    err(s.pos, "cannot assign to '" + s.name +
                        "': it is uniquely borrowed (an '&' view is live)");
                    break;
                }
                if (!s.value) break;
                V v = eval_take(*s.value, st);
                release(st, b->val);
                unfreeze(st, b->freeze_src);
                acquire(st, v);
                b->val = std::move(v);
                b->dead = false;
                store_view_freeze(st, b->val, b);
                break;
            }
            case StmtKind::AssignDeref: {
                if (!s.target) break;
                if (out_.is_struct_ptr(s.target->ty)) {
                    V base = eval_use(*s.target, st);
                    if (!s.value) break;
                    V v = eval_take(*s.value, st);
                    heap_struct_store(s.pos, base, v, st);
                } else {
                    V ptr = place_val(*s.target, st);
                    check_write_through(s.pos, ptr, st, "*ptr");
                    if (s.value) eval_use(*s.value, st);
                }
                break;
            }
            case StmtKind::AssignIndex: {
                if (!s.target) break;
                V ptr = place_val(*s.target, st);
                check_write_through(s.pos, ptr, st, "indexed store");
                if (s.to) eval_use(*s.to, st);
                if (!s.value) break;
                if (out_.is_struct_ptr(s.target->ty)) {
                    V v = eval_take(*s.value, st);
                    if (has_owned_field(v))
                        err(s.pos, "cannot move an owned pointer into a heap-array "
                            "slot (no drop runs on array slots in the MVP; store a "
                            "shared view instead, e.g. 'x as *const T')");
                    register_array_views(st, v);
                } else {
                    eval_use(*s.value, st);
                }
                break;
            }
            case StmtKind::AssignField: {
                walk_assign_field(s, st);
                break;
            }
            case StmtKind::Return: {
                if (s.value) {
                    V v = eval_take(*s.value, st);
                    if (v.mut_view && v.freeze.valid && v.freeze.is_bind) {
                        if (Bind* tgt = deref_bind_ref(st, v.freeze)) {
                            if (!tgt->is_param) {
                                err(s.pos, "cannot return a borrow of a local "
                                    "('&' of a local outlives the function)");
                                break;
                            }
                        }
                    }
                    if (ret_union_) *ret_union_ |= full_pset(v);
                }
                exit_all_scopes(st);
                unreachable_ = true;
                break;
            }
            case StmtKind::If: {
                if (s.cond) eval_use(*s.cond, st);
                State th = st, el = st;
                bool th_fell, el_fell;
                {
                    th.scopes.push_back(Scope{});
                    th_fell = walk_stmts(s.body, th);
                    if (th_fell) exit_scope(th);
                }
                {
                    el.scopes.push_back(Scope{});
                    el_fell = walk_stmts(s.else_body, el);
                    if (el_fell) exit_scope(el);
                }
                if (th_fell && el_fell) join_into(th, el);
                else if (el_fell) th = el;
                else if (!th_fell) unreachable_ = true;
                // the branch walks may have left unreachable_ set (a path
                // that always diverges): the join decides reachability
                if (th_fell || el_fell) unreachable_ = false;
                st = th;
                break;
            }
            case StmtKind::While:
                walk_loop(s.cond.get(), s.body, st, true);
                break;
            case StmtKind::For: {
                if (s.target) eval_use(*s.target, st);
                if (s.to) eval_use(*s.to, st);
                walk_loop(nullptr, s.body, st, false);
                break;
            }
            case StmtKind::ExprStmt:
                if (s.value) eval_use(*s.value, st);
                break;
            case StmtKind::Break:
            case StmtKind::Continue: {
                State snap = st;
                if (!loops_.empty()) {
                    LoopCtx* lc = loops_.back();
                    while (snap.scopes.size() > lc->scope_depth)
                        exit_scope(snap); // defers run on the jump path
                    if (s.kind == StmtKind::Break) lc->breaks.push_back(snap);
                    else lc->conts.push_back(snap);
                }
                unreachable_ = true;
                break;
            }
            case StmtKind::Defer:
                st.scopes.back().defers.push_back(&s);
                break;
            case StmtKind::Block: {
                st.scopes.push_back(Scope{});
                walk_stmts(s.body, st);
                exit_scope(st);
                break;
            }
        }
    }

    void walk_loop(Expr* cond, std::vector<StmtP>& body, State& st, bool is_while) {
        State cur = st;
        LoopCtx lc;
        lc.scope_depth = st.scopes.size();
        auto one_round = [&](State& t, bool rep) -> bool {
            lc.breaks.clear();
            lc.conts.clear();
            bool saved = report_;
            if (!rep) report_ = false;
            bool saved_unr = unreachable_;
            unreachable_ = false;
            t.scopes.push_back(Scope{});
            loops_.push_back(&lc);
            bool fell = walk_stmts(body, t);
            loops_.pop_back();
            bool completed = fell && !unreachable_;
            unreachable_ = saved_unr;
            report_ = saved;
            if (completed) exit_scope(t);
            return completed;
        };
        for (u32 round = 0; round < kMaxLoopRounds; ++round) {
            State t = cur;
            bool completed = one_round(t, false);
            State nxt = cur;
            if (completed) join_into(nxt, t);
            for (State& c : lc.conts) join_into(nxt, c);
            if (same_state(nxt, cur)) break;
            cur = nxt;
        }
        if (is_while && cond) eval_use(*cond, cur);
        {
            State t = cur;
            bool completed = one_round(t, true);
            State after = cur;
            if (completed) join_into(after, t);
            for (State& c : lc.conts) join_into(after, c);
            for (State& b : lc.breaks) join_into(after, b);
            st = after;
        }
    }

    void walk_assign_field(Stmt& s, State& st) {
        if (!s.target || !s.value) return;
        TypeId bt = s.target->ty;
        if (struct_of_ty(bt) == ty_none()) { // bitfield segments: u64 stores
            eval_use(*s.value, st);
            return;
        }
        size_t fidx = field_index(bt, s.name);
        const StructField* fd = field_at(bt, fidx);
        if (!fd) return;
        bool heap = out_.is_struct_ptr(bt);
        V base = eval_use(*s.target, st); // base liveness + origin snapshot
        if (is_ptr_like(fd->ty) || struct_of_ty(fd->ty) != ty_none()) {
            // B5 fix: a pointer-bearing store through an unresolvable base
            // (heap-array element: arr[i].f = ...) has the same no-drop
            // problem as a whole-struct array store — reject explicitly
            if (!out_.is_struct_ptr(bt) && root_bind(st, *s.target) == nullptr) {
                V vv = eval_take(*s.value, st);
                err(s.pos, "cannot store a pointer-bearing value through a "
                    "heap-array element (no drop runs on array slots in the "
                    "MVP; store a shared view instead, e.g. 'x as *const T')");
                return;
            }
            V v = eval_take(*s.value, st);
            if (v.mut_view)
                err(s.pos, "cannot store a borrow of a local ('&' value) into a "
                    "struct field — the local dies first");
            if (v.pset != 0 && !v.shared)
                err(s.pos, "cannot store a borrowed (parameter) pointer into a "
                    "struct field — it does not outlive the call");
            if (heap) {
                Origin* o = st.origins.find(base.origin);
                if (o) {
                    if (o->freed) { err(s.pos, "use after free of the base struct"); return; }
                    if (V* fld = o->heap_fields.find(static_cast<u32>(fidx))) {
                        if (fld->borrowed) {
                            err(s.pos, "cannot write field '" + s.name +
                                "': it is uniquely borrowed (an '&' view is live)");
                            return;
                        }
                        release(st, *fld);
                    }
                    if (v.shared && v.origin != kNoOrigin) acquire(st, v);
                    o->heap_fields.insert(static_cast<u32>(fidx), v);
                }
            } else {
                Bind* b = root_bind(st, *s.target);
                if (b) {
                    if (b->dead) { err(s.pos, "use after move of '" + b->name + "'"); return; }
                    if (b->frozen) {
                        err(s.pos, "cannot write a field of '" + b->name +
                            "': it is uniquely borrowed (an '&' view is live)");
                        return;
                    }
                    if (V* fld = b->val.fields.find(static_cast<u32>(fidx))) {
                        if (fld->dead) {
                            err(s.pos, "field '" + s.name + "' of '" + b->name +
                                "' was moved out or freed");
                            return;
                        }
                        release(st, *fld);
                    }
                    if (v.shared && v.origin != kNoOrigin) acquire(st, v);
                    b->val.fields.insert(static_cast<u32>(fidx), v);
                    b->dead = false;
                }
            }
        } else {
            // scalar field: a write — the base must not be frozen/borrowed
            if (!heap) {
                Bind* b = root_bind(st, *s.target);
                if (b && b->frozen)
                    err(s.pos, "cannot write a field of '" + b->name +
                        "': it is uniquely borrowed (an '&' view is live)");
            } else {
                Origin* o = st.origins.find(base.origin);
                if (o && !o->freed)
                    if (V* fld = o->heap_fields.find(static_cast<u32>(fidx)))
                        if (fld->borrowed)
                            err(s.pos, "cannot write field '" + s.name +
                                "': it is uniquely borrowed");
            }
            eval_use(*s.value, st);
        }
    }

    Bind* root_bind(State& st, Expr& e) {
        Expr* p = &e;
        for (int depth = 0; depth < 64 && p; ++depth) {
            if (p->kind == ExprKind::Ident) return find_bind(st, p->name);
            if (p->kind == ExprKind::Field || p->kind == ExprKind::MethodCall ||
                p->kind == ExprKind::Unary) {
                p = p->lhs.get();
                continue;
            }
            break;
        }
        return nullptr;
    }

    // ---- scope exits --------------------------------------------------------
    void exit_scope(State& st) {
        if (st.scopes.empty()) return;
        // Snapshot the defer list: a defer body may push scopes (Block /
        // If), which reallocates the scope vector and would dangle the
        // reference mid-iteration (the same class of bug the Task-26
        // review fixed in the frontend's own defer replay).
        std::vector<Stmt*> ds = st.scopes.back().defers;
        for (auto it = ds.rbegin(); it != ds.rend(); ++it)
            for (StmtP& d : (*it)->body)
                walk_stmt(*d, st); // the defer's BODY (nested defers are
                                   // rejected by the frontend already)
        drop_scope_binds(st);
    }
    void drop_scope_binds(State& st) {
        if (st.scopes.empty()) return;
        Scope& sc = st.scopes.back();
        for (Bind& b : sc.binds) {
            release(st, b.val);
            unfreeze(st, b.freeze_src);
        }
        st.scopes.pop_back();
    }
    void exit_all_scopes(State& st) {
        while (!st.scopes.empty())
            exit_scope(st);
    }
    void register_array_views(State& st, const V& v) {
        for (const auto& kv : v.fields.entries()) {
            if (kv.second.shared && kv.second.origin != kNoOrigin)
                st.array_stored.insert(kv.second.origin);
            register_array_views(st, kv.second);
        }
    }

    // ---- enforcement helpers ------------------------------------------------
    void check_write_through(SourcePos pos, const V& ptr, State& st, const char* what) {
        if (ptr.shared) {
            err(pos, std::string("cannot write through a shared (*const) view (") +
                what + ")");
            return;
        }
        if (ptr.origin == kNoOrigin) return; // untracked/fuzzy owner
        Origin* o = st.origins.find(ptr.origin);
        if (o && o->freed) { err(pos, "use after free"); return; }
        if (ptr.mut_view) return; // the unique view itself
        if (o && o->shared_live > 0) {
            err(pos, "cannot write: shared (*const) views of the allocation are live");
            return;
        }
        if (has_live_alias(st, ptr.origin)) {
            err(pos, "cannot write: the allocation is uniquely borrowed (a "
                "call-result alias or '&' view is live)");
            return;
        }
    }
    bool has_live_alias(State& st, u32 origin) {
        for (Scope& sc : st.scopes)
            for (Bind& b : sc.binds)
                if (!b.dead && b.val.mut_view && b.val.origin == origin) return true;
        return false;
    }
    void heap_struct_store(SourcePos pos, const V& base, V& v, State& st) {
        if (base.origin == kNoOrigin) return;
        Origin* o = st.origins.find(base.origin);
        if (!o) return;
        if (o->freed) { err(pos, "use after free of the destination"); return; }
        if (o->shared_live > 0) {
            err(pos, "cannot store into a heap struct with live shared views");
            return;
        }
        for (auto& kv : o->heap_fields.entries()) release(st, kv.second);
        o->heap_fields.clear();
        for (auto& kv : v.fields.entries()) o->heap_fields.insert(kv.first, kv.second);
        for (auto& kv : v.fields.entries())
            if (kv.second.shared && kv.second.origin != kNoOrigin)
                acquire(st, kv.second);
    }

    // ---- place resolution ---------------------------------------------------
    // The STORED value of a pointer/struct-pointer place (not view-ified).
    V place_val(Expr& e, State& st) {
        if (e.kind == ExprKind::Ident) {
            Bind* b = find_bind(st, e.name);
            if (!b) return V{};
            if (b->dead) {
                report_dead_use(e.pos, *b, st);
                return V{};
            }
            return b->val;
        }
        if (e.kind == ExprKind::Field) {
            FieldPlace fp;
            if (resolve_field_place(e, st, fp)) {
                if (fp.heap) {
                    Origin* o = st.origins.find(fp.origin);
                    if (o)
                        if (V* f = o->heap_fields.find(fp.field)) return *f;
                    return V{};
                }
                Bind* b = fp.bind;
                if (b->dead) { report_dead_use(e.pos, *b, st); return V{}; }
                if (V* f = b->val.fields.find(fp.field)) return *f;
                return V{};
            }
            return eval_use(e, st);
        }
        if (e.kind == ExprKind::Cast) {
            // place through a cast: use-view (non-const casts do not consume
            // in place context; the write targets the same allocation)
            if (e.lhs) return eval_use(*e.lhs, st);
            return V{};
        }
        return eval_use(e, st);
    }

    struct FieldPlace {
        Bind* bind = nullptr;    // local struct binding
        u32 field = 0;
        u32 origin = kNoOrigin;  // heap struct identity
        bool heap = false;
        bool ok = false;
    };

    // Resolve base.f where base is a struct value (local) or struct-ptr.
    bool resolve_field_place(Expr& e, State& st, FieldPlace& out) {
        if (e.kind != ExprKind::Field || !e.lhs) return false;
        // field lookup goes through the BASE's type (auto-deref included);
        // e.ty is the field's own type
        size_t fidx = field_index(e.lhs->ty, e.name);
        if (fidx == SIZE_MAX) return false;
        TypeId bt = e.lhs->ty;
        if (out_.is_struct_ptr(bt)) {
            V base = place_val(*e.lhs, st);
            if (base.origin == kNoOrigin) { out.ok = true; out.heap = true; out.origin = kNoOrigin; out.field = 0; return true; }
            Origin* o = st.origins.find(base.origin);
            if (!o) { out.ok = true; out.heap = true; out.origin = kNoOrigin; out.field = 0; return true; }
            if (o->freed) report_freed(e.pos);
            seed_heap_fields(st, base.origin, bt);
            out.ok = true;
            out.heap = true;
            out.origin = base.origin;
            out.field = static_cast<u32>(fidx);
            return true;
        }
        if (struct_of_ty(bt) != ty_none()) {
            Bind* b = root_bind(st, *e.lhs);
            if (!b) return false;
            if (b->dead) { report_dead_use(e.pos, *b, st); return false; }
            ensure_struct_origin(st, *b);
            out.ok = true;
            out.heap = false;
            out.bind = b;
            out.field = static_cast<u32>(fidx);
            return true;
        }
        return false;
    }

    void report_dead_use(SourcePos pos, Bind& b, State& st) {
        if (b.val.origin != kNoOrigin) {
            if (const Origin* o = st.origins.find(b.val.origin))
                if (o->freed) {
                    err(pos, "use after free of '" + b.name + "'");
                    return;
                }
        }
        err(pos, "use of moved value '" + b.name + "'");
    }

    void report_freed(SourcePos pos) { err(pos, "use after free"); }

    V join_v(V a, const V& b) {
        V r;
        r.dead = a.dead || b.dead;
        r.shared = a.shared || b.shared;
        r.mut_view = a.mut_view || b.mut_view;
        r.pset = a.pset | b.pset;
        r.borrowed = a.borrowed > b.borrowed ? a.borrowed : b.borrowed;
        if (a.origin != b.origin) {
            r.fuzzy = true;
            r.origin = a.origin; // kept for bookkeeping; fuzzy gates checks
        } else {
            r.origin = a.origin;
            r.fuzzy = a.fuzzy || b.fuzzy;
        }
        if (a.freeze == b.freeze) r.freeze = a.freeze;
        // field maps: keys are stable (seeded); join per key
        FlatMap<u32, V> nf;
        for (size_t i = 0; i < a.fields.size(); ++i) {
            const auto& ae = a.fields.entries()[i];
            V x = ae.second;
            if (const V* fb = b.fields.find(ae.first)) x = join_v(x, *fb);
            else x.dead = true; // conservative
            nf.insert(ae.first, x);
        }
        r.fields = nf;
        return r;
    }
    Bind join_bind(Bind a, const Bind& b) {
        a.dead = a.dead || b.dead;
        a.frozen = a.frozen > b.frozen ? a.frozen : b.frozen;
        a.val = join_v(a.val, b.val);
        return a;
    }

    friend struct V;

    // ---- eval_use: non-moving read ---------------------------------------
    V eval_use(Expr& e, State& st) {
        switch (e.kind) {
            case ExprKind::IntLit:
            case ExprKind::FloatLit:
            case ExprKind::BoolLit:
            case ExprKind::ComptimeBlock:
                return V{};
            case ExprKind::Ident: {
                if (e.comptime_value) return V{};
                Bind* b = find_bind(st, e.name);
                if (!b) return V{};
                if (b->dead) {
                    report_dead_use(e.pos, *b, st);
                    return V{};
                }
                if (b->frozen) {
                    err(e.pos, "cannot use '" + b->name +
                        "' while it is uniquely borrowed (an '&' view is live)");
                    return V{};
                }
                return b->val;
            }
            case ExprKind::Unary: {
                // Neg/Not/BNot: plain operand reads (deref is ExprKind::Deref)
                if (e.lhs) eval_use(*e.lhs, st);
                return V{};
            }
            case ExprKind::Deref: {
                // *p: a read through the pointer (place semantics)
                if (!e.lhs) return V{};
                V p = place_val(*e.lhs, st);
                if (p.origin != kNoOrigin) {
                    Origin* o = st.origins.find(p.origin);
                    if (o && o->freed) { err(e.pos, "use after free"); return V{}; }
                }
                if (out_.is_struct_ptr(e.ty)) {
                    // whole struct read out of a heap struct: view-ified copy
                    V r;
                    r.origin = p.origin;
                    if (p.origin != kNoOrigin) {
                        seed_heap_fields(st, p.origin, e.ty);
                        if (Origin* o = st.origins.find(p.origin)) {
                            if (o->freed) { err(e.pos, "use after free"); return V{}; }
                            for (auto& kv : o->heap_fields.entries()) {
                                V f = kv.second;
                                if (!f.dead && f.origin != kNoOrigin && !f.shared)
                                    f.shared = true; // heap copies are views
                                r.fields.insert(kv.first, f);
                            }
                        }
                    }
                    return r;
                }
                return V{}; // scalar pointee load
            }
            case ExprKind::Binary: {
                if (e.lhs) eval_use(*e.lhs, st);
                if (e.rhs) eval_use(*e.rhs, st);
                return V{};
            }
            case ExprKind::Call:
            case ExprKind::MethodCall:
                return eval_call(e, st);
            case ExprKind::Cast:
                return eval_cast(e, st, false);
            case ExprKind::Index: {
                V base = place_val(*e.lhs, st);
                if (e.rhs) eval_use(*e.rhs, st);
                if (base.origin != kNoOrigin) {
                    Origin* o = st.origins.find(base.origin);
                    if (o && o->freed) { err(e.pos, "use after free"); return V{}; }
                }
                if (out_.is_struct_ptr(e.ty)) {
                    // struct copy out of a heap ARRAY slot: anonymous views
                    V r;
                    r.pset = base.pset;
                    seed_fields(r, e.ty, true, base.pset);
                    return r;
                }
                return V{};
            }
            case ExprKind::Field: {
                if (e.comptime_value) return V{}; // Enum.Variant folds
                FieldPlace fp;
                if (!resolve_field_place(e, st, fp)) {
                    if (e.lhs) eval_use(*e.lhs, st);
                    return V{};
                }
                if (fp.heap) {
                    if (fp.origin == kNoOrigin) return V{};
                    Origin* o = st.origins.find(fp.origin);
                    if (!o) return V{};
                    if (o->freed) { err(e.pos, "use after free"); return V{}; }
                    V f{};
                    if (V* stored = o->heap_fields.find(fp.field)) f = *stored;
                    if (!f.dead && f.origin != kNoOrigin && !f.shared && !f.mut_view)
                        f.shared = true; // copies out of the heap are views
                    return f;
                }
                Bind* b = fp.bind;
                if (b->dead) { report_dead_use(e.pos, *b, st); return V{}; }
                if (b->frozen) {
                    err(e.pos, "cannot use '" + b->name +
                        "' while it is uniquely borrowed");
                    return V{};
                }
                if (V* f = b->val.fields.find(fp.field)) {
                    if (f->dead) {
                        err(e.pos, "field '" + e.name + "' of '" + b->name +
                            "' was moved out or freed");
                        return V{};
                    }
                    return *f;
                }
                return V{};
            }
            case ExprKind::StructLit: {
                V r;
                r.origin = site_origin(&e);
                seed_fields(r, e.ty, false, 0);
                for (size_t i = 0; i < e.args.size(); ++i)
                    if (e.args[i]) eval_use(*e.args[i], st);
                return r;
            }
            case ExprKind::AddrOf: {
                // unique borrow: the view freezes its target when STORED
                // (let/assign/field); transient uses never freeze.
                Bind* b = root_bind(st, *e.lhs);
                if (!b) return V{};
                if (b->dead) { report_dead_use(e.pos, *b, st); return V{}; }
                if (b->frozen) {
                    err(e.pos, "cannot take a second unique borrow ('&') of '" +
                        b->name + "' while one is live");
                    return V{};
                }
                ensure_struct_origin(st, *b);
                V r;
                r.mut_view = true;
                r.pset = b->val.pset;
                if (e.lhs->kind == ExprKind::Field) {
                    // &s.f: borrow of the field slot (origin), freeze root
                    FieldPlace fp;
                    if (resolve_field_place(*e.lhs, st, fp) && !fp.heap)
                        if (V* f = fp.bind->val.fields.find(fp.field))
                            if (f->origin != kNoOrigin) r.origin = f->origin;
                }
                if (r.origin == kNoOrigin) r.origin = b->val.origin;
                r.freeze = bind_ref(st, b);
                return r;
            }
        }
        return V{};
    }

    V eval_cast(Expr& e, State& st, bool take) {
        if (!e.lhs) return V{};
        bool to_ptr = is_ptr_like(e.cast_target) ||
                      (ty_is_user(e.cast_target) &&
                       out_.user_types.kind_of(e.cast_target) == UserKind::StructPtr);
        bool from_ptr = is_ptr_like(e.lhs->ty);
        if (to_ptr && from_ptr) {
            if (e.cast_ptr_const) {
                // shared view: copyable read-only alias
                V v = place_val(*e.lhs, st);
                if (v.origin != kNoOrigin) {
                    if (Origin* o = st.origins.find(v.origin))
                        if (o->freed) { err(e.pos, "use after free"); return V{}; }
                }
                V r = v;
                r.shared = true;
                r.mut_view = false;
                return r;
            }
            // *mut relabel: transfers ownership
            if (take) return eval_take(*e.lhs, st);
            return place_val(*e.lhs, st);
        }
        if (to_ptr && !from_ptr) {
            err(e.pos, "cannot cast an integer to a pointer in strict mode "
                "(unpredictable provenance)");
            return V{};
        }
        // ptr -> int (inert) and scalar casts
        return eval_use(*e.lhs, st);
    }

    // ---- eval_take: moving read (storage targets) -------------------------
    V eval_take(Expr& e, State& st) {
        switch (e.kind) {
            case ExprKind::Ident: {
                Bind* b = find_bind(st, e.name);
                if (!b) return V{};
                if (b->dead) {
                    report_dead_use(e.pos, *b, st);
                    return V{};
                }
                if (b->frozen) {
                    err(e.pos, "cannot move '" + b->name +
                        "' out while it is uniquely borrowed");
                    return V{};
                }
                V v = b->val;
                // whole-struct moves carry their fields
                if (struct_of_ty(b->ty) != ty_none())
                    for (auto& kv : v.fields.entries())
                        if (kv.second.dead) {
                            err(e.pos, "cannot move '" + b->name +
                                "': field '" + field_name_of(b->ty, kv.first) +
                                "' was moved out or freed");
                            break;
                        }
                b->dead = true;
                return v;
            }
            case ExprKind::Field: {
                if (e.comptime_value) return V{};
                FieldPlace fp;
                if (!resolve_field_place(e, st, fp)) return eval_use(e, st);
                if (fp.heap) {
                    if (fp.origin == kNoOrigin) return eval_use(e, st);
                    Origin* o = st.origins.find(fp.origin);
                    if (!o || o->freed) { err(e.pos, "use after free"); return V{}; }
                    V f{};
                    if (V* stored = o->heap_fields.find(fp.field)) f = *stored;
                    if (f.dead) { err(e.pos, "field was moved out or freed"); return V{}; }
                    // taking out of the HEAP yields a view; the free is
                    // free(x.field), the owner stays in the field
                    if (f.origin != kNoOrigin && !f.shared && !f.mut_view)
                        f.shared = true;
                    return f;
                }
                Bind* b = fp.bind;
                if (b->dead) { report_dead_use(e.pos, *b, st); return V{}; }
                if (b->frozen) {
                    err(e.pos, "cannot move a field of '" + b->name +
                        "' while it is uniquely borrowed");
                    return V{};
                }
                ensure_struct_origin(st, *b);
                V* f = b->val.fields.find(fp.field);
                if (!f) return V{};
                if (f->dead) {
                    err(e.pos, "field '" + e.name + "' of '" + b->name +
                        "' was already moved out or freed");
                    return V{};
                }
                V v = *f;
                f->dead = true; // partial move out of the local
                return v;
            }
            case ExprKind::Unary: {
                if (e.lhs) return eval_take(*e.lhs, st);
                return V{};
            }
            case ExprKind::Deref: {
                // let x = *sp: view-ified struct copy out of the heap
                if (!e.lhs) return V{};
                return eval_use(e, st);
            }
            case ExprKind::Binary: {
                if (e.lhs) eval_use(*e.lhs, st);
                if (e.rhs) eval_use(*e.rhs, st);
                return V{};
            }
            case ExprKind::Call:
            case ExprKind::MethodCall:
                return eval_call(e, st);
            case ExprKind::Cast:
                return eval_cast(e, st, true);
            case ExprKind::Index: {
                // struct copy out of a heap array slot: anonymous views
                V base = place_val(*e.lhs, st);
                if (e.rhs) eval_use(*e.rhs, st);
                if (base.origin != kNoOrigin) {
                    if (Origin* o = st.origins.find(base.origin))
                        if (o->freed) { err(e.pos, "use after free"); return V{}; }
                }
                V r;
                r.pset = base.pset;
                if (out_.is_struct_ptr(e.ty)) seed_fields(r, e.ty, true, base.pset);
                return r;
            }
            case ExprKind::StructLit: {
                V r;
                r.origin = site_origin(&e);
                seed_fields(r, e.ty, false, 0);
                for (size_t i = 0; i < e.args.size(); ++i) {
                    if (!e.args[i]) continue;
                    if (i < e.field_names.size()) {
                        size_t fidx = field_index(e.ty, e.field_names[i]);
                        if (fidx != SIZE_MAX) {
                            V fv = eval_take(*e.args[i], st);
                            const StructField* fd = field_at(e.ty, fidx);
                            if (fd && is_ptr_like(fd->ty)) {
                                if (fv.mut_view) {
                                    err(e.args[i]->pos, "cannot store a borrow of a "
                                        "local ('&' value) into a struct field — "
                                        "the local dies first");
                                }
                                if (fv.shared && fv.origin != kNoOrigin) acquire(st, fv);
                                r.fields.insert(static_cast<u32>(fidx), fv);
                                continue;
                            }
                            if (fd && struct_of_ty(fd->ty) != ty_none()) {
                                if (fv.mut_view)
                                    err(e.args[i]->pos, "cannot store a borrow of a "
                                        "local into a struct field");
                                r.fields.insert(static_cast<u32>(fidx), fv);
                                continue;
                            }
                        }
                    }
                    eval_use(*e.args[i], st);
                }
                return r;
            }
            case ExprKind::AddrOf:
                return eval_use(e, st);
            case ExprKind::ComptimeBlock:
            case ExprKind::IntLit:
            case ExprKind::FloatLit:
            case ExprKind::BoolLit:
                return V{};
        }
        return V{};
    }

    std::string field_name_of(TypeId struct_ty, u32 idx) {
        const StructField* f = field_at(struct_ty, idx);
        if (!f) return "?";
        return std::string(syms_.name(f->name));
    }

    // ---- calls ------------------------------------------------------------
    V eval_call(Expr& e, State& st) {
        if (e.kind == ExprKind::Call) {
            std::string callee = e.name;
            if (callee == "alloc") return eval_alloc(e, st);
            if (callee == "free") { do_free(e, st); return V{}; }
            if (callee == "print") {
                if (!e.args.empty() && e.args[0]) eval_use(*e.args[0], st);
                return V{};
            }
        }
        // resolve the callee (function or method) for param types
        const SemaFn* cf = nullptr;
        const FnDecl* decl = nullptr;
        if (const size_t* fi = out_.fn_by_name.find(e.callee)) {
            cf = &out_.fns[*fi];
            decl = &mod_.fns[cf->ast_index];
        }
        V recv;
        if (e.kind == ExprKind::MethodCall) {
            if (!e.lhs) return V{};
            recv = eval_use(*e.lhs, st);
            if (decl && decl->self_kind == kSelfMut && recv.origin != kNoOrigin &&
                !recv.mut_view) {
                if (Origin* o = st.origins.find(recv.origin))
                    if (o->shared_live > 0)
                        err(e.pos, "cannot take a unique (&mut self) receiver while "
                            "shared (*const) views are live");
            }
        }
        if (!cf || !decl) {
            // extern or unresolved: plain uses (externs are scalar-only)
            for (ExprP& a : e.args) if (a) eval_use(*a, st);
            return V{};
        }

        // argument values, index-aligned with the callee's params: a method's
        // params[0] is self, so the receiver occupies slot 0
        std::vector<V> argvals;
        std::vector<bool> arg_is_addr;
        if (e.kind == ExprKind::MethodCall) {
            argvals.push_back(recv);
            arg_is_addr.push_back(e.lhs->kind == ExprKind::AddrOf);
        }
        for (ExprP& a : e.args) {
            if (!a) { argvals.push_back(V{}); arg_is_addr.push_back(false); continue; }
            argvals.push_back(eval_use(*a, st));
            arg_is_addr.push_back(a->kind == ExprKind::AddrOf);
        }
        // freeze &x args for the call duration (unfreeze after unless the
        // result inherits the borrow through return-flow)
        for (size_t i = 0; i < argvals.size(); ++i) {
            if (!arg_is_addr[i]) continue;
            if (Bind* tgt = deref_bind_ref(st, argvals[i].freeze)) tgt->frozen++;
        }

        u64 flow = 0;
        if (const u64* f = ret_flow_.find(e.callee)) flow = *f;
        V r;
        if (is_ptr_like(e.ty)) {
            if (flow == 0) {
                r.origin = site_origin(&e);
                // B2 fix: mint sites REGISTER the origin so the free/write
                // discipline can never be silently skipped
                if (st.origins.find(r.origin) == nullptr)
                    st.origins.insert(r.origin, Origin{});
                if (out_.is_struct_ptr(e.ty)) seed_heap_fields(st, r.origin, e.ty);
            } else if ((flow & (flow - 1)) == 0) {
                // exactly one flowing arg: alias it (Rust reborrow shape)
                for (size_t i = 0; i < argvals.size() && i < 63; ++i) {
                    if (((flow >> i) & 1) == 0) continue;
                    r = argvals[i];
                    if (!r.shared && r.origin != kNoOrigin) r.mut_view = true;
                    break;
                }
            } else {
                // multiple flowing args: anonymous read-only alias
                r.shared = true;
                for (size_t i = 0; i < argvals.size() && i < 63; ++i)
                    if ((flow >> i) & 1) r.pset |= argvals[i].pset;
            }
        } else if (struct_of_ty(e.ty) != ty_none()) {
            r.origin = site_origin(&e);
            if (flow == 0) seed_fields(r, e.ty, false, 0);
            else {
                r.origin = kNoOrigin;
                u64 ps = 0;
                for (size_t i = 0; i < argvals.size() && i < 63; ++i)
                    if ((flow >> i) & 1) ps |= argvals[i].pset;
                seed_fields(r, e.ty, true, ps);
            }
        }
        // release the transient &x freezes the result did not inherit
        if (flow == 0) {
            for (size_t i = 0; i < argvals.size(); ++i) {
                if (!arg_is_addr[i]) continue;
                if (Bind* tgt = deref_bind_ref(st, argvals[i].freeze))
                    if (tgt->frozen > 0) tgt->frozen--;
            }
        }
        return r;
    }

    V eval_alloc(Expr& e, State& st) {
        for (ExprP& a : e.args)
            if (a) eval_use(*a, st);
        V r;
        r.origin = site_origin(&e);
        st.origins.insert(r.origin, Origin{}); // fresh/reset (loop rounds)
        if (out_.is_struct_ptr(e.ty)) seed_heap_fields(st, r.origin, e.ty);
        return r;
    }

    void do_free(Expr& e, State& st) {
        if (e.args.empty() || !e.args[0]) return;
        Expr& a = *e.args[0];
        // pre-diagnostics for the binding forms (better messages)
        if (a.kind == ExprKind::Ident) {
            if (Bind* b = find_bind(st, a.name)) {
                if (b->dead) {
                    if (b->val.origin != kNoOrigin) {
                        if (const Origin* o = st.origins.find(b->val.origin)) {
                            if (o->freed) {
                                err(a.pos, "double free of '" + a.name + "'");
                                return;
                            }
                        }
                    }
                    err(a.pos, "free of a moved value '" + a.name + "'");
                    return;
                }
            }
        }
        if (a.kind == ExprKind::Index) {
            err(a.pos, "cannot free through a heap-array slot in strict mode "
                "(slot ownership is not statically provable); free via the "
                "owning binding or field");
            if (a.lhs) eval_use(*a.lhs, st);
            return;
        }
        // S6: a heap-struct field free (free(x.f)) is the documented OWNING
        // free — resolve the stored field value directly (no view-ification)
        // so the owner (not a read-only copy) is consumed and marked.
        if (a.kind == ExprKind::Field) {
            FieldPlace fp;
            if (resolve_field_place(a, st, fp) && fp.heap && fp.origin != kNoOrigin) {
                Origin* o = st.origins.find(fp.origin);
                if (o && !o->freed) {
                    if (V* stored = o->heap_fields.find(fp.field)) {
                        if (stored->dead) {
                            err(a.pos, "field was already moved out or freed");
                            return;
                        }
                        V sv = *stored; // the stored value (owner if owned)
                        stored->dead = true;
                        // route through the shared discipline below by
                        // treating the stored value as the freed operand
                        if (sv.shared) {
                            err(a.pos, "cannot free a shared (*const) view "
                                "stored in the field; free through the owning "
                                "binding");
                            return;
                        }
                        if (sv.mut_view) {
                            err(a.pos, "cannot free a borrowed pointer stored "
                                "in the field");
                            return;
                        }
                        if (sv.pset != 0) {
                            err(a.pos, "cannot free a borrowed (parameter) "
                                "pointer stored in the field");
                            return;
                        }
                        if (sv.origin == kNoOrigin) {
                            err(a.pos, "cannot free a field of unknown "
                                "provenance in strict mode");
                            return;
                        }
                        if (st.origins.find(sv.origin) == nullptr)
                            st.origins.insert(sv.origin, Origin{});
                        Origin* so = st.origins.find(sv.origin);
                        if (so->freed) { err(a.pos, "double free"); return; }
                        if (so->shared_live > 0) {
                            err(a.pos, "cannot free: shared (*const) views of "
                                "the allocation are live");
                            return;
                        }
                        if (st.array_stored.count(sv.origin) != 0) {
                            err(a.pos, "cannot free: shared views of the "
                                "allocation were stored into a heap array");
                            return;
                        }
                        if (has_live_alias(st, sv.origin)) {
                            err(a.pos, "cannot free: the allocation is uniquely "
                                "borrowed");
                            return;
                        }
                        so->freed = true;
                        return;
                    }
                }
            }
        }
        V v = eval_take(a, st);
        if (v.shared) {
            err(a.pos, "cannot free a shared (*const) view; free through the "
                "owning binding");
            return;
        }
        if (v.mut_view) {
            err(a.pos, "cannot free a borrowed pointer ('&' of a local)");
            return;
        }
        if (v.pset != 0) {
            err(a.pos, "cannot free a borrowed value: the pointer is owned by "
                "the caller (function parameter)");
            return;
        }
        if (v.origin == kNoOrigin) {
            err(a.pos, "cannot free a value of unknown provenance in strict mode");
            return;
        }
        // B1/B3 fix: never silently skip an unregistered or fuzzy origin —
        // register it (fresh) and run the full discipline. The alias scan
        // walks bindings, so it is complete regardless of the table.
        if (st.origins.find(v.origin) == nullptr)
            st.origins.insert(v.origin, Origin{});
        Origin* o = st.origins.find(v.origin);
        if (o->freed) { err(a.pos, "double free"); return; }
        if (o->shared_live > 0) {
            err(a.pos, "cannot free: shared (*const) views of the allocation "
                "are live");
            return;
        }
        if (st.array_stored.count(v.origin) != 0) {
            err(a.pos, "cannot free: shared views of the allocation were "
                "stored into a heap array (slot liveness is not provable)");
            return;
        }
        if (has_live_alias(st, v.origin)) {
            err(a.pos, "cannot free: the allocation is uniquely borrowed (a "
                "call-result alias or '&' view is live)");
            return;
        }
        o->freed = true;
        // views stored inside the freed struct die with it
        for (auto& kv : o->heap_fields.entries()) release(st, kv.second);
    }

    // ---- state equality / join ---------------------------------------------
    bool same_state(const State& a, const State& b) {
        if (a.scopes.size() != b.scopes.size()) return false;
        for (size_t i = 0; i < a.scopes.size(); ++i) {
            if (a.scopes[i].binds.size() != b.scopes[i].binds.size()) return false;
            for (size_t j = 0; j < a.scopes[i].binds.size(); ++j) {
                const Bind& x = a.scopes[i].binds[j];
                const Bind& y = b.scopes[i].binds[j];
                if (x.name != y.name || x.dead != y.dead || x.frozen != y.frozen ||
                    !same_v(x.val, y.val))
                    return false;
            }
        }
        if (a.origins.size() != b.origins.size()) return false;
        for (size_t i = 0; i < a.origins.size(); ++i) {
            const auto& xo = a.origins.entries()[i];
            const auto& yo = b.origins.entries()[i];
            if (xo.first != yo.first) return false;
            if (xo.second.freed != yo.second.freed ||
                xo.second.shared_live != yo.second.shared_live)
                return false;
            if (xo.second.heap_fields.size() != yo.second.heap_fields.size())
                return false;
            for (size_t k = 0; k < xo.second.heap_fields.size(); ++k) {
                const auto& xf = xo.second.heap_fields.entries()[k];
                const auto& yf = yo.second.heap_fields.entries()[k];
                if (xf.first != yf.first || !same_v(xf.second, yf.second)) return false;
            }
        }
        return a.array_stored == b.array_stored;
    }

    void join_into(State& a, const State& b) {
        for (size_t i = 0; i < a.scopes.size() && i < b.scopes.size(); ++i) {
            for (size_t j = 0; j < a.scopes[i].binds.size() &&
                               j < b.scopes[i].binds.size(); ++j)
                a.scopes[i].binds[j] = join_bind(a.scopes[i].binds[j],
                                                 b.scopes[i].binds[j]);
        }
        for (size_t i = 0; i < b.origins.size(); ++i) {
            const auto& be = b.origins.entries()[i];
            Origin* ao = a.origins.find(be.first);
            if (!ao) {
                a.origins.insert(be.first, be.second);
                continue;
            }
            ao->freed = ao->freed || be.second.freed;
            ao->shared_live = ao->shared_live > be.second.shared_live
                                  ? ao->shared_live
                                  : be.second.shared_live;
            for (size_t k = 0; k < be.second.heap_fields.size(); ++k) {
                const auto& bf = be.second.heap_fields.entries()[k];
                if (V* af = ao->heap_fields.find(bf.first))
                    *af = join_v(*af, bf.second);
                else
                    ao->heap_fields.insert(bf.first, bf.second);
            }
        }
        for (u32 o : b.array_stored) a.array_stored.insert(o);
    }
};

} // namespace

bool run_borrow_check(ModuleAst& mod, SemaModule& out, Diagnostics& diag,
                      SymbolTable& syms) {
    Checker c(mod, out, diag, syms);
    return c.run();
}

} // namespace jules
