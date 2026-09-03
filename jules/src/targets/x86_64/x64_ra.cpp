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
    bool starts_at_def = false;  // first activity is a def (allows a
                                 // predecessor range to expire AT this
                                 // position: its final use is the same
                                 // instruction that starts this range)
    // Generation sub-intervals (exact liveness): multi-def slots — loop
    // and merge phis — get one [start, end] per linear redefinition. The
    // hull [first_live, last_live] over-approximates (it merges the old
    // value's tail with the new value's head across the backedge); the
    // gens are what makes hint coalescing sound: two slots may share a
    // register iff their gens never overlap, even when their hulls do.
    std::vector<std::pair<size_t, size_t>> gens;

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

// Does this instruction (a raw pre-RA one) define `slot`?
bool slot_def_at(const Inst& i) {
    i32 s = 0;
    bool f = false;
    return slot_def_inst(i, s, f);
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
    // Generation sub-intervals: collect def/use positions per slot, then
    // split at each redefinition. A def kills the running generation; the
    // next generation starts there. Single-def slots collapse to one gen
    // == the hull. Re-run after any range EXTENSION (fuse extraction moves
    // a result's birth to the chain's load position; coalescing decisions
    // made on pre-extension gens would miss the extension's interference
    // and could place a live B-operand on the register the fused leading
    // move is about to clobber).
    // ------------------------------------------------------------------
    void compute_gens() {
        std::vector<std::vector<size_t>> defs(
            static_cast<size_t>(lf.slot_count > 0 ? lf.slot_count : 0));
        std::vector<std::vector<size_t>> uses(defs.size());
        for (size_t i = 0; i < lf.code.size(); ++i) {
            i32 s = 0;
            bool f = false, a = false;
            if (slot_def_inst(lf.code[i], s, f))
                defs[static_cast<size_t>(s)].push_back(i);
            else if (slot_use_inst(lf.code[i], s, f, a))
                uses[static_cast<size_t>(s)].push_back(i);
        }
        for (size_t s = 0; s < defs.size(); ++s) {
            LiveRange& r = ranges[s];
            const std::vector<size_t>& dv = defs[s];
            if (dv.size() <= 1) {
                r.gens.clear();
                if (r.first_live != SIZE_MAX)
                    r.gens.push_back({r.first_live, r.last_live});
                continue;
            }
            r.gens.clear();
            for (size_t j = 0; j < dv.size(); ++j) {
                size_t start = dv[j];
                size_t end = start;
                size_t bound = j + 1 < dv.size() ? dv[j + 1]
                                                 : lf.code.size();
                for (size_t u : uses[s])
                    if (u > start && u < bound && u > end) end = u;
                r.gens.push_back({start, end});
            }
        }
    }

    // ------------------------------------------------------------------
    // Store-load pair folding (the pass-86 patterns, applied BEFORE the
    // liveness analysis: an adjacent same-register store+load pair costs
    // ZERO instructions when folded, but ONE move when promoted — for
    // single-use values the fold strictly dominates promotion. This also
    // feeds the frame-elision decision (post-fold stream).
    //     [mov [s], reg][mov reg', [s]] -> [mov reg', reg] when s is not read later
    //     [movq $imm, [s]][mov reg', [s]] -> [movq $imm, reg'] likewise
    //     same-register pair -> nothing at all
    // ------------------------------------------------------------------
    void pair_fold() {
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
    }

    // ------------------------------------------------------------------
    // Accumulator-chain fusion extraction (pre-assignment).
    //
    // The isel contract threads every computation through fixed scratch
    // registers (rax for GP, xmm0 for FP):
    //     [load sA -> acc] [load sB -> rcx/xmm1] [op acc, src] [store sZ <- acc]
    // Two-operand x86 lets the whole chain compute directly in sZ's
    // register:  [mov Z, A] [op Z, src]  — and when Z and A share a
    // register (hint coalescing), the leading mov disappears too and the
    // op runs in place — loop-carried updates become `addq $1, %r15` /
    // `addsd %xmm4, %xmm3` exactly like a production compiler.
    //
    // Extraction registers each chain as a Fuse and adds a hint (sZ <-
    // sA) plus a range extension (sZ's live range must start at the load
    // position, where the fused [mov Z, A] now writes the register).
    // ------------------------------------------------------------------
    struct Fuse {
        size_t load_pos = 0;   // [slot-load of sA into acc]
        size_t store_pos = 0;  // [slot-store of sZ from acc]
        i32 a_slot = 0;
        i32 z_slot = 0;
        bool fp = false;
        u32 ops = 0;           // chain-op count (0 = pure phi copy)
        bool inplace_ok = false; // the load is sA's generation's last use
        bool self_update = false; // sZ == sA: the chain updates its own
                                  // accumulator slot in place (a = a*c + d)
        size_t first_op_pos = SIZE_MAX; // first acc-WRITING op position (a
                                  // gap reading sZ before it sees the old
                                  // value; after it, the new one)
        SmallVec<std::pair<i32, size_t>, 8> gap_slots; // (slot, position) of
                                  // memory-resident operand loads in the
                                  // window; their registers must not end
                                  // up equal to Z (coalescing can do that
                                  // — the leading move would clobber the
                                  // operand before the op reads it)
    };
    std::vector<Fuse> fuses_;
    std::vector<std::pair<i32, i32>> hints_; // (z, a): z wants a's register

    bool is_acc(R r, bool fp) const { return fp ? r == R::Xmm0 : r == R::Rax; }

    bool is_chain_op(const Inst& i, bool fp) const {
        if (i.a.k != Operand::K::Reg) return false;
        if (fp) {
            if (i.a.reg != R::Xmm0) return false;
            // A snapshot store of an INTERMEDIATE chain result keeps the
            // accumulator value intact (the pair fold already removed the
            // adjacent reload, or a later reload is a legal gap load): the
            // chain continues through it. Retargeted to Z like the ops.
            if (i.op == IOp::MovFpS) return i.b.k == Operand::K::Slot;
            return i.op == IOp::FpBin || i.op == IOp::FpNeg;
        }
        if (i.a.reg != R::Rax) return false;
        if (i.op == IOp::MovRS) return i.b.k == Operand::K::Slot; // snapshot store
        switch (i.op) {
            case IOp::ArithRR:
            case IOp::ArithRImm:
            case IOp::ShiftImm:
            case IOp::ShiftCl:
            case IOp::Neg:
            case IOp::Not:
                return true;
            default:
                return false;
        }
    }

    // A "gap" instruction between chain ops: never reads or writes the
    // accumulator, and writes only non-allocator registers (scratch or the
    // const pool) — so the fused op running on Z cannot interact with it.
    //   * B-operand loads into the fixed source scratch (mov rcx/xmm1, ...)
    //   * FP constant materialization pairs [movq $bits, %rax][movq %rax,
    //     %xmmN(pool)] — present inside loops at RA time; pass 87 hoists
    //     them out afterwards, but the chain must survive them NOW or the
    //     a-chain of `a = a*c + d` never fuses
    //   * FpZero (+0.0 materialization into the scratch)
    bool is_gap_ok(const Inst& i, bool fp) const {
        if (i.op == IOp::Nop) return true;
        if (i.a.k != Operand::K::Reg) return false;
        R acc = fp ? R::Xmm0 : R::Rax;
        if (i.a.reg == acc) return false;
        if (i.b.k == Operand::K::Reg && i.b.reg == acc) return false;
        switch (i.op) {
            case IOp::MovRR:   // B-operand load (GP): mov rcx, X
            case IOp::MovSR:   // B-operand load (GP, memory): mov rcx, [s]
            case IOp::MovFpFp: // B-operand load (FP): movsd xmm1, X
            case IOp::MovFpR:  // B-operand load (FP, memory)
            case IOp::MovRImm:      // FP const bits into rax (never the FP acc)
            case IOp::MovFpFromGpr: // FP const bits: rax -> pool xmm
            case IOp::MovFpFromGpr32:
            case IOp::FpZero:       // +0.0 into the scratch
                return true;
            default:
                return false;
        }
    }

    void extract_fuses() {
#ifdef JULES_DEBUG_RA3
        {
            int f = 0;
            for (const Inst& c : lf.code) {
                if (c.op == IOp::Label || c.op == IOp::Jmp || c.op == IOp::Jcc) ++f;
                std::fprintf(stderr, "[ra3] %s%d: op=%d a=(%d,%d,%d) b=(%d,%d,%d)\n",
                             f ? "B" : "P", f ? f : 0, (int)c.op, (int)c.a.k, (int)c.a.reg,
                             (int)c.a.slot, (int)c.b.k, (int)c.b.reg, (int)c.b.slot);
            }
        }
#endif
        for (size_t i = 0; i < lf.code.size(); ++i) {
            const Inst& ld = lf.code[i];
            // slot-load into the accumulator: [MovSR rax, [sA]] or [MovFpR xmm0, [sA]]
            bool fp = ld.op == IOp::MovFpR;
            if (ld.op != IOp::MovSR && ld.op != IOp::MovFpR) continue;
            if (ld.b.k != Operand::K::Slot) continue;
            if (!is_acc(ld.a.reg, fp)) continue;
            i32 a_slot = ld.b.slot;

            // walk the chain: gap instructions and accumulator ops. Gaps
            // (B-operand loads into the fixed source scratch) are legal
            // before the first op too — the isel emits [load A][load B]
            // [op] [store], so the operand load precedes the chain.
            // Snapshot stores [store s_mid <- acc] are chain elements:
            // they read the accumulator without clobbering it (an adjacent
            // reload was already pair-folded; a later reload is a legal
            // gap). The chain TERMINATES at the last store in the run —
            // the walk only breaks at non-chain non-gap instructions, so
            // at most trailing gaps sit between the terminal store and the
            // break; they belong to FOLLOWING nodes and must not join the
            // fuse window (a trailing gap loading sZ is a legitimate
            // post-store use, not an operand clobber).
            size_t j = i + 1;
            u32 ops = 0;                    // acc-WRITING ops only (stores excluded:
            size_t first_write_pos = SIZE_MAX; // stores never clobber the accumulator
            size_t last_store_pos = SIZE_MAX; // terminal (or snapshot) store
            SmallVec<std::pair<i32, size_t>, 8> gap_slots;
            while (j < lf.code.size()) {
                const Inst& c = lf.code[j];
                if (c.op == IOp::Nop) { ++j; continue; }
                if (is_chain_op(c, fp)) {
                    bool is_store = (c.op == IOp::MovFpS || c.op == IOp::MovRS);
                    if (is_store) {
                        last_store_pos = j;
                    } else {
                        ++ops;
                        if (first_write_pos == SIZE_MAX) first_write_pos = j;
                    }
                    ++j;
                    continue;
                }
                if (is_gap_ok(c, fp)) {
                    // remember memory-resident operand loads: if the chain
                    // reads the RESULT slot as an operand (`x = y + x`),
                    // the fused leading move would clobber it before the op
                    if (c.b.k == Operand::K::Slot)
                        gap_slots.push_back({c.b.slot, j});
                    ++j;
                    continue;
                }
                break;
            }
            size_t t = last_store_pos;
            if (t == SIZE_MAX) {
                // BISECT fallback: terminal = store at the break position
                if (j < lf.code.size() && (lf.code[j].op == IOp::MovFpS ||
                                           lf.code[j].op == IOp::MovRS))
                    t = j;
                else continue;
            }
            // trailing gaps after the terminal store: outside the window
            {
                SmallVec<std::pair<i32, size_t>, 8> inside;
                for (const auto& g : gap_slots)
                    if (g.second < t) inside.push_back(g);
                gap_slots = inside;
            }
            // terminal: slot-store from the same accumulator
            const Inst& stt = lf.code[t];
            bool sfp = stt.op == IOp::MovFpS;
            if (stt.op != IOp::MovRS && stt.op != IOp::MovFpS) continue;
            if (stt.b.k != Operand::K::Slot) continue;
            if (sfp != fp) continue;                     // class must match
            if (!is_acc(stt.a.reg, fp)) continue;
            i32 z_slot = stt.b.slot;
            size_t j_end = t; // fuse window end (retarget loop bound)
            // Self-update chains (z == a: `a = a*c + d` through the phi
            // slot itself) fuse IN PLACE only — the copy form degenerates
            // to a self-move. A gap reading the accumulator slot is legal
            // only BEFORE the first chain op (old-value read); after the
            // first op writes Z, a gap would see the new value instead.
            bool self_update = (z_slot == a_slot);
            bool z_gap_after_first_op = false;
            for (const auto& g : gap_slots)
                if (g.first == z_slot && g.second > first_write_pos)
                    { z_gap_after_first_op = true; break; }
            if (z_gap_after_first_op) continue;
            // the result slot must not feed the chain as an operand for
            // distinct-slot chains (leading-move clobber; see rewrite)
            if (!self_update) {
                for (const auto& g : gap_slots)
                    if (g.first == z_slot) { z_slot = -1; break; }
                if (z_slot < 0) continue;
            }

            // In-place validity: the load must be sA's generation's LAST
            // use — after the fused op clobbers the register, sA's old
            // value must have no remaining readers (a later activity of
            // sA is always a redefinition, which is safe).
            const LiveRange& ar = range(a_slot);
            bool last_of_gen = false;
            for (const auto& g : ar.gens)
                if (g.second == static_cast<size_t>(i)) last_of_gen = true;

            Fuse fz;
            fz.load_pos = i;
            fz.store_pos = j_end;
            fz.a_slot = a_slot;
            fz.z_slot = z_slot;
            fz.fp = fp;
            fz.ops = ops;
            fz.inplace_ok = last_of_gen;
            fz.self_update = self_update;
            fz.first_op_pos = first_write_pos;
            fz.gap_slots = gap_slots;
            fuses_.push_back(fz);
            // Coalescing hints: a phi copy (ops == 0) closes the loop cycle
            // — Z IS the phi's next value, so sharing A's register is
            // dynamically sound even for loop-carried A. An accumulator
            // fuse may only hint when A is not loop-carried: a value live
            // across a backedge is re-read on every dynamic iteration, and
            // an in-place op would clobber it before those re-reads (the
            // static "last use" lies — the use itself re-executes).
            // Self-update chains need no hint: A and Z are the same slot,
            // already on one register by construction.
            if (!self_update && (ops == 0 || !range(a_slot).spans_backedge))
                hints_.push_back({z_slot, a_slot});
            // range extension: the fused [mov Z, A] writes Z's register at
            // the load position, so Z's live range must start there
            // (self-update: the load already reads the slot, no extension)
            if (!self_update) {
                LiveRange& zr = range(z_slot);
                if (zr.first_live > i) {
                    zr.first_live = i;
                    zr.starts_at_def = true;
                }
            }
        }
        // ranges were extended: regenerate exact sub-intervals so the
        // retarget's coalescing decisions see the extended birth positions
        compute_gens();
    }

    // ------------------------------------------------------------------
    // Hint retarget (post-assignment coalescing repair).
    //
    // The linear scan assigns hulls, so a loop phi (hull wraps the whole
    // loop) never lands on the same register as its source value (hull
    // nested inside). The exact liveness (gens) says they can share: the
    // phi's old generation ends exactly where the source's begins. Move
    // one partner onto the other's register when the pair is gen-disjoint
    // and no third range claims the register in between.
    // ------------------------------------------------------------------
    bool try_retarget(i32 move, i32 onto) {
        LiveRange& m = range(move);
        LiveRange& o = range(onto);
        R reg = o.assigned;
        if (m.fp != o.fp) return false;
        if (m.crosses_call && !reg_is_callee_saved_gpr(reg)) return false;
        // exact liveness of the pair must be disjoint — with one exception:
        // a TOUCHING boundary (one gen's end == the other's start) is a
        // value handover in a single instruction ([mov Z, X] reads the old
        // and writes the new; a coalesced two-operand op does the same),
        // not an interference.
        auto touch = [](const std::pair<size_t, size_t>& a,
                        const std::pair<size_t, size_t>& b) {
            return a.first <= b.second && b.first <= a.second;
        };
        for (const auto& g1 : m.gens)
            for (const auto& g2 : o.gens) {
                if (!touch(g1, g2)) continue;
                if (g1.second == g2.first || g2.second == g1.first) continue;
                return false; // genuine overlap
            }
        // no third range on the register may overlap the mover's hull
        for (LiveRange& u : ranges) {
            if (&u == &m || &u == &o) continue;
            if (!u.promoted || u.assigned != reg) continue;
            if (u.last_live >= m.first_live && u.first_live <= m.last_live)
                return false;
        }
        m.assigned = reg;
        return true;
    }

    void retarget() {
#ifdef JULES_DEBUG_RA3
        for (const Fuse& f : fuses_)
            std::fprintf(stderr, "[ra3] fuse load@%zu store@%zu a=s%d z=s%d fp=%d ops=%u inpl=%d\n",
                         f.load_pos, f.store_pos, f.a_slot, f.z_slot, (int)f.fp, f.ops,
                         (int)f.inplace_ok);
#endif
        for (auto& h : hints_) {
            if (h.first == h.second) continue;
            LiveRange& z = range(h.first);
            LiveRange& a = range(h.second);
            if (!z.defined || !a.defined) continue;
            if (!z.promoted || !a.promoted) continue;
            if (z.assigned == a.assigned) continue;
            bool r1 = try_retarget(h.first, h.second);
            bool r2 = r1 ? false : try_retarget(h.second, h.first);
            if (r1 || r2) {
                ++lf.ra_coalesced;
#ifdef JULES_DEBUG_RA3
                std::fprintf(stderr, "[ra3] coalesce s%d<-s%d: s%d -> reg %d\n", h.first, h.second,
                             r1 ? h.first : h.second,
                             (int)range(r1 ? h.first : h.second).assigned);
#endif
            }
        }
    }

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
            r.starts_at_def =
                r.defined && r.first_live < lf.code.size() &&
                slot_def_at(lf.code[r.first_live]);
        }

        compute_gens();

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
        const R gp_callee[] = {R::Rbx, R::R12, R::R13, R::R14, R::R15};
        // xmm2..: all XMMs are caller-saved in SysV, so these are only
        // usable for ranges that do not cross calls. The isel FP constant
        // pool owns the top of the bank down to lf.fp_const_min_xmm (it
        // grows from xmm15 when the function has distinct loop constants —
        // a bigger pool trades allocatable registers for zero per-iteration
        // rematerialization, which is the right trade for FP-heavy loops).
        int xmm_hi = lf.fp_const_min_xmm;
        if (xmm_hi > 14) xmm_hi = 14; // pool unused: 14/15 stay reserved
        if (xmm_hi < 3) xmm_hi = 3;   // degenerate: keep at least xmm2
        const R xmm_fixed[] = {R::Xmm2,  R::Xmm3,  R::Xmm4,  R::Xmm5,  R::Xmm6,
                              R::Xmm7,  R::Xmm8,  R::Xmm9,  R::Xmm10, R::Xmm11,
                              R::Xmm12, R::Xmm13};
        const size_t xmm_n = static_cast<size_t>(xmm_hi - 2);

        // Caller-saved pool: usable only for non-crossing GPR ranges.
        // In functions with no calls at all, the argument registers are
        // dead isel territory — nothing ever writes them after the
        // prologue parameter spill (which only READS them). Scanning the
        // emitted stream for actual writes and adopting every untouched
        // argument register relieves exactly the pressure that hurts
        // multi-loop kernels (7+ simultaneously live values vs the 7
        // registers the base pools offer). rax/rcx stay out: Setcc/MovZX
        // and the div/shift contracts write them unconditionally.
        R extra_caller[5];
        u8 n_extra = 0;
        {
            const R cand[] = {R::Rdx, R::Rsi, R::Rdi, R::R8, R::R9};
            auto writes_reg = [&](const Inst& q, R c) {
                auto dst = [&](const Operand& o) {
                    return o.k == Operand::K::Reg && o.reg == c;
                };
                switch (q.op) {
                    case IOp::MovRR: case IOp::MovSR: case IOp::MovRImm:
                    case IOp::ArithRR: case IOp::ArithRImm:
                    case IOp::ShiftImm: case IOp::ShiftCl:
                    case IOp::Neg: case IOp::Not: case IOp::Cmov:
                    case IOp::LoadMem: case IOp::LeaSlot: case IOp::LeaSym:
                    case IOp::SExt32:
                        return dst(q.a);
                    case IOp::Cqo: return c == R::Rdx;
                    case IOp::IDiv: case IOp::UDiv:
                        return c == R::Rax || c == R::Rdx;
                    case IOp::CallFn: case IOp::CallSym: case IOp::TailCallFn:
                        return true; // clobbers every caller-saved register
                    default:
                        return false; // FP ops / stores / labels / branches
                }
            };
            for (R c : cand) {
                bool clean = true;
                for (const Inst& q : lf.code) {
                    if (writes_reg(q, c)) { clean = false; break; }
                }
                if (clean) extra_caller[n_extra++] = c;
            }
        }
        std::vector<R> caller_list;
        caller_list.push_back(R::R10);
        caller_list.push_back(R::R11);
        for (u8 e = 0; e < n_extra; ++e) caller_list.push_back(extra_caller[e]);
        RegPool caller_pool(caller_list.data(), caller_list.size());
        RegPool callee_pool(gp_callee, sizeof gp_callee / sizeof gp_callee[0]);
        RegPool xmm_pool(xmm_fixed, xmm_n < sizeof xmm_fixed / sizeof xmm_fixed[0]
                                         ? xmm_n
                                         : sizeof xmm_fixed / sizeof xmm_fixed[0]);
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
            // expire: ranges that ended before this one starts. A range
            // whose FIRST activity is a def may start exactly at another
            // range's final use (the fused [mov Z, A] reads A and writes Z
            // in one instruction — and a coalesced op reads the old value
            // and writes the new one in the same instruction), so the
            // touching case `u.last_live == r.first_live` is a handover,
            // not an interference — but only when the newcomer begins
            // with a def. A newcomer that begins with a USE genuinely
            // overlaps (both values live at that position).
            for (Active& a : active) {
                if (a.r->last_live < r->first_live) a.dead = true;
                else if (a.r->last_live == r->first_live && r->starts_at_def)
                    a.dead = true;
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

    // Slot use positions (collected pre-promotion, when operands still
    // carry Slot ids) and promoted home-store positions (collected during
    // the promotion loop). Together they drive single-use def forwarding:
    // a promoted value with exactly one use whose load the operand folds
    // already absorbed — its home store can die and the consumer can read
    // the value straight from the store's source register.
    std::vector<std::vector<size_t>> slot_use_pos_;
    std::vector<std::vector<size_t>> slot_def_pos_;
    FlatMap<size_t, i32> home_store_;
    FlatMap<size_t, i32> use_slot_at_; // use position -> the slot it read

    void rewrite() {
        // (pair folding ran before liveness analysis — see run())

        slot_use_pos_.assign(static_cast<size_t>(lf.slot_count > 0 ? lf.slot_count : 0), {});
        slot_def_pos_.assign(static_cast<size_t>(lf.slot_count > 0 ? lf.slot_count : 0), {});
        home_store_.clear();
        use_slot_at_.clear();
        for (size_t p = 0; p < lf.code.size(); ++p) {
            i32 s = 0;
            bool f = false, a = false;
            if (slot_use_inst(lf.code[p], s, f, a)) {
                slot_use_pos_[static_cast<size_t>(s)].push_back(p);
                use_slot_at_.insert(static_cast<i32>(p), s);
            }
            if (slot_def_inst(lf.code[p], s, f))
                slot_def_pos_[static_cast<size_t>(s)].push_back(p);
        }

        for (size_t p = 0; p < lf.code.size(); ++p) {
            Inst& i = lf.code[p];
            if ((i.op == IOp::MovRS || i.op == IOp::MovFpS) && i.b.k == Operand::K::Slot) {
                const LiveRange& r = range(i.b.slot);
                if (!r.promoted) continue;
                home_store_.insert(static_cast<i32>(p), i.b.slot);
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

        // (positional promotion loop ends above)

        // Next live (non-Nop) index at-or-after `from`: folds must look
        // THROUGH instructions earlier folds killed — adjacency-by-
        // position alone misses the A-side setup mov once the B-side mov
        // became a Nop between it and the consumer.
        auto next_live = [&](size_t from) -> size_t {
            size_t j = from;
            while (j < lf.code.size() && lf.code[j].op == IOp::Nop) ++j;
            return j;
        };

        // FP operand folding: the isel contract loads FpBin/FpCmp operands
        // into xmm0/xmm1 via MovFpFp (after promotion). A register-resident
        // source feeding the fixed src register can fold straight into the
        // consumer's operand, killing the movsd: SSE is two-operand, and
        // FpBin/FpCmp/FpNeg take real register operands now. FpCmp is
        // non-destructive (flags only), so its *destination* operand folds
        // too: [movsd xmm0, X][ucomisd xmm1, xmm0] -> [ucomisd xmm1, X].
        // Fixpoint: the isel emits up to two setup movs per consumer
        // (A-then-B); one pass folds only the LAST one, because killing it
        // is what makes the earlier one adjacent to the consumer. Loop
        // until no fold fires (bounded by setup-mov count per inst).
        for (int round = 0; round < 4; ++round) {
        bool folded_this_round = false;
        for (size_t i = 0; i + 1 < lf.code.size(); ++i) {
            Inst& mov = lf.code[i];
            if (mov.op != IOp::MovFpFp || mov.a.k != Operand::K::Reg ||
                mov.b.k != Operand::K::Reg)
                continue;
            size_t ui = next_live(i + 1);
            if (ui >= lf.code.size()) continue;
            Inst& use = lf.code[ui];
            IOp consumer = use.op;
            if (consumer != IOp::FpBin && consumer != IOp::FpCmp) continue;
            // B-side: the mov loads the consumer's SOURCE register
            if (use.b.k == Operand::K::Reg && use.b.reg == mov.a.reg &&
                mov.b.reg != use.a.reg) {
                use.b.reg = mov.b.reg;
                mov.op = IOp::Nop;
                folded_this_round = true;
                continue;
            }
            // A-side (non-destructive compare only)
            if (consumer == IOp::FpCmp && use.a.k == Operand::K::Reg &&
                use.a.reg == mov.a.reg &&
                !(use.b.k == Operand::K::Reg && use.b.reg == mov.b.reg)) {
                use.a.reg = mov.b.reg;
                mov.op = IOp::Nop;
                folded_this_round = true;
            }
        }
        if (!folded_this_round) break;
        }

        // GP operand folding (mirror of the FP fold): the isel contract
        // loads ArithRR/CmpRR operands into rax (destination) / rcx
        // (source). A register-resident source feeding the fixed scratch
        // register folds straight into the consumer's operand, killing the
        // mov — compare/branch pairs collapse to `cmp %reg, %reg` with no
        // setup moves, and binary ops read their operands directly out of
        // promoted registers. CmpRR/Test are non-destructive (flags), so
        // the destination operand folds as well.
        // Fixpoint (same reason as the FP fold): a consumer with two setup
        // movs [mov rax, A][mov rcx, B][cmp] only exposes the rax mov after
        // the rcx mov dies; without the re-run the loop guard kept
        // `mov %r10, %rax; cmp %rdx, %rax` — the bound never landed in the
        // compare's operand slot.
        for (int round = 0; round < 4; ++round) {
        bool folded_this_round = false;
        for (size_t i = 0; i + 1 < lf.code.size(); ++i) {
            Inst& mov = lf.code[i];
            if (mov.op != IOp::MovRR || mov.a.k != Operand::K::Reg ||
                mov.b.k != Operand::K::Reg)
                continue;
            size_t ui = next_live(i + 1);
            if (ui >= lf.code.size()) continue;
            Inst& use = lf.code[ui];
            // only isel scratch targets: never touch allocator-homed moves
            if (mov.a.reg != R::Rax && mov.a.reg != R::Rcx) continue;
            if (use.op == IOp::ArithRR) {
                if (use.b.k == Operand::K::Reg && use.b.reg == mov.a.reg &&
                    mov.b.reg != use.a.reg) {
                    use.b.reg = mov.b.reg;
                    mov.op = IOp::Nop;
                    folded_this_round = true;
                    continue;
                }
                // A-side fold for DESTRUCTIVE consumers (the op writes its
                // a operand) is coalescing-unsafe: mov.b.reg may be the
                // home of OTHER gen-disjoint slots whose uses after this
                // point would read the op's result instead of their value.
                // The compare-and-branch family (non-destructive) keeps its
                // A-side fold; destructive ops keep the one setup mov.
            } else if (use.op == IOp::CmpRR) {
                if (use.b.k == Operand::K::Reg && use.b.reg == mov.a.reg &&
                    mov.b.reg != use.a.reg) {
                    use.b.reg = mov.b.reg; // B-side
                    mov.op = IOp::Nop;
                    folded_this_round = true;
                } else if (use.a.k == Operand::K::Reg && use.a.reg == mov.a.reg &&
                           !(use.b.k == Operand::K::Reg && use.b.reg == mov.b.reg)) {
                    use.a.reg = mov.b.reg; // A-side (non-destructive)
                    mov.op = IOp::Nop;
                    folded_this_round = true;
                }
            } else if (use.op == IOp::Test) {
                if (use.a.k == Operand::K::Reg && use.a.reg == mov.a.reg) {
                    use.a.reg = mov.b.reg;
                    mov.op = IOp::Nop;
                    folded_this_round = true;
                }
            } else if (use.op == IOp::CmpRImm) {
                // [mov rax, X][cmp $imm, rax] -> [cmp $imm, X]: loop guards
                // compare the IV against an invariant constant
                if (use.a.k == Operand::K::Reg && use.a.reg == mov.a.reg) {
                    use.a.reg = mov.b.reg;
                    mov.op = IOp::Nop;
                    folded_this_round = true;
                }
            }
        }
        if (!folded_this_round) break;
        }

        // ------------------------------------------------------------------
        // Single-use home-store forwarding. A promoted value with exactly
        // one use, whose load the operand folds already absorbed, keeps a
        // round trip [movsd H, S] ... [consumer reads H]: the value went to
        // its home register only to be read right back. When nothing in
        // between touches H or clobbers S, the consumer reads S directly
        // and the home store dies. The single-use property (from the
        // pre-promotion slot scan) plus the RA's interference-free
        // assignment (no other slot on H is live inside the window) make
        // the later readers question moot: any later H reader belongs to a
        // slot whose own def rewrites H first.
        // ------------------------------------------------------------------
        {
            // The window scan must block on ANY reference to H (the home)
            // or of S (the forwarded source — an isel SCRATCH that every
            // later isel op reuses: setcc/movzx write AL/RAX, arith writes
            // its accumulator, loads write their target). Reference-level
            // blocking is strictly sound; a setcc-writes-AL gap here left
            // a hoisted `a < b` bool in %rax across a loop whose body's
            // own setcc clobbered it (t19 @ -Oz).
            auto reg_ref = [](const Inst& q, R x) {
                if (q.a.k == Operand::K::Reg && q.a.reg == x) return true;
                if (q.b.k == Operand::K::Reg && q.b.reg == x) return true;
                return false;
            };
            for (const auto& e : home_store_.entries()) {
                size_t d = static_cast<size_t>(e.first);
                i32 s = e.second;
                if (s < 0 || static_cast<size_t>(s) >= slot_use_pos_.size()) continue;
                Inst& mov = lf.code[d];
                if (mov.op != IOp::MovFpFp && mov.op != IOp::MovRR) continue;
                if (mov.a.k != Operand::K::Reg || mov.b.k != Operand::K::Reg) continue;
                R H = mov.a.reg, S = mov.b.reg;
                if (H == S) continue;
                // single-def AND single-use: a phi slot has one def per
                // predecessor (a merge!), and forwarding one def's source
                // register to the consumer silently breaks every other
                // def's path — the consumer would read whatever register
                // happens to be live there instead of the merged value.
                if (slot_use_pos_[static_cast<size_t>(s)].size() != 1 ||
                    slot_def_pos_[static_cast<size_t>(s)].size() != 1) continue;
                size_t u = slot_use_pos_[static_cast<size_t>(s)][0];
                if (u <= d || u + 1 >= lf.code.size()) continue;
                if (lf.code[u].op != IOp::Nop) continue; // use-load survived the folds
                Inst& consumer = lf.code[u + 1];
                // locate the H-reading operand to forward
                bool fwd_b = false, fwd_a = false;
                switch (consumer.op) {
                    case IOp::FpBin: case IOp::ArithRR: case IOp::Cmov:
                        if (consumer.b.k == Operand::K::Reg && consumer.b.reg == H)
                            fwd_b = true;
                        break;
                    case IOp::CmpRR: case IOp::FpCmp:
                        if (consumer.b.k == Operand::K::Reg && consumer.b.reg == H)
                            fwd_b = true;
                        else if (consumer.a.k == Operand::K::Reg && consumer.a.reg == H)
                            fwd_a = true;
                        break;
                    case IOp::Test:
                        if (consumer.a.k == Operand::K::Reg && consumer.a.reg == H)
                            fwd_a = true;
                        break;
                    default: break;
                }
                if (!fwd_b && !fwd_a) continue;
                // the consumer must not write S (its result would replace
                // S's value for downstream readers)
                if (consumer.a.k == Operand::K::Reg && consumer.a.reg == S) continue;
                // gap window (d, u): nothing may reference H or S
                bool ok = true;
                for (size_t k = d + 1; k < u; ++k) {
                    const Inst& q = lf.code[k];
                    if (q.op == IOp::Nop) continue;
                    if (reg_ref(q, H) || reg_ref(q, S)) { ok = false; break; }
                }
                if (!ok) continue;
                if (fwd_b) consumer.b.reg = S;
                if (fwd_a) consumer.a.reg = S;
                mov.op = IOp::Nop;
            }
        }

        // ------------------------------------------------------------------
        // Accumulator-chain fusion (the pre-registered Fuses). Post-
        // promotion, the chain has one of these shapes:
        //   [mov acc, X]  (A promoted)   or  [mov acc, [sA]]  (A memory)
        //   [op acc, ...] xN
        //   [mov Z, acc]  (Z promoted)   or  [mov [sZ], acc]  (Z memory)
        // Rewrite, when Z is promoted:
        //   [mov Z, X][op Z, ...]         (A register-resident)
        //   [mov Z, [sA]][op Z, ...]      (A memory-resident)
        // and when Z's register == A's register (hint coalescing landed
        // AND the load is A's generation's last use), the leading move
        // disappears entirely: the op runs in place, exactly like the
        // two-operand forms a production compiler emits. Zero-op chains
        // (phi copies) whose slots ended up on the same register simply
        // evaporate — the value is already in the right home.
        // ------------------------------------------------------------------
        {
            u32 fused = 0;
            for (const Fuse& f : fuses_) {
                Inst& load = lf.code[f.load_pos];
                Inst& store = lf.code[f.store_pos];
                if (load.op == IOp::Nop || store.op == IOp::Nop) continue;
                const LiveRange& zr = range(f.z_slot);
                if (!zr.promoted) continue; // result must be register-resident
                R acc = f.fp ? R::Xmm0 : R::Rax;
                R Z = zr.assigned;

                // A's home: register X or memory slot
                bool a_is_reg = (load.op == (f.fp ? IOp::MovFpFp : IOp::MovRR)) &&
                                load.a.k == Operand::K::Reg && load.a.reg == acc &&
                                load.b.k == Operand::K::Reg;
                R X = a_is_reg ? load.b.reg : R::Rax;

                bool same_reg = a_is_reg && X == Z;
                // coalescing may have landed an operand of this very chain
                // on Z's register (slot-identity is not enough — registers
                // are shared): the leading [mov Z, X] / in-place op would
                // clobber the operand before the chain reads it.
                // In-place exception: a gap that reads Z's register BEFORE
                // the first chain op reads the OLD value ([mulsd Z, g] with
                // g loaded first) — that is exactly what the original code
                // did. Gaps after the first op see the NEW value and stay
                // forbidden. Copy-form chains have their leading move at
                // load_pos, before every gap: any Z-register gap is a
                // clobber, no exception.
                bool operand_on_z = false;
                for (const auto& g : f.gap_slots) {
                    const LiveRange& gr = range(g.first);
                    if (gr.promoted && gr.assigned == Z) {
                        if (same_reg && g.second < f.first_op_pos) continue;
                        operand_on_z = true;
                        break;
                    }
                }
                if (operand_on_z) continue;

                if (same_reg && f.ops > 0 && !f.inplace_ok) continue;
                // (in-place would clobber A while its old value still has
                // readers; the copy form degenerates to a self-move)

                // retarget every chain op from the accumulator to Z
                // (ops carry the acc in `a`; promoted snapshot stores carry
                // it in `b` — [MovFpFp H_mid <- acc] reads the accumulator)
                for (size_t k = f.load_pos + 1; k < f.store_pos; ++k) {
                    Inst& c = lf.code[k];
                    if (c.op == IOp::Nop) continue;
                    if (c.a.k == Operand::K::Reg && c.a.reg == acc) {
                        c.a.reg = Z;
                    } else if ((c.op == IOp::MovFpFp || c.op == IOp::MovRR) &&
                               c.b.k == Operand::K::Reg && c.b.reg == acc) {
                        c.b.reg = Z;
                    }
                }

                if (same_reg) {
                    // in place: no leading move at all
                    load.op = IOp::Nop;
                } else if (a_is_reg) {
                    load.op = f.fp ? IOp::MovFpFp : IOp::MovRR;
                    load.a.k = Operand::K::Reg;
                    load.a.reg = Z;          // dst
                    load.b.k = Operand::K::Reg;
                    load.b.reg = X;          // src (unchanged)
                } else {
                    // A memory-resident: load straight into Z
                    load.a.reg = Z;          // (op stays MovSR / MovFpR)
                }
                store.op = IOp::Nop;         // the result is already home
                ++fused;
#ifdef JULES_DEBUG_RA3
                std::fprintf(stderr, "[ra3] fuse APPLIED a=s%d(z=s%d) same=%d Z=%d X=%d\n",
                             f.a_slot, f.z_slot, (int)same_reg, (int)Z, (int)X);
#endif
            }
            lf.ra_fused += fused;
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
        lf.slot_offset.assign(static_cast<size_t>(lf.slot_count > 0 ? lf.slot_count : 0), 0);
        lf.slot_reg.clear();
        std::vector<R> callee_used;
        for (const LiveRange& r : ranges) {
            if (r.promoted && reg_is_callee_saved_gpr(r.assigned))
                callee_used.push_back(r.assigned);
        }
        // Several promoted ranges can share one callee-saved register (hint
        // coalescing / pool reuse). Each physical register is saved exactly
        // once: a duplicate push would shift every rbp-relative slot below
        // it and double every restore.
        {
            std::sort(callee_used.begin(), callee_used.end(), [](R a, R b) {
                return static_cast<int>(a) < static_cast<int>(b);
            });
            callee_used.erase(std::unique(callee_used.begin(), callee_used.end()),
                              callee_used.end());
        }

        // ---- rebuild the instruction stream ----
        std::vector<Inst> out;
        out.reserve(lf.code.size() + 2 * callee_used.size() + 8);
        bool pushed = false;
        bool frame_patched = false;
        i32 unpromoted = 0;
        for (i32 s = 0; s < lf.slot_count; ++s)
            if (range(s).needs_memory()) ++unpromoted;

        i32 callee_area = 8 * static_cast<i32>(callee_used.size());

        // ---- stack slot coloring (pass 28 at machine level) -------------
        // Two memory slots whose live ranges are disjoint share one frame
        // offset — the classic coloring over spill slots, driven by the
        // same exact live ranges the register assignment used. Address-
        // taken slots never share: distinct allocations must keep distinct
        // addresses. Multi-def slots use their hull (conservative).
        std::vector<i32> color_of(static_cast<size_t>(lf.slot_count), -1);
        i32 distinct_slots = 0;
        {
            struct Hole { i32 slot; size_t first, last; };
            std::vector<Hole> holes;
            for (i32 s = 0; s < lf.slot_count; ++s) {
                if (color_of[static_cast<size_t>(s)] != -1) continue; // (none yet)
                const LiveRange& r = range(s);
                if (r.promoted || !r.needs_memory() || r.addr_taken) continue;
                size_t lo = r.first_live, hi = r.last_live;
                for (const auto& g : r.gens) {
                    lo = std::min(lo, g.first);
                    hi = std::max(hi, g.second);
                }
                holes.push_back(Hole{s, lo, hi});
            }
            std::vector<size_t> color_last; // last activity per offset
            for (Hole& h : holes) {
                i32 c = -1;
                for (size_t k = 0; k < color_last.size(); ++k) {
                    if (color_last[k] < h.first) { // strictly before: disjoint
                        c = static_cast<i32>(k);
                        color_last[k] = h.last;
                        break;
                    }
                }
                if (c < 0) {
                    color_last.push_back(h.last);
                    c = static_cast<i32>(color_last.size() - 1);
                }
                color_of[static_cast<size_t>(h.slot)] = c;
            }
            distinct_slots = static_cast<i32>(color_last.size());
            // every un-shared memory slot (addr-taken or unmatched) keeps
            // its own offset beyond the colored ones
            i32 extra = 0;
            for (i32 s = 0; s < lf.slot_count; ++s) {
                if (color_of[static_cast<size_t>(s)] != -1) continue;
                const LiveRange& r = range(s);
                if (r.promoted || !r.needs_memory()) continue;
                color_of[static_cast<size_t>(s)] = distinct_slots + extra;
                ++extra;
            }
            distinct_slots += extra;
            // record the offsets (callee_area known here)
            for (i32 s = 0; s < lf.slot_count; ++s) {
                i32 c = color_of[static_cast<size_t>(s)];
                if (c < 0) continue;
                lf.slot_offset[static_cast<size_t>(s)] =
                    -(callee_area + 8 * (c + 1));
            }
            lf.ra_colored = static_cast<u32>(
                static_cast<i32>(holes.size()) > distinct_slots
                    ? static_cast<i32>(holes.size()) - distinct_slots
                    : 0);
        }
        i32 slot_area = 8 * distinct_slots;
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
        pair_fold();      // same-slot store/load adjacency — before analysis
        analyze();        // ranges, generations, exact sub-intervals
        extract_fuses();  // accumulator chains + coalescing hints
        assign(size_biased);
        retarget();       // unify hinted pairs onto shared registers
        rewrite();        // promotion + operand folds + fuse application
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
