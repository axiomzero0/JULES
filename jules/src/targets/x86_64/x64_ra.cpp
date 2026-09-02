// Pass 85 — x86-64 linear-scan register allocator.
//
// Input: post-isel MIR where every value lives in a dedicated frame slot
// (the spill-everywhere model of the MVP emitter). The allocator promotes
// slots to physical registers:
//
//   * Live ranges are computed on the linear instruction stream: a slot's
//     range spans [first def, last activity] (defs = MovRS/MovFpS/MovSImm,
//     uses = MovSR/MovFpR/LeaSlot). The SoN values behind slots are SSA, so
//     a range with multiple defs only arises from phi copies — still a
//     single linear interval, which is exactly what linear scan assigns.
//   * Register pools: callee-saved GPRs (rbx, r12-r15) for ranges that cross
//     calls (SysV: preserved; saved/restored by our prologue/epilogue),
//     caller-saved GPRs (r10, r11 — never touched by isel) for local ranges,
//     and xmm2-7 for FP ranges that do not cross calls (all XMMs are
//     caller-saved in SysV, so call-crossing FP values stay in memory).
//   * Address-taken slots (LeaSlot — stack objects) and slots referenced by
//     both GPR and FP slot-ops stay in memory.
//   * Interference = interval overlap (Poletto linear scan). A slot that
//     finds no free register is simply not promoted: its instructions keep
//     the original memory operands, so spilling degenerates to the
//     correctness-first spill-everywhere behavior for that value.
//
// ABI notes: pushes are inserted between `mov rbp, rsp` and the frame
// subtraction, so the callee-save area occupies rbp-8..rbp-8n. Restores are
// movq off(%rbp) inserted before every Ret/TailCallFn (leave;ret / leave;jmp
// discards that area by resetting rsp from rbp, so restoring before the
// leave is the only requirement). FrameSub is re-patched so the final rsp
// stays 16-byte aligned.
#include "core/codegen/linear.h"

#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <vector>

namespace jules {

namespace {

struct LiveRange {
    i32 slot = 0;
    bool fp = false;          // referenced by MovFpS/MovFpR
    bool gp = false;          // referenced by MovRS/MovSR/MovSImm/LeaSlot
    bool addr_taken = false;  // LeaSlot references it
    bool defined = false;
    bool has_reads = false;   // any load (use) of the slot exists
    u32 use_count = 0;        // number of load instructions reading the slot
    bool mem_ref = false;     // still referenced by a slot operand post-fold
    size_t first_live = SIZE_MAX; // earliest def / live-before position
    size_t last_live = 0;         // latest use / live-after position
    bool crosses_call = false;
    bool spans_backedge = false; // live across a loop backedge: re-read every
                                 // iteration; never a spill victim
    bool promoted = false;
    R assigned = R::Rax;

    // Dead def (stored, never read): its stores are removable by the RA.
    bool dead_def() const { return defined && !has_reads && !addr_taken; }
    // Still needs a frame slot after RA + dead-store removal?
    bool needs_memory() const {
        if (promoted) return false;
        return mem_ref;
    }
};

bool op_is_call(IOp op) {
    // TailCallFn is NOT a clobber point: we restore our callee-saved regs
    // before the leave;jmp, and the tail-called callee preserves them per
    // the ABI. Caller-saved values cannot be live past a tail call (the
    // function never executes again).
    return op == IOp::CallFn || op == IOp::CallSym;
}

bool slot_def_inst(const Inst& i, i32& slot, bool& fp) {
    if ((i.op == IOp::MovRS || i.op == IOp::MovFpS) && i.b.k == Operand::K::Slot) {
        slot = i.b.slot;
        fp = i.op == IOp::MovFpS;
        return true;
    }
    if (i.op == IOp::MovSImm && i.a.k == Operand::K::Slot) {
        slot = i.a.slot;
        fp = false;
        return true;
    }
    return false;
}

bool slot_use_inst(const Inst& i, i32& slot, bool& fp, bool& addr) {
    if ((i.op == IOp::MovSR || i.op == IOp::MovFpR) && i.b.k == Operand::K::Slot) {
        slot = i.b.slot;
        fp = i.op == IOp::MovFpR;
        addr = false;
        return true;
    }
    if (i.op == IOp::LeaSlot && i.b.k == Operand::K::Slot) {
        slot = i.b.slot;
        fp = false;
        addr = true;
        return true;
    }
    return false;
}

struct RegPool {
    std::vector<R> regs;
    std::vector<bool> used;

    RegPool(const R* list, size_t n) : regs(list, list + n), used(n, false) {}
    int find_free() const {
        for (size_t i = 0; i < regs.size(); ++i)
            if (!used[i]) return static_cast<int>(i);
        return -1;
    }
    void take(int i) { used[static_cast<size_t>(i)] = true; }
    void release(int i) { used[static_cast<size_t>(i)] = false; }
};

// Block-level slot sets (small functions; linear scan in vectors).
struct SlotSet {
    std::vector<i32> slots;
    bool has(i32 s) const {
        for (i32 x : slots) if (x == s) return true;
        return false;
    }
    void add(i32 s) {
        if (!has(s)) slots.push_back(s);
    }
    void add_all(const SlotSet& other) {
        for (i32 s : other.slots) add(s);
    }
    void remove(i32 s) {
        for (size_t i = 0; i < slots.size(); ++i)
            if (slots[i] == s) { slots.erase(slots.begin() + i); return; }
    }
};

struct Allocator {
    LFunction& lf;

    std::vector<LiveRange> ranges;

    explicit Allocator(LFunction& f) : lf(f) {
        ranges.resize(static_cast<size_t>(lf.slot_count > 0 ? lf.slot_count : 0));
        for (size_t s = 0; s < ranges.size(); ++s) ranges[s].slot = static_cast<i32>(s);
    }

    LiveRange& range(i32 slot) { return ranges[static_cast<size_t>(slot)]; }

    // ------------------------------------------------------------------
    // Liveness: backward dataflow over the emitted blocks, then a
    // per-instruction backward walk. Linear [first use, last use]
    // intervals are UNSOUND with backedges (a value defined before a
    // loop and used at the loop top is re-read on every iteration, so
    // its live range must extend to the latch). The block graph is
    // still available in lf.blocks; the emitted Label instructions
    // (id == block index, base 0) locate each block's instruction range.
    // ------------------------------------------------------------------
    void analyze() {
        const size_t nblocks = lf.blocks.size();
        if (nblocks == 0 || lf.code.empty()) return;

        // 1) map instructions to blocks; prologue = pseudo-block `nblocks`.
        std::vector<int> inst_block(lf.code.size(), -1);
        int cur = -1;
        for (size_t i = 0; i < lf.code.size(); ++i) {
            if (lf.code[i].op == IOp::Label) {
                int id = lf.code[i].a.label;
                if (id >= 0 && static_cast<size_t>(id) < nblocks) cur = id;
            }
            inst_block[i] = cur;
        }
        const size_t nb = nblocks + 1; // + prologue pseudo-block
        const size_t entry = nblocks;

        // 2) per-block use/def sets (forward scan; a use after a def in
        // the same block is not a block-level use).
        std::vector<SlotSet> buse(nb), bdef(nb);
        for (size_t i = 0; i < lf.code.size(); ++i) {
            int b = inst_block[i];
            if (b < 0) b = static_cast<int>(entry);
            size_t bi = static_cast<size_t>(b);
            i32 slot = 0;
            bool fp = false, addr = false;
            if (slot_def_inst(lf.code[i], slot, fp)) {
                LiveRange& r = range(slot);
                r.defined = true;
                if (fp) r.fp = true; else r.gp = true;
                bdef[bi].add(slot);
            } else if (slot_use_inst(lf.code[i], slot, fp, addr)) {
                LiveRange& r = range(slot);
                if (fp) r.fp = true; else r.gp = true;
                if (addr) r.addr_taken = true;
                if (!bdef[bi].has(slot)) buse[bi].add(slot);
            }
        }

        // 3) successors: real blocks from lf.blocks, prologue -> block 0.
        std::vector<std::vector<int>> succs(nb);
        for (size_t b = 0; b < nblocks; ++b)
            for (int s : lf.blocks[b].succs)
                if (s >= 0 && static_cast<size_t>(s) < nblocks)
                    succs[b].push_back(s);
        succs[entry].push_back(0);

        // 4) live-in/live-out fixpoint (backward).
        std::vector<SlotSet> live_in(nb), live_out(nb);
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t bi = nb; bi-- > 0;) {
                SlotSet out;
                for (int s : succs[bi]) out.add_all(live_in[static_cast<size_t>(s)]);
                if (out.slots.size() != live_out[bi].slots.size()) changed = true;
                live_out[bi] = std::move(out);
                SlotSet in = buse[bi];
                for (i32 s : live_out[bi].slots)
                    if (!bdef[bi].has(s)) in.add(s);
                if (in.slots.size() != live_in[bi].slots.size()) changed = true;
                live_in[bi] = std::move(in);
            }
        }

        // 5) per-instruction backward walk: range events + call crossing.
        for (size_t bi = nb; bi-- > 0;) {
            SlotSet live = live_out[bi];
            for (size_t i = lf.code.size(); i-- > 0;) {
                if (inst_block[i] < 0 ? static_cast<size_t>(entry) != bi
                                      : static_cast<size_t>(inst_block[i]) != bi)
                    continue;
                const Inst& inst = lf.code[i];
                // live AFTER this instruction = `live`
                if (op_is_call(inst.op)) {
                    for (i32 s : live.slots) range(s).crosses_call = true;
                }
                for (i32 s : live.slots) {
                    LiveRange& r = range(s);
                    if (r.last_live < i) r.last_live = i;
                }
                i32 slot = 0;
                bool fp = false, addr = false;
                if (slot_def_inst(inst, slot, fp)) {
                    LiveRange& r = range(slot);
                    if (r.last_live < i) r.last_live = i;
                    if (r.first_live > i) r.first_live = i; // def starts the range
                    live.remove(slot);
                } else if (slot_use_inst(inst, slot, fp, addr)) {
                    LiveRange& r = range(slot);
                    r.has_reads = true;
                    ++r.use_count;
                    if (r.last_live < i) r.last_live = i;
                    live.add(slot);
                }
                // live BEFORE this instruction = `live` now
                for (i32 s : live.slots) {
                    LiveRange& r = range(s);
                    if (r.first_live > i) r.first_live = i;
                }
            }
        }

        // defined but never live/used: degenerate range at the def site so
        // the promotion logic (and dead-store cleanup) treats it as dead.
        for (LiveRange& r : ranges) {
            if (r.first_live == SIZE_MAX) r.first_live = r.last_live;
        }

        // Backedge detection on the linear stream: a jump to an earlier
        // label. A live range that spans a backedge is loop-carried — the
        // linear stream contains ONE copy of the loop body, so static use
        // counts and densities undercount its real (per-iteration) use; such
        // ranges are never eligible as spill victims.
        {
            FlatMap<int, size_t> label_at;
            for (size_t i = 0; i < lf.code.size(); ++i)
                if (lf.code[i].op == IOp::Label) label_at.insert(lf.code[i].a.label, i);
            for (size_t i = 0; i < lf.code.size(); ++i) {
                const Inst& j = lf.code[i];
                if (j.op != IOp::Jcc && j.op != IOp::Jmp) continue;
                const size_t* tp = label_at.find(j.a.label);
                if (!tp || *tp >= i) continue;
                size_t top = *tp, bot = i;
                for (LiveRange& r : ranges)
                    if (r.first_live != SIZE_MAX && r.first_live < bot &&
                        r.last_live > top)
                        r.spans_backedge = true;
            }
        }
#ifdef JULES_DEBUG_RA2
        for (i32 si = 0; si < lf.slot_count; ++si) {
            const LiveRange& r = range(si);
            if (r.promoted) continue;
            fprintf(stderr, "[ra2] slot %d fp=%d first=%zu last=%zu defs:", si, (int)r.fp,
                    r.first_live == SIZE_MAX ? 999u : r.first_live, r.last_live);
            for (size_t i = 0; i < lf.code.size(); ++i) {
                i32 ds = 0; bool df = false;
                if (slot_def_inst(lf.code[i], ds, df) && ds == si) fprintf(stderr, " d@%zu", i);
            }
            fprintf(stderr, " uses:");
            for (size_t i = 0; i < lf.code.size(); ++i) {
                i32 us = 0; bool uf = false, ua = false;
                if (slot_use_inst(lf.code[i], us, uf, ua) && us == si) fprintf(stderr, " u@%zu", i);
            }
            fprintf(stderr, "\n");
        }
#endif
    }

    bool promotable(const LiveRange& r) const {
        if (!r.defined) return false;         // never defined: leave in memory
        if (r.addr_taken) return false;       // stack object: address escapes
        if (r.fp && r.gp) return false;       // class conflict: safety first
        if (r.fp && r.crosses_call) return false; // no callee-saved XMM in SysV
        if (r.last_live <= r.first_live) {
            // dead def: no uses and a single def — leave for dead-store
            // cleanup in pass 86/87 rather than rewriting it.
            return false;
        }
        return true;
    }

    // Does this slot still require a frame slot after RA + dead-store
    // removal? (promoted, or spilled with live reads)
    bool needs_memory(const LiveRange& r) const {
        if (r.promoted) return false;
        if (r.addr_taken) return true;
        // reads exist iff some use position was recorded
        return r.has_reads;
    }

    // Poletto linear scan over ranges sorted by start position. With the
    // size-biased cost model (-Os/-Oz), callee-saved registers are preferred
    // for everything: fewer live push/pop pairs trade a little speed for
    // smaller frames and prologues.
    void assign(bool size_biased) {
        const R gp_caller[] = {R::R10, R::R11};
        const R gp_callee[] = {R::Rbx, R::R12, R::R13, R::R14, R::R15};
        // xmm2-13: all XMMs are caller-saved in SysV, so these are only
        // usable for ranges that do not cross calls; xmm14-15 are reserved
        // for the isel constant pool.
        const R xmm[] = {R::Xmm2, R::Xmm3, R::Xmm4, R::Xmm5,  R::Xmm6,  R::Xmm7,
                         R::Xmm8, R::Xmm9, R::Xmm10, R::Xmm11, R::Xmm12, R::Xmm13};

        // Caller-saved pool: usable only for non-crossing GPR ranges.
        RegPool caller_pool(gp_caller, sizeof gp_caller / sizeof gp_caller[0]);
        RegPool callee_pool(gp_callee, sizeof gp_callee / sizeof gp_callee[0]);
        RegPool xmm_pool(xmm, sizeof xmm / sizeof xmm[0]);
        // A register in the caller pool that is also used by the callee pool
        // never happens (disjoint lists).

        std::vector<LiveRange*> order;
        for (LiveRange& r : ranges)
            if (promotable(r)) order.push_back(&r);
        std::sort(order.begin(), order.end(),
                  [](const LiveRange* a, const LiveRange* b) {
                      return a->first_live < b->first_live;
                  });

        struct Active {
            LiveRange* r;
            RegPool* pool;
            int idx;
            bool dead = false;
        };
        std::vector<Active> active;

        for (LiveRange* r : order) {
            // expire: ranges that ended before this one starts
            for (Active& a : active) {
                if (a.r->last_live < r->first_live) a.dead = true;
            }
            // (mark-then-sweep to keep indices stable)
            std::vector<Active> keep;
            for (Active& a : active) {
                if (a.dead) a.pool->release(a.idx);
                else keep.push_back(a);
            }
            active = std::move(keep);

            // Candidate pools in preference order:
            //   non-crossing GP  : caller-saved first (no prologue cost), then callee
            //   crossing GP      : callee-saved only (survive calls)
            //   FP               : xmm2-7 (all XMMs caller-saved in SysV)
            // Poletto spill heuristic: when a pool is exhausted, spill the
            // active range from THAT pool whose live range ends furthest and
            // whose end is later than the newcomer's — its register goes to
            // the newcomer (the victim's slot keeps memory operands, which
            // degenerates to spill-everywhere for that value).
            auto try_pool = [&](RegPool* cand) -> bool {
                int free = cand->find_free();
                if (free >= 0) {
                    cand->take(free);
                    r->promoted = true;
                    r->assigned = cand->regs[static_cast<size_t>(free)];
                    active.push_back(Active{r, cand, free, false});
                    return true;
                }
                // spill heuristic inside this pool: prefer the LEAST
                // DENSELY USED active range that ends later than the
                // newcomer. Interval end alone misleads for loop-carried
                // values (their interval spans the whole loop while the
                // next use is immediate), so rank by use density
                // (uses per instruction of live range) and only spill
                // strictly-colder victims than the newcomer.
                auto density = [](const LiveRange* v) {
                    size_t span = v->last_live > v->first_live
                                      ? v->last_live - v->first_live + 1
                                      : 1;
                    return static_cast<double>(v->use_count) /
                           static_cast<double>(span);
                };
                Active* victim = nullptr;
                for (Active& a : active) {
                    if (a.pool != cand) continue;
                    if (a.r->spans_backedge) continue; // loop-carried: keep
                    if (a.r->last_live <= r->last_live) continue;
                    if (density(a.r) >= density(r)) continue; // keep hotter values
                    if (!victim || density(a.r) < density(victim->r)) victim = &a;
                }
                if (!victim) return false;
                victim->r->promoted = false; // spilled back to memory
                int vi = victim->idx;
                // ownership of the register moves to the newcomer; remove the
                // victim WITHOUT releasing (the pool slot stays taken)
                active.erase(active.begin() + (victim - active.data()));
                cand->take(vi);
                r->promoted = true;
                r->assigned = cand->regs[static_cast<size_t>(vi)];
                active.push_back(Active{r, cand, vi, false});
                return true;
            };

            bool done = false;
            if (!r->fp) {
                if (r->crosses_call || size_biased) {
                    done = try_pool(&callee_pool);
                    if (!done && !r->crosses_call) done = try_pool(&caller_pool);
                } else {
                    done = try_pool(&caller_pool);
                    if (!done) done = try_pool(&callee_pool);
                }
            } else {
                done = try_pool(&xmm_pool);
            }
            (void)done; // a value that found no register simply stays in memory
        }
    }

    void rewrite() {
        // Store-load pair folding (the pass-86 patterns, applied BEFORE the
        // promotion rewrite: an adjacent same-register store+load pair costs
        // ZERO instructions when folded, but ONE move when promoted — for
        // single-use values the fold strictly dominates promotion. This also
        // feeds the frame-elision decision (post-fold stream).
 //     [mov [s], reg][mov reg', [s]] -> [mov reg', reg] when s is not read later
        //     [movq $imm, [s]][mov reg', [s]] -> [movq $imm, reg'] likewise
        //     same-register pair -> nothing at all
        auto slot_read_after = [&](size_t from, i32 s) {
            for (size_t j = from; j < lf.code.size(); ++j) {
                const Inst& c = lf.code[j];
                if ((c.op == IOp::MovSR || c.op == IOp::MovFpR) &&
                    c.b.k == Operand::K::Slot && c.b.slot == s)
                    return true;
            }
            return false;
        };
        for (size_t i = 0; i + 1 < lf.code.size(); ++i) {
            Inst& cur = lf.code[i];
            Inst& nxt = lf.code[i + 1];
            if (cur.op == IOp::MovFpS && cur.b.k == Operand::K::Slot &&
                nxt.op == IOp::MovFpR && nxt.b.k == Operand::K::Slot &&
                nxt.b.slot == cur.b.slot && cur.a.k == Operand::K::Reg &&
                nxt.a.k == Operand::K::Reg &&
                !slot_read_after(i + 2, cur.b.slot)) {
                if (nxt.a.reg != cur.a.reg) {
                    cur.op = IOp::MovFpFp;
                    cur.a.reg = nxt.a.reg; // dst
                    cur.b.k = Operand::K::Reg;
                } else {
                    cur.op = IOp::Nop;
                }
                nxt.op = IOp::Nop;
                continue;
            }
            if (cur.op == IOp::MovRS && cur.b.k == Operand::K::Slot &&
                nxt.op == IOp::MovSR && nxt.b.k == Operand::K::Slot &&
                nxt.b.slot == cur.b.slot && cur.a.k == Operand::K::Reg &&
                nxt.a.k == Operand::K::Reg &&
                !slot_read_after(i + 2, cur.b.slot)) {
                if (nxt.a.reg != cur.a.reg) {
                    cur.op = IOp::MovRR;
                    cur.a.reg = nxt.a.reg; // dst
                    cur.b.k = Operand::K::Reg;
                    cur.size = 8;
                } else {
                    cur.op = IOp::Nop;
                }
                nxt.op = IOp::Nop;
                continue;
            }
            if (cur.op == IOp::MovSImm && cur.a.k == Operand::K::Slot &&
                nxt.op == IOp::MovSR && nxt.b.k == Operand::K::Slot &&
                nxt.b.slot == cur.a.slot && nxt.a.k == Operand::K::Reg &&
                !slot_read_after(i + 2, cur.a.slot)) {
                cur.op = IOp::MovRImm;
                cur.a.k = Operand::K::Reg;
                cur.a.reg = nxt.a.reg;
                nxt.op = IOp::Nop;
                continue;
            }
        }

        for (Inst& i : lf.code) {
            if ((i.op == IOp::MovRS || i.op == IOp::MovFpS) && i.b.k == Operand::K::Slot) {
                const LiveRange& r = range(i.b.slot);
                if (!r.promoted) continue;
                if (i.op == IOp::MovRS) {
                    // store: mov [slot], reg  ->  mov assigned, reg
                    R src = i.a.reg;
                    i.op = IOp::MovRR;
                    i.a.k = Operand::K::Reg;
                    i.a.reg = r.assigned;
                    i.b.k = Operand::K::Reg;
                    i.b.reg = src;
                } else {
                    // store: movsd [slot], xmm  ->  movsd assigned, xmm
                    R src = i.a.reg;
                    i.op = IOp::MovFpFp;
                    i.a.k = Operand::K::Reg;
                    i.a.reg = r.assigned;
                    i.b.k = Operand::K::Reg;
                    i.b.reg = src;
                }
                continue;
            }
            if (i.op == IOp::MovSImm && i.a.k == Operand::K::Slot) {
                const LiveRange& r = range(i.a.slot);
                if (!r.promoted) continue;
                // movq $imm, [slot]  ->  movq $imm, assigned
                i.op = IOp::MovRImm;
                i.a.k = Operand::K::Reg;
                i.a.reg = r.assigned;
                continue;
            }
            if ((i.op == IOp::MovSR || i.op == IOp::MovFpR) && i.b.k == Operand::K::Slot) {
                const LiveRange& r = range(i.b.slot);
                if (!r.promoted) continue;
                if (i.op == IOp::MovSR) {
                    // load: mov rax, [slot]  ->  mov rax, assigned
                    i.op = IOp::MovRR;
                    i.b.k = Operand::K::Reg;
                    i.b.reg = r.assigned;
                } else {
                    // load: movsd xmm0, [slot]  ->  movsd xmm0, assigned
                    i.op = IOp::MovFpFp;
                    i.b.k = Operand::K::Reg;
                    i.b.reg = r.assigned;
                }
                continue;
            }
        }

        // FP operand folding: the isel contract loads FpBin/FpCmp operands
        // into xmm0/xmm1 via MovFpFp (after promotion). A register-resident
        // source feeding the fixed src register can fold straight into the
        // consumer's operand, killing the movsd: SSE is two-operand, and
        // FpBin/FpCmp/FpNeg take real register operands now.
        for (size_t i = 0; i + 1 < lf.code.size(); ++i) {
            Inst& mov = lf.code[i];
            Inst& use = lf.code[i + 1];
            if (mov.op != IOp::MovFpFp || mov.a.k != Operand::K::Reg ||
                mov.b.k != Operand::K::Reg)
                continue;
            IOp consumer = use.op;
            if (consumer != IOp::FpBin && consumer != IOp::FpCmp) continue;
            // the mov must be loading the consumer's SOURCE register
            if (use.b.k != Operand::K::Reg || use.b.reg != mov.a.reg) continue;
            if (mov.b.reg == use.a.reg) continue; // would alias dst and src
            use.b.reg = mov.b.reg;
            mov.op = IOp::Nop;
        }

        // Dead-def stores: slots that are defined but never read keep a
        // store whose result nobody consumes. The RA already knows they are
        // dead — removing them here makes the frame-elision decision
        // self-contained (no reliance on pass 87 sweeping them later).
        for (Inst& i : lf.code) {
            i32 s = 0;
            bool f = false;
            if (slot_def_inst(i, s, f) && range(s).dead_def()) i.op = IOp::Nop;
        }

        // Recompute which slots still have memory operands (post-fold):
        // this drives the frame-elision decision.
        for (LiveRange& r : ranges) r.mem_ref = false;
        for (const Inst& i : lf.code) {
            i32 s = 0;
            bool f = false, a = false;
            if (slot_def_inst(i, s, f)) range(s).mem_ref = true;
            else if (slot_use_inst(i, s, f, a)) range(s).mem_ref = true;
        }
    }

    // Prologue pushes + pre-epilogue restores + frame layout + FrameSub patch.
    void finalize() {
        std::vector<R> callee_used;
        for (const LiveRange& r : ranges) {
            if (r.promoted && reg_is_callee_saved_gpr(r.assigned))
                callee_used.push_back(r.assigned);
        }
        // stable order (rbx, r12..r15) for deterministic frames
        std::sort(callee_used.begin(), callee_used.end(), [](R a, R b) {
            return static_cast<int>(a) < static_cast<int>(b);
        });

        // ---- rebuild the instruction stream ----
        std::vector<Inst> out;
        out.reserve(lf.code.size() + 2 * callee_used.size() + 8);
        bool pushed = false;
        bool frame_patched = false;
        i32 unpromoted = 0;
        for (i32 s = 0; s < lf.slot_count; ++s)
            if (range(s).needs_memory()) ++unpromoted;

        i32 callee_area = 8 * static_cast<i32>(callee_used.size());
        i32 slot_area = 8 * unpromoted;
        i32 total = callee_area + slot_area;
        i32 frame = (total + 15) & ~15;          // keep rsp 16-byte aligned
        i32 frame_sub = frame - callee_area;     // pushes already moved rsp

        // ---- frame elision (omit frame pointer) ---------------------------
        // No unpromoted slots => no rbp-relative addressing survives, so the
        // rbp chain and the frame subtraction are pure overhead. The
        // callee-saved area becomes a push/pop stack. SysV call alignment
        // must still hold at call sites: entry rsp = E with E % 16 == 8;
        // with k = (pad? 1 : 0) + n_pushes, rsp = E - 8k must be 0 mod 16,
        // i.e. k odd when the function makes calls. A lone `push rbp`
        // (never used as a frame pointer, popped at exit) is the pad.
        bool has_call_inst = false;
        for (const Inst& ci : lf.code)
            if (ci.op == IOp::CallFn || ci.op == IOp::CallSym) { has_call_inst = true; break; }
        bool elide = (unpromoted == 0);
        bool rbp_pad = elide && has_call_inst && (callee_used.size() % 2 == 0);
#ifdef JULES_DEBUG_RA
        if (elide)
            fprintf(stderr, "[ra] fn %u frame elided (%u callee-saved)\n", lf.fid,
                    (unsigned)callee_used.size());
        else {
            fprintf(stderr, "[ra] fn %u frame kept: %u unpromoted of %u:", lf.fid,
                    (unsigned)unpromoted, lf.slot_count);
            for (i32 s = 0; s < lf.slot_count; ++s)
                if (!range(s).promoted)
                    fprintf(stderr, " s%d(fp=%d addr=%d def=%d reads=%d live=%zu..%zu)", s,
                            range(s).fp, range(s).addr_taken, range(s).defined,
                            (int)range(s).has_reads,
                            range(s).first_live, range(s).last_live);
            fprintf(stderr, "\n");
        }
#endif

        for (Inst& i : lf.code) {
            if (elide) {
                if (i.op == IOp::PushRbp) {
                    if (rbp_pad) { out.push_back(i); } // alignment pad only
                    continue;                          // no frame pointer
                }
                if (i.op == IOp::MovRR && i.a.k == Operand::K::Reg &&
                    i.a.reg == R::Rbp && i.b.k == Operand::K::Reg &&
                    i.b.reg == R::Rsp)
                    continue;                          // no rbp = rsp
            }
            // callee-save pushes: between `mov rbp, rsp` and the frame sub
            if (i.op == IOp::FrameSub && !pushed) {
                pushed = true;
                for (R r : callee_used) {
                    Inst& p = out.emplace_back();
                    p.op = IOp::PushCal;
                    p.a.k = Operand::K::Reg;
                    p.a.reg = r;
                }
                if (elide) continue;                   // no frame subtraction
                i.b.k = Operand::K::Imm;
                i.b.imm = frame_sub;
                frame_patched = true;
                out.push_back(i);
                continue;
            }
            if (i.op == IOp::FrameSub) { // defensive: any other FrameSub
                if (!elide) {
                    i.b.imm = frame_sub;
                    frame_patched = true;
                    out.push_back(i);
                }
                continue;
            }
            // restores before every epilogue/tail-call exit
            if (i.op == IOp::Ret || i.op == IOp::TailCallFn) {
                for (size_t k = callee_used.size(); k-- > 0;) {
                    Inst& rv = out.emplace_back();
                    rv.a.k = Operand::K::Reg;
                    rv.a.reg = callee_used[k];
                    if (elide) {
                        rv.op = IOp::PopCal;
                    } else {
                        rv.op = IOp::RestoreCal;
                        rv.b.k = Operand::K::Imm;
                        rv.b.imm = -8 * static_cast<i64>(k + 1);
                    }
                }
                if (elide && rbp_pad) {
                    Inst& pad = out.emplace_back();
                    pad.op = IOp::PopCal;
                    pad.a.k = Operand::K::Reg;
                    pad.a.reg = R::Rbp; // pop the alignment pad
                }
                if (elide) {
                    if (i.op == IOp::Ret) {
                        Inst& rn = out.emplace_back();
                        rn.op = IOp::RetNaked;
                        continue;
                    }
                    Inst& tc = out.emplace_back();
                    tc.op = IOp::TailCallNaked;
                    tc.a = i.a; // label operand
                    continue;
                }
                out.push_back(i);
                continue;
            }
            out.push_back(i);
        }
        (void)frame_patched;
        lf.code = std::move(out);
        lf.frame_size = elide ? 8 * static_cast<i32>(callee_used.size()) +
                                    (rbp_pad ? 8 : 0)
                              : frame;

        // ---- slot offsets: memory-resident slots live below the callee area ----
        lf.slot_offset.assign(static_cast<size_t>(lf.slot_count), 0);
        lf.slot_reg.clear();
        i32 next = 0;
        for (i32 s = 0; s < lf.slot_count; ++s) {
            const LiveRange& r = range(s);
            if (r.promoted) {
                lf.slot_reg.insert(s, r.assigned);
            } else if (r.needs_memory()) {
                ++next;
                lf.slot_offset[static_cast<size_t>(s)] =
                    -(callee_area + 8 * next);
            }
        }
        lf.ra_promoted = 0;
        lf.ra_spilled = 0;
        for (i32 s = 0; s < lf.slot_count; ++s) {
            if (range(s).promoted) ++lf.ra_promoted;
            else if (range(s).needs_memory()) ++lf.ra_spilled;
        }
    }

    bool run(bool size_biased, bool aggressive) {
        (void)aggressive;
        if (lf.slot_count <= 0) {
            lf.slot_offset.clear();
            lf.frame_size = 16;
            return false;
        }
        analyze();
        assign(size_biased);
        rewrite();
        finalize();
        return lf.ra_promoted > 0;
    }
};

} // namespace

bool x64_allocate_registers(LFunction& lf, const Graph* /*g*/, bool use_registers,
                             bool aggressive, bool size_biased) {
    if (!use_registers) {
        // -O0/-Og "simple" allocator: spill-everywhere frame layout.
        return x64_allocate_frame(lf);
    }
    Allocator a(lf);
    return a.run(size_biased, aggressive);
}

} // namespace jules
