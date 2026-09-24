// The JULES superoptimizer engine — implementation. See superopt.h for the
// design contract; nothing below knows an opcode name.
#include "superopt.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace jules::superopt {
namespace {

// ---- deterministic value derivation ------------------------------------

u64 splitmix64(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Directed corner values — TEST-VECTOR construction policy: generic
// input-domain corners (boundary integers, powers of two, alternating bit
// patterns). These seed the search lanes, the verification lanes and the
// immediate pool; they carry no knowledge of any optimization.
constexpr i64 kCorners[] = {
    0, 1, -1, 2, -2, 3, 7, 15, 16, -16, 31, 32,
    0x7F, -128, 0x7FFF, -32768,
    0x7FFFFFFF, -2147483648LL, 0xFFFFFFFFLL, 4294967295LL,
    static_cast<i64>(0x5555555555555555ull),
    static_cast<i64>(0xAAAAAAAAAAAAAAAAull),
    static_cast<i64>(0x9E3779B97F4A7C15ull),
};
constexpr u32 kNCorners = sizeof(kCorners) / sizeof(kCorners[0]);

// The search runs one directed batch; the commit gate re-verifies on this
// many batches (directed + deterministic random). 7 * 12 = 84 fresh lanes.
constexpr u32 kVerifyBatches = 7;

// LANE LAYOUT (test-vector POLICY — generic input concretization, the
// standard superoptimizer defense against the "condition never fires on
// random inputs" hole: a window full of `cmp $K / cmovcc` looks like an
// identity when no lane's register ever equals K, and the search happily
// erases the whole conditional):
//   lanes [0, kSweepLanes)  — corner/random per batch (general behavior)
//   lanes [kSweepLanes, ..) — CONSTANT SWEEP: every location holds one of
//                             the window's own immediate values, so every
//                             compare-against-a-window-constant FIRES on
//                             some lane, in the search AND in the commit
//                             gate's re-verification.
constexpr u32 kSweepLanes = kVecSearch / 2;

void seed_state(VState& st, u32 batch, u32 nslots, const i64* imms, u32 nims) {
    i64 lane_base[kVecSearch];
    for (u32 v = 0; v < kSweepLanes; ++v) {
        if (batch == 0) {
            lane_base[v] = kCorners[(v * kNCorners) / kSweepLanes];
        } else {
            lane_base[v] = static_cast<i64>(
                splitmix64(0xC0FFEE123456789ull ^ (0xABCDEFull * batch) ^
                           (0x2545F4914F6CDD1Dull * v)));
        }
    }
    for (u32 v = 0; v < kVecSearch; ++v) {
        if (v >= kSweepLanes && nims) {
            // constant-sweep lane: one window constant everywhere
            i64 c = imms[(v - kSweepLanes) % nims];
            for (u32 g = 0; g < kMaxGpr; ++g) st.r[v][g] = c;
            for (u32 k = 0; k < kMaxSlot; ++k) st.s[v][k] = c;
            for (u32 f = 0; f < kNF; ++f) st.f[v][f] = c & 1;
            continue;
        }
        for (u32 g = 0; g < kMaxGpr; ++g)
            st.r[v][g] = static_cast<i64>(splitmix64(
                splitmix64(static_cast<u64>(lane_base[v % kSweepLanes])) ^
                (0x9E3779B9ull * (g + 1))));
        for (u32 k = 0; k < kMaxSlot; ++k)
            st.s[v][k] = k < nslots ? static_cast<i64>(splitmix64(
                                          splitmix64(static_cast<u64>(lane_base[v % kSweepLanes])) ^
                                          (0x9E3779B9ull * (kMaxGpr + k + 1))))
                                    : 0;
        for (u32 f = 0; f < kNF; ++f)
            st.f[v][f] = splitmix64(splitmix64(static_cast<u64>(lane_base[v % kSweepLanes])) ^
                                   (0x9E3779B9ull * (kMaxGpr + kMaxSlot + f + 1))) & 1;
    }
}

// ---- table lookup ---------------------------------------------------------

struct TableIndex {
    std::vector<const Row*> by_op; // dense over IOp value
    explicit TableIndex(const IsaTable& isa) {
        u32 max_op = 0;
        for (const Row& r : isa)
            max_op = std::max(max_op, static_cast<u32>(r.op));
        by_op.assign(static_cast<size_t>(max_op) + 1, nullptr);
        for (const Row& r : isa) {
            const Row*& slot = by_op[static_cast<size_t>(r.op)];
            if (!slot || r.sim) slot = &r; // searchable row wins over a
                                           // boundary row for the same op
        }
    }
};

bool row_effects(const TableIndex& idx, const Inst& i, Effects& e) {
    if (static_cast<u32>(i.op) >= idx.by_op.size()) return false;
    const Row* r = idx.by_op[static_cast<size_t>(i.op)];
    if (!r || !r->effects) return false;
    r->effects(i, e);
    return true;
}

// ---- incremental fingerprint (the throughput-critical restructure) -------
//
// The search constructs MILLIONS of candidate states per window; hashing
// each from scratch (the splitmix chain above) costs ~1.3us per candidate
// and dominated the entire engine. Instead the fingerprint is an XOR-fold
// of per-position terms:
//
//     fp(S) = XOR over hashed positions p of term(Z[p], S[p])
//     term(z, x) = avalanche((z ^ x) * PHI)
//
// Hashed positions: (gpr, lane), (window slot, lane), (flag, lane) — the
// SAME location set the chain above hashes. The fold is a PURE function of
// the state: any two paths that reach the same state produce the same
// fingerprint, so it is path-independent under incremental update — remove
// the old term, add the new one. A write updates only its own positions.
//
// Collision behavior is unchanged in KIND from the previous 64-bit hash:
// a collision can only lose a win (a candidate is discarded as a duplicate
// state), never invent one — the goal test compares exact states.

inline u64 term64(u64 z, u64 x) {
    u64 t = (z ^ x) * 0x9E3779B97F4A7C15ull;
    return t ^ (t >> 31);
}

struct Zobrist {
    // [position][lane]; positions: kMaxGpr GPRs, kMaxSlot slots, kNF flags
    u64 z[kMaxGpr + kMaxSlot + kNF][kVecSearch];
    Zobrist() {
        u64 s = 0x5DEECE66Dull;
        for (auto& row : z)
            for (u64& x : row) x = splitmix64(s = splitmix64(s));
    }
};
const Zobrist& ztable() {
    static Zobrist z;
    return z;
}

u64 fp_full(const VState& st, u32 nslots) {
    const Zobrist& Z = ztable();
    u64 h = 0;
    for (u32 v = 0; v < kVecSearch; ++v) {
        for (u32 g = 0; g < kMaxGpr; ++g)
            h ^= term64(Z.z[g][v], static_cast<u64>(st.r[v][g]));
        for (u32 k = 0; k < nslots; ++k)
            h ^= term64(Z.z[kMaxGpr + k][v], static_cast<u64>(st.s[v][k]));
        for (u32 f = 0; f < kNF; ++f)
            h ^= term64(Z.z[kMaxGpr + kMaxSlot + f][v],
                        static_cast<u64>(st.f[v][f]));
    }
    return h;
}

// ---- write-set journal -------------------------------------------------
//
// Candidates simulate IN PLACE on the parent state (the 2.5KB VState copy
// per construction was the second-largest cost). The journal saves the
// write set BEFORE the sim (the set comes from the ISA row's Effects — the
// same single source the liveness dataflow trusts) and restores it after,
// keeping the parent pristine for the next construction.
struct Journal {
    u32 gr[4];               // written GPR indices (e.g. IDiv/MulHi write 2)
    u32 ngr = 0;
    i64 r[kVecSearch][4];
    bool slot = false;      // window-local index of a written slot
    u32 slot_idx = 0;
    i64 s[kVecSearch];
    bool flags = false;     // ANY flag write (defined or undefined-poison)
    i64 f[kVecSearch][kNF];
};

void journal_save(Journal& j, const VState& st, const Effects& e,
                  const i32* wslots, u32 nslots) {
    j.ngr = 0;
    for (u32 g = 0; g < kMaxGpr; ++g)
        if (e.gpr_write & (1u << g)) {
            // A row writing more than 4 GPRs would silently corrupt the
            // parent state (the journal would drop writes). No current row
            // exceeds 2 (IDiv/MulHi: rax+rdx) — this abort is the invariant
            // gate for future ISA rows.
            if (j.ngr == 4) {
                std::fprintf(stderr,
                             "[superopt] journal overflow: op writes >4 GPRs "
                             "(raise Journal::gr)\n");
                std::abort();
            }
            j.gr[j.ngr++] = g;
        }
    for (u32 v = 0; v < kVecSearch; ++v)
        for (u32 i = 0; i < j.ngr; ++i) j.r[v][i] = st.r[v][j.gr[i]];
    j.slot = false;
    if (e.slot_write >= 0) {
        for (u32 s = 0; s < nslots; ++s) {
            if (wslots[s] == e.slot_write) {
                j.slot = true;
                j.slot_idx = s;
                break;
            }
        }
        if (j.slot)
            for (u32 v = 0; v < kVecSearch; ++v) j.s[v] = st.s[v][j.slot_idx];
    }
    j.flags = e.writes_flags || e.undef_flags;
    if (j.flags)
        for (u32 v = 0; v < kVecSearch; ++v)
            for (u32 f = 0; f < kNF; ++f) j.f[v][f] = st.f[v][f];
}

void journal_restore(VState& st, const Journal& j) {
    for (u32 v = 0; v < kVecSearch; ++v) {
        for (u32 i = 0; i < j.ngr; ++i) st.r[v][j.gr[i]] = j.r[v][i];
        if (j.slot) st.s[v][j.slot_idx] = j.s[v];
        if (j.flags)
            for (u32 f = 0; f < kNF; ++f) st.f[v][f] = j.f[v][f];
    }
}

// XOR-fold delta over the journaled positions: old terms out, new in.
u64 journal_fp_delta(const VState& st, const Journal& j, u32 nslots) {
    const Zobrist& Z = ztable();
    u64 d = 0;
    for (u32 v = 0; v < kVecSearch; ++v) {
        for (u32 i = 0; i < j.ngr; ++i) {
            const u32 g = j.gr[i];
            d ^= term64(Z.z[g][v], static_cast<u64>(j.r[v][i])) ^
                 term64(Z.z[g][v], static_cast<u64>(st.r[v][g]));
        }
        if (j.slot) {
            const u32 k = j.slot_idx;
            d ^= term64(Z.z[kMaxGpr + k][v], static_cast<u64>(j.s[v])) ^
                 term64(Z.z[kMaxGpr + k][v], static_cast<u64>(st.s[v][k]));
        }
        if (j.flags)
            for (u32 f = 0; f < kNF; ++f)
                d ^= term64(Z.z[kMaxGpr + kMaxSlot + f][v],
                            static_cast<u64>(j.f[v][f])) ^
                     term64(Z.z[kMaxGpr + kMaxSlot + f][v],
                            static_cast<u64>(st.f[v][f]));
    }
    return d;
}

// ---- flat open-addressing maps (dedup + expanded-set) --------------------
// Linear probing, fixed capacity per window (bounded by the states budget —
// never grows, never rehashes; the std::unordered_map traffic was the last
// measurable per-construction cost after the fingerprint and the copy).
struct FlatMap {
    std::vector<u64> fps;
    std::vector<i32> costs;
    std::vector<u8> used;
    size_t mask = 0;
    size_t count = 0;
    size_t cap = 0;

    void init(size_t want) {
        size_t n = 16;
        while (n < want * 2) {
            n <<= 1;
            if (n == 0) { n = 1; break; } // overflow guard (absurd budgets)
        }
        fps.assign(n, 0);
        costs.assign(n, 0);
        used.assign(n, 0);
        mask = n - 1;
        count = 0;
        cap = n;
    }

    // True when `cost` improves on the recorded cost for `fp` (or the fp
    // is new) — i.e. the caller should push. Mirrors the old
    // find-then-maybe-insert on the unordered_map.
    bool improve(u64 fp, i32 cost) {
        size_t i = fp & mask;
        while (used[i]) {
            if (fps[i] == fp) {
                if (costs[i] <= cost) return false;
                costs[i] = cost;
                return true;
            }
            i = (i + 1) & mask;
        }
        if (count * 2 >= cap) return false; // load factor gate (treat as cap)
        used[i] = 1;
        fps[i] = fp;
        costs[i] = cost;
        ++count;
        return true;
    }
};

struct FlatSet {
    std::vector<u64> fps;
    std::vector<u8> used;
    size_t mask = 0;
    size_t count = 0;
    size_t cap = 0;

    void init(size_t want) {
        size_t n = 16;
        while (n < want * 2) {
            n <<= 1;
            if (n == 0) { n = 1; break; }
        }
        fps.assign(n, 0);
        used.assign(n, 0);
        mask = n - 1;
        count = 0;
        cap = n;
    }

    bool insert(u64 fp) {
        size_t i = fp & mask;
        while (used[i]) {
            if (fps[i] == fp) return false;
            i = (i + 1) & mask;
        }
        if (count * 2 >= cap) return true; // full: claim present (stop)
        used[i] = 1;
        fps[i] = fp;
        ++count;
        return true;
    }
};

// ---- regions (label-delimited basic blocks of the machine code) ----------

struct Regions {
    std::vector<u32> begin, end; // inst ranges [begin, end)
    std::vector<u32> region_of;   // inst -> region index
    std::vector<std::vector<u32>> succs;
    std::unordered_map<int, u32> label_region; // label id -> region that
                                                // FOLLOWS the Label inst
};

// Control-flow SKELETON (labels and branch/terminator shape), not
// instruction semantics — the one structural fact the engine needs to cut
// regions; per-instruction behavior stays in the ISA table. (Review note 10:
// a purist table would carry an is_branch bit; not worth the coupling.)
bool is_label(const Inst& i) { return i.op == IOp::Label; }
bool is_terminator(const Inst& i) {
    switch (i.op) {
        case IOp::Jmp:
        case IOp::Jcc:
        case IOp::Ret:
        case IOp::RetNaked:
        case IOp::TailCallFn:
        case IOp::TailCallNaked: return true;
        default: return false;
    }
}

void build_regions(const LFunction& lf, Regions& rg) {
    const std::vector<Inst>& code = lf.code;
    const size_t n = code.size();
    rg.region_of.assign(n, 0);
    u32 start = 0;
    if (n > 0 && is_label(code[0])) {
        rg.label_region[code[0].a.label] = 0;
        start = 1;
    }
    u32 r = 0;
    rg.begin.push_back(start);
    for (size_t i = start; i < n; ++i) {
        if (is_label(code[i])) {
            rg.end.push_back(static_cast<u32>(i));
            rg.begin.push_back(static_cast<u32>(i + 1));
            rg.label_region[code[i].a.label] = ++r;
        }
        rg.region_of[i] = r;
    }
    rg.end.push_back(static_cast<u32>(n));

    rg.succs.assign(rg.begin.size(), {});
    for (u32 k = 0; k < rg.begin.size(); ++k) {
        std::vector<u32>& s = rg.succs[k];
        bool exited = false;
        for (u32 i = rg.begin[k]; i < rg.end[k]; ++i) {
            const Inst& in = code[i];
            if (in.op == IOp::Jmp) {
                auto it = rg.label_region.find(in.a.label);
                if (it != rg.label_region.end()) s.push_back(it->second);
                exited = true;
                break;
            }
            if (in.op == IOp::Jcc) {
                auto it = rg.label_region.find(in.a.label);
                if (it != rg.label_region.end()) s.push_back(it->second);
                if (k + 1 < rg.begin.size()) s.push_back(k + 1);
                exited = true;
                break;
            }
            if (is_terminator(in)) {
                exited = true;
                break;
            }
        }
        if (!exited && k + 1 < rg.begin.size()) s.push_back(k + 1);
    }
}

// ---- liveness (GPRs, flags, slots; backward fixpoint over regions) -------

constexpr u32 kAllFlags = (1u << kNF) - 1;

struct LiveSet {
    u32 gpr = 0;
    u32 flags = 0;
    std::vector<u64> slots; // bitset over absolute slot ids
    bool same_as(const LiveSet& o, u32 words) const {
        if (gpr != o.gpr || flags != o.flags) return false;
        for (u32 w = 0; w < words; ++w)
            if (slots[w] != o.slots[w]) return false;
        return true;
    }
};

void live_transfer_inst(const LiveSet& after, LiveSet& before, const Inst& i,
                        const TableIndex& idx) {
    before = after;
    Effects e;
    if (!row_effects(idx, i, e)) {
        // Unknown opcode: conservative — everything stays live (reads all,
        // kills nothing). The x86-64 table lists every IOp, so this is a
        // defensive fallback, not an expected path.
        before.gpr = (1u << kMaxGpr) - 1;
        before.flags = kAllFlags;
        for (u64& w : before.slots) w = ~0ull;
        return;
    }
    before.gpr = (before.gpr & ~e.gpr_write) | e.gpr_read;
    if (e.writes_flags || e.undef_flags) before.flags &= ~kAllFlags;
    if (e.reads_flags) before.flags |= kAllFlags;
    if (e.slot_write >= 0 && e.slot_write_covers) {
        size_t w = static_cast<size_t>(e.slot_write) / 64;
        if (w < before.slots.size())
            before.slots[w] &= ~(1ull << (static_cast<size_t>(e.slot_write) % 64));
    }
    if (e.slot_read >= 0) {
        size_t w = static_cast<size_t>(e.slot_read) / 64;
        if (w < before.slots.size())
            before.slots[w] |= (1ull << (static_cast<size_t>(e.slot_read) % 64));
    }
}

struct Liveness {
    std::vector<LiveSet> in, out; // per region
};

void compute_liveness(const LFunction& lf, const Regions& rg, const TableIndex& idx,
                      u32 nslots_fn, Liveness& lv) {
    const size_t nr = rg.begin.size();
    const u32 words = (nslots_fn + 63) / 64;
    lv.in.assign(nr, LiveSet{});
    lv.out.assign(nr, LiveSet{});
    for (size_t k = 0; k < nr; ++k) {
        lv.in[k].slots.assign(words, 0);
        lv.out[k].slots.assign(words, 0);
    }
    bool changed = true;
    u32 guard = 0;
    // exact convergence (monotone backward fixpoint); cap = chain length
    // worst case with slack — a truncated fixpoint would UNDER-approximate
    // liveness and weaken the replacement contract (unsound), so the cap
    // is generous, never tight
    while (changed && guard++ < rg.begin.size() + 8) {
        changed = false;
        for (size_t k = nr; k-- > 0;) {
            LiveSet out{};
            out.slots.assign(words, 0);
            for (u32 s : rg.succs[k]) {
                out.gpr |= lv.in[s].gpr;
                out.flags |= lv.in[s].flags;
                for (u32 w = 0; w < words; ++w)
                    out.slots[w] |= lv.in[s].slots[w];
            }
            LiveSet in = out;
            for (u32 i = rg.end[k]; i-- > rg.begin[k];) {
                LiveSet before = in;
                live_transfer_inst(in, before, lf.code[i], idx);
                in = before;
            }
            if (!out.same_as(lv.out[k], words) || !in.same_as(lv.in[k], words))
                changed = true;
            lv.out[k] = std::move(out);
            lv.in[k] = std::move(in);
        }
    }
}

// Live set on entry to instruction position `pos` of region `k`
// (walking the region backward from its EXIT live-set).
LiveSet live_at(const LFunction& lf, const Regions& rg, const TableIndex& idx,
                const Liveness& lv, u32 k, u32 pos) {
    LiveSet live = lv.out[k];
    for (u32 i = rg.end[k]; i-- > pos;) {
        LiveSet before = live;
        live_transfer_inst(live, before, lf.code[i], idx);
        live = before;
    }
    return live;
}

// ---- windows ---------------------------------------------------------------

struct WindowCtx {
    u32 w0 = 0, w1 = 0;
    u32 region = 0;
    i32 slots[kMaxSlot] = {0}; // state slot k = absolute slot id
    u32 nslots = 0;
    u32 livein_gpr = 0;
    u32 livein_slots = 0; // window-slot bits defined at entry
    bool livein_flags = false;
    u32 contract_gpr = 0;
    u32 contract_slots = 0; // window-slot bits that must match at exit
    bool contract_flags = false;
    u32 regpool = 0; // live-in | original writes (operand pool)
    std::vector<i64> immpool;
    i32 orig_cost = 0;
    u32 orig_len = 0;
    std::vector<u32> enum_rows; // searchable rows, in-window rows first
};

bool contract_ok(const VState& a, const VState& b, const WindowCtx& c) {
    for (u32 g = 0; g < kMaxGpr; ++g) {
        if (!(c.contract_gpr & (1u << g))) continue;
        for (u32 v = 0; v < kVecSearch; ++v)
            if (a.r[v][g] != b.r[v][g]) return false;
    }
    for (u32 k = 0; k < c.nslots; ++k) {
        if (!(c.contract_slots & (1u << k))) continue;
        for (u32 v = 0; v < kVecSearch; ++v)
            if (a.s[v][k] != b.s[v][k]) return false;
    }
    if (c.contract_flags) {
        for (u32 v = 0; v < kVecSearch; ++v)
            for (u32 f = 0; f < kNF; ++f)
                if (a.f[v][f] != b.f[v][f]) return false;
    }
    return true;
}

// How many CONTRACT locations already equal the original's final state —
// the greedy goal-distance guide for equal-(cost,len) tie-breaking. Pure
// engine concept: contract + the original's behavior, zero instruction
// knowledge.
u32 contract_matches(const VState& a, const VState& b, const WindowCtx& c) {
    u32 m = 0;
    for (u32 g = 0; g < kMaxGpr; ++g) {
        if (!(c.contract_gpr & (1u << g))) continue;
        bool eq = true;
        for (u32 v = 0; v < kVecSearch && eq; ++v) eq = a.r[v][g] == b.r[v][g];
        if (eq) ++m;
    }
    for (u32 k = 0; k < c.nslots; ++k) {
        if (!(c.contract_slots & (1u << k))) continue;
        bool eq = true;
        for (u32 v = 0; v < kVecSearch && eq; ++v) eq = a.s[v][k] == b.s[v][k];
        if (eq) ++m;
    }
    if (c.contract_flags) {
        bool eq = true;
        for (u32 v = 0; v < kVecSearch && eq; ++v)
            for (u32 f = 0; f < kNF && eq; ++f) eq = a.f[v][f] == b.f[v][f];
        if (eq) ++m;
    }
    return m;
}

// Simulate `n` instructions from the batch seed into `st` (st is the seed
// on entry, the final state on return). Returns false on any fault.
bool simulate(VState& st, const WindowCtx& c, const TableIndex& idx,
              const Inst* prog, u32 n, u32 batch) {
    seed_state(st, batch, c.nslots, c.immpool.data(),
               static_cast<u32>(c.immpool.size()));
    for (u32 k = 0; k < n; ++k) {
        if (static_cast<u32>(prog[k].op) >= idx.by_op.size()) return false;
        const Row* r = idx.by_op[static_cast<size_t>(prog[k].op)];
        if (!r || !r->sim) return false;
        if (!r->sim(st, c.slots, c.nslots, prog[k])) return false;
    }
    return true;
}

} // namespace

Report run(LFunction& lf, const IsaTable& isa, const Opts& o) {
    Report rep;
        // Hot-loop env lookups are hoisted ONCE (getenv is a linear scan of the
    // environment; per-pop calls were measurable at the new rates). Same
    // 0-disables semantics as the pass-layer knobs.
    const bool trace = [] {
        const char* v = std::getenv("JULES_SUPEROPT_TRACE");
        return v && *v != '0';
    }();
    // AUDIT MODE: recompute the full XOR-fold after every in-place
    // simulation and after every restore, and against the replayed
    // state at every pop, and abort on mismatch. This is
    // the differential lock proving the journal's write set (Effects)
    // exactly covers what the sims write — the load-bearing assumption of
    // the in-place restructure. Run the suite once with it on.
    const bool audit = [] {
        const char* v = std::getenv("JULES_SUPEROPT_AUDIT");
        return v && *v != '0';
    }();
    TableIndex idx(isa);
    if (lf.code.empty()) return rep;
    if (static_cast<u32>(lf.slot_count) > o.max_slots_fn) return rep;
    // the program rebuild buffer is a fixed 40 entries (see the pop loop);
    // a policy raise of max_window + max_growth past it would truncate
    // arena walks — refuse instead of corrupting
    if (o.max_window + o.max_growth + 1 > 40) return rep;

    Regions rg;
    build_regions(lf, rg);
    if (rg.begin.size() > o.max_regions_fn) return rep;

    Liveness lv;
    compute_liveness(lf, rg, idx, static_cast<u32>(lf.slot_count), lv);

    // A row qualifies as a window instruction when it is searchable AND it
    // models this inst's size (the row's sizes axis doubles as the scale
    // axis for the lea family, which rides in Inst.size).
    auto searchable = [&](const Inst& i) -> const Row* {
        if (static_cast<u32>(i.op) >= idx.by_op.size()) return nullptr;
        const Row* r = idx.by_op[static_cast<size_t>(i.op)];
        if (!r || !r->sim || !r->valid || !r->valid(i)) return nullptr;
        if (r->nsizes) {
            bool ok = false;
            for (u32 k = 0; k < r->nsizes; ++k)
                if (r->sizes[k] == i.size) ok = true;
            if (!ok) return nullptr;
        }
        return r;
    };

    // ---- collect windows: maximal runs of searchable insts, chunked ----
    struct Job {
        WindowCtx ctx;
        bool ok = false;
    };
    std::vector<Job> jobs;
    std::unordered_set<int> window_ops; // scratch for row ordering

    for (u32 k = 0; k < rg.begin.size(); ++k) {
        u32 i = rg.begin[k];
        while (i < rg.end[k]) {
            const Row* r = searchable(lf.code[i]);
            if (!r) {
                ++i;
                continue;
            }
            u32 j = i;
            while (j < rg.end[k] && searchable(lf.code[j])) ++j;
            for (u32 w0 = i; w0 < j; w0 += o.max_window) {
                u32 w1 = std::min(w0 + o.max_window, j);
                Job job;
                job.ok = true;
                WindowCtx& c = job.ctx;
                c.w0 = w0;
                c.w1 = w1;
                c.region = k;
                c.orig_len = w1 - w0;

                // -- slots: absolute ids -> window indices ---------------
                std::vector<i32> abs_slots;
                auto take_slot = [&](const Operand& op) {
                    if (op.k != Operand::K::Slot) return;
                    for (i32 s : abs_slots)
                        if (s == op.slot) return;
                    abs_slots.push_back(op.slot);
                };
                for (u32 p = w0; p < w1; ++p) {
                    take_slot(lf.code[p].a);
                    take_slot(lf.code[p].b);
                }
                if (abs_slots.size() > o.slot_pool_cap) {
                    job.ok = false;
                } else {
                    for (size_t s = 0; s < abs_slots.size(); ++s) {
                        const bool* wide = lf.slot_wide.find(abs_slots[s]);
                        if (wide && *wide) job.ok = false;
                        c.slots[s] = abs_slots[s];
                    }
                    c.nslots = static_cast<u32>(abs_slots.size());
                }
                if (!job.ok) {
                    jobs.push_back(std::move(job));
                    continue;
                }

                // -- live-in / contract ---------------------------------
                LiveSet li = live_at(lf, rg, idx, lv, k, w0);
                LiveSet lo = live_at(lf, rg, idx, lv, k, w1);
                // Entry flags are UNMODELLABLE (their runtime values are
                // whatever preceding code left; the simulator's seeds are
                // not a sound model), so flags are never available at
                // window entry — for the original AND for candidates. A
                // window that reads flags before writing them is skipped.
                c.livein_flags = false;
                c.livein_gpr = li.gpr;
                c.contract_gpr = lo.gpr;
                c.contract_flags = lo.flags != 0;
                for (u32 s = 0; s < c.nslots; ++s) {
                    u64 bit = 1ull << (c.slots[s] % 64);
                    if (li.slots[static_cast<size_t>(c.slots[s]) / 64] & bit)
                        c.livein_slots |= (1u << s);
                    if (lo.slots[static_cast<size_t>(c.slots[s]) / 64] & bit)
                        c.contract_slots |= (1u << s);
                }

                // -- reg pool + definedness walk + cost + flags check ----
                u32 avail_gpr = c.livein_gpr;
                u32 avail_slots = c.livein_slots;
                bool avail_flags = false; // never (see livein_flags note)
                window_ops.clear();
                bool undef_flags_seen = false;
                i32 cost = 0;
                u32 orig_read = 0, orig_write = 0; // the ORIGINAL's registers
                                                   // — the candidate operand
                                                   // pool (see regpool below)
                for (u32 p = w0; p < w1; ++p) {
                    const Inst& in = lf.code[p];
                    const Row* rr = searchable(in);
                    if (!rr) { // defensive: searchable was true moments ago
                        job.ok = false;
                        break;
                    }
                    Effects e;
                    rr->effects(in, e);
                    if (e.gpr_read & ~avail_gpr) {
                        job.ok = false;
                        ++rep.skipped_dead_def;
                        break;
                    }
                    if (e.slot_read >= 0) {
                        bool found = false;
                        for (u32 s = 0; s < c.nslots; ++s)
                            if (c.slots[s] == e.slot_read && (avail_slots & (1u << s)))
                                found = true;
                        if (!found) {
                            job.ok = false;
                            ++rep.skipped_dead_def;
                            break;
                        }
                    }
                    if (e.reads_flags && !avail_flags) {
                        job.ok = false;
                        ++rep.skipped_flag_in;
                        break;
                    }
                    if (e.undef_flags) undef_flags_seen = true;
                    orig_read |= e.gpr_read;
                    orig_write |= e.gpr_write;
                    avail_gpr |= e.gpr_write;
                    if (e.slot_write >= 0)
                        for (u32 s = 0; s < c.nslots; ++s)
                            if (c.slots[s] == e.slot_write) avail_slots |= (1u << s);
                    if (e.writes_flags || e.undef_flags) avail_flags = true;
                    cost += rr->latency ? rr->latency(in) : 1;
                    window_ops.insert(static_cast<int>(in.op));
                }
                if (!job.ok) {
                    jobs.push_back(std::move(job));
                    continue;
                }
                if (undef_flags_seen && c.contract_flags) {
                    job.ok = false;
                    ++rep.skipped_undef_flags;
                    jobs.push_back(std::move(job));
                    continue;
                }
                c.orig_cost = cost;
                // CANDIDATE OPERAND POLICY: candidates may only use the
                // registers the ORIGINAL window itself used (its read and
                // write set). Soundness is unaffected (the live-out contract
                // still governs the goal test); this is the classic
                // superoptimizer operand reduction — without it, a window
                // adjacent to a call carries the call's whole conservative
                // argument-read set in its live-in, the register pool
                // explodes (Lea2 alone: 8 regs -> 12288 bindings) and the
                // winning candidate sits far beyond every budget.
                c.regpool = orig_read | orig_write;
                u32 rpc = 0;
                for (u32 g = 0; g < kMaxGpr; ++g)
                    if (c.regpool & (1u << g)) ++rpc;
                if (rpc > o.reg_pool_cap) job.ok = false;

                // -- immediate pool: the window's own constants first, then
                //    corner values sampled evenly up to the cap -----------
                if (job.ok) {
                    std::vector<i64> own, rest;
                    own.push_back(0); // the additive identity: lea's "no
                                      // displacement", add's "no change"
                    for (u32 p = w0; p < w1; ++p) {
                        const Inst& in = lf.code[p];
                        if (in.b.k == Operand::K::Imm) own.push_back(in.b.imm);
                        else if (in.b.imm != 0) rest.push_back(in.b.imm); // lea disp
                    }
                    for (i64 cv : kCorners) rest.push_back(cv);
                    for (auto* v : {&own, &rest}) {
                        std::sort(v->begin(), v->end());
                        v->erase(std::unique(v->begin(), v->end()), v->end());
                    }
                    std::vector<i64> imms;
                    for (i64 x : own) {
                        if (imms.size() >= o.imm_pool_cap) break;
                        imms.push_back(x);
                    }
                    for (size_t s = 0;
                         s < rest.size() && imms.size() < o.imm_pool_cap; ++s) {
                        if ((s * rest.size()) / o.imm_pool_cap !=
                            ((s + 1) * rest.size()) / o.imm_pool_cap)
                            imms.push_back(rest[s]);
                    }
                    if (imms.empty()) imms.push_back(0);
                    c.immpool = std::move(imms);
                }

                // -- enumeration order: in-window rows first ---------------
                for (u32 ri = 0; ri < isa.size(); ++ri) {
                    if (!isa[ri].sim) continue;
                    if (window_ops.count(static_cast<int>(isa[ri].op)))
                        c.enum_rows.push_back(ri);
                }
                for (u32 ri = 0; ri < isa.size(); ++ri) {
                    if (!isa[ri].sim) continue;
                    if (!window_ops.count(static_cast<int>(isa[ri].op)))
                        c.enum_rows.push_back(ri);
                }

                jobs.push_back(std::move(job));
            }
            i = j;
        }
    }

    // ---- per-window Dijkstra search ---------------------------------------
    struct Commit {
        u32 w0, w1;
        std::vector<Inst> prog;
    };
    std::vector<Commit> commits;

    struct QEnt {
        i32 cost;
        u16 len;
        u32 seq;
        u32 node;
        u32 avail_gpr;
        u32 avail_slots;
        u8 avail_flags;
        u16 match; // contract locations already equal to the goal
        u64 fp;   // the node's state fingerprint (incremental fold)
    };
    // POP ORDER: goal-distance first (match DESC), then cost, length,
    // insertion. A state whose match equals the full contract IS a goal
    // (match counts exactly the goal-test locations), so goals pop before
    // everything else. Trade-off vs pure Dijkstra (cost-first): the popped
    // winner is strictly better than the ORIGINAL but not necessarily the
    // minimal equivalent — honest weakening for a budget-incomplete search
    // (cost-first left depth-2 goals buried behind hundreds of cost-1
    // states and the pop budget died before reaching them).
    struct QEntGreater {
        bool operator()(const QEnt& a, const QEnt& b) const {
            if (a.match != b.match) return a.match < b.match; // goal first
            if (a.cost != b.cost) return a.cost > b.cost;
            if (a.len != b.len) return a.len > b.len;
            return a.seq > b.seq;
        }
    };
    struct ArenaNode {
        Inst inst;
        u32 parent;
    };
    // STATES ARE NOT STORED per node (2.5KB x every pushed candidate was
    // 300MB+ under large budgets, and the page-fault churn ate 40% of wall
    // time). Instead: the seed is computed ONCE per window; a pop that
    // survives the stale check materializes its state by replaying its
    // <=40-instruction program from the cached seed. Stale pops (the
    // overwhelming majority under lazy Dijkstra) just compare the
    // fingerprint carried by the queue entry — no state, no replay.
    // Self-healing by construction: every expanded state is derived from
    // the seed + true instruction semantics, never from a mutated cache.

    u32 fn_pops_left = o.pops_per_fn;

    // concretized operand pools for the current window
    std::vector<Operand> pool_r, pool_s, pool_i, pool_b;

    for (Job& job : jobs) {
        if (!job.ok) continue;
        const WindowCtx& c = job.ctx;
        if (fn_pops_left == 0) break;

        VState orig_final;
        if (!simulate(orig_final, c, idx, &lf.code[c.w0], c.orig_len, 0)) {
            // the original faults on a sampled lane: leave it alone
            ++rep.skipped_orig_fault;
            continue;
        }
        rep.attempted_cost += c.orig_cost;
        ++rep.windows;

        // The batch-0 seed is a pure function of the window — compute it
        // ONCE (the old code re-derived it per pop: 312 splitmix64 calls).
        // Needed by the cross-probe below, the Dijkstra root, and pop replay.
        VState seed0;
        seed_state(seed0, 0, c.nslots, c.immpool.data(),
                   static_cast<u32>(c.immpool.size()));

        // Tier-3 cross-probe: the SMT encoder must reproduce the simulator's
        // ORIGINAL-window result on the batch-0 concrete inputs, bit for bit,
        // before any of its equivalence verdicts may be trusted this run.
        if (o.tier4_selftest) {
            Tier4Query q;
            q.lf = &lf;
            q.w0 = c.w0;
            q.w1 = c.w1;
            q.seed = &seed0;
            q.orig_final = &orig_final;
            q.livein_gpr = c.livein_gpr;
            q.livein_slots = c.livein_slots;
            q.contract_gpr = c.contract_gpr;
            q.contract_slots = c.contract_slots;
            q.contract_flags = c.contract_flags;
            for (u32 s = 0; s < c.nslots; ++s) q.slots[s] = c.slots[s];
            q.nslots = c.nslots;
            o.tier4_selftest(q); // aborts loudly on any disagreement
            ++rep.smt_selfchecks;
        }

        // -- pools ---------------------------------------------------------
        pool_r.clear();
        pool_s.clear();
        pool_i.clear();
        pool_b.clear();
        for (u32 g = 0; g < kMaxGpr; ++g) {
            if (c.regpool & (1u << g)) {
                Operand op;
                op.k = Operand::K::Reg;
                op.reg = static_cast<R>(g);
                pool_r.push_back(op);
            }
        }
        for (u32 s = 0; s < c.nslots; ++s) {
            Operand op;
            op.k = Operand::K::Slot;
            op.slot = c.slots[s];
            pool_s.push_back(op);
        }
        for (i64 iv : c.immpool) {
            Operand op;
            op.k = Operand::K::Imm;
            op.imm = iv;
            pool_i.push_back(op);
        }
        pool_b = pool_r;
        pool_b.insert(pool_b.end(), pool_s.begin(), pool_s.end());

        // -- Dijkstra ------------------------------------------------------
        std::vector<ArenaNode> arena; // node 0 = root sentinel
        arena.push_back(ArenaNode{Inst{}, 0});
        // Lazy Dijkstra: a state may be reached by several programs of
        // different cost (e.g. `imul $5` and `lea [rbx+rbx*4]` converge).
        // Dedup-at-push with a plain set keeps whichever program was
        // CONSTRUCTED FIRST — the expensive one blocks the cheap one
        // forever. The correct structure: keep the best known cost per
        // state, push improvements, skip stale entries at pop time.
        FlatMap best;
        best.init(o.states_per_window + 8);
        FlatSet expanded;
        // pops <= pops_per_window (the while-condition budget); improvements
        // can out-pop it, so clamp — a full set claims "fresh" (extra work,
        // still sound: dedup is a budget device, equivalence is tested
        // exactly).
        expanded.init(std::min<size_t>(o.pops_per_window, 500000u) + 8);
        std::priority_queue<QEnt, std::vector<QEnt>, QEntGreater> pq;
        u32 seq = 0;
        u32 pops = 0;
        bool state_cap_hit = false;

        const u64 root_fp = fp_full(seed0, c.nslots);
        best.improve(root_fp, 0);
        QEnt root{0, 0, seq++, 0, c.livein_gpr, c.livein_slots, 0,
                  static_cast<u16>(contract_matches(seed0, orig_final, c)),
                  root_fp};
        pq.push(root);

        std::vector<Inst> winner;
        bool have_winner = false;
        u32 cands = 0; // constructed candidates this window
        bool cand_cap = false; // candidate budget consumed: expansion stops,
                               // but the queue keeps draining (a goal state
                               // already pushed must still be popped)

        while (!pq.empty() && pops < o.pops_per_window && fn_pops_left > 0 &&
               !state_cap_hit) {
            QEnt e = pq.top();
            pq.pop();
            ++pops;
            --fn_pops_left;
            rep.pops += 1;

            if (!expanded.insert(e.fp)) continue; // stale queue entry

            // Materialize the popped state: replay this node's program from
            // the cached seed (self-healing: derived from true semantics,
            // never from a mutated cache). Only non-stale pops pay this.
            Inst prog[40];
            u32 n = 0;
            for (u32 node = e.node; node != 0 && n < 40;
                 node = arena[node].parent)
                prog[n++] = arena[node].inst;
            if (n > 1) // (n == 0 is the root: the empty program)
                for (u32 a = 0, b = n - 1; a < b; ++a, --b)
                    std::swap(prog[a], prog[b]);

            VState st = seed0;
            bool replay_ok = true;
            for (u32 k = 0; k < n; ++k) {
                if (static_cast<u32>(prog[k].op) >= idx.by_op.size() ||
                    !idx.by_op[static_cast<size_t>(prog[k].op)] ||
                    !idx.by_op[static_cast<size_t>(prog[k].op)]->sim(
                        st, c.slots, c.nslots, prog[k])) {
                    replay_ok = false; // cannot happen (the construction
                                        // sim succeeded for this program)
                    break;
                }
            }
            if (!replay_ok) continue;
            if (audit && fp_full(st, c.nslots) != e.fp) {
                std::fprintf(stderr,
                             "[so-audit] POP CHAIN MISMATCH: the incremental "
                             "fingerprint disagrees with the replayed state\n");
                std::abort();
            }

            if (trace && e.len <= 2)
                std::fprintf(stderr,
                             "[so-trace] pop len=%u cost=%d match=%u cands=%u "
                             "states=%zu pq=%zu op=%d\n",
                             e.len, e.cost, e.match, cands, best.count,
                             pq.size(),
                             n ? static_cast<int>(prog[n - 1].op) : -1);
            if (contract_ok(st, orig_final, c)) {
                // first goal pop under match-first ordering (see above)
                if (e.cost < c.orig_cost ||
                    (e.cost == c.orig_cost && e.len < c.orig_len)) {
                    winner.assign(prog, prog + n);
                    have_winner = true;
                }
                break;
            }

            if (cand_cap) continue; // budget consumed: drain the queue only
            u32 made_this_pop = 0; // per-pop construction budget

            // ---- expand children ----------------------------------------
            // EACH POP enumerates the full candidate space from flat 0
            // (resumable per-row cursors turned out structurally wrong:
            // every state after the root would append to a >= 1-instruction
            // program, so pure depth-1 candidates in late rows were never
            // constructable at all). Repeats across pops are absorbed by
            // the state dedup; two budgets bound the work:
            //   candidates_per_pop     — constructions per queue pop
            //   candidates_per_window  — constructions per window (cap)
            for (u32 ri_pos = 0; ri_pos < c.enum_rows.size(); ++ri_pos) {
                const Row& row = isa[c.enum_rows[ri_pos]];
                const u32 nb = row.nbins ? row.nbins : 1;
                const u32 nc = row.nconds ? row.nconds : 1;
                const u32 ns = row.nsizes ? row.nsizes : 1;

                const std::vector<Operand>* pools[4] = {};
                u32 nbinds = 1;
                bool pool_empty = false;
                for (u32 d = 0; d < row.nops; ++d) {
                    switch (row.ops[d].pool) {
                        case 'R': pools[d] = &pool_r; break;
                        case 'S': pools[d] = &pool_s; break;
                        case 'I': pools[d] = &pool_i; break;
                        case 'B': pools[d] = &pool_b; break;
                        default: break;
                    }
                }
                for (u32 d = 0; d < row.nops; ++d) {
                    if (!pools[d]) continue;
                    if (pools[d]->empty()) { pool_empty = true; break; }
                    nbinds *= static_cast<u32>(pools[d]->size());
                }
                if (pool_empty) continue;
                const u32 row_total = nb * nc * ns * nbinds;

                for (u32 flat = 0; flat < row_total &&
                                   made_this_pop < o.candidates_per_pop &&
                                   !state_cap_hit && !cand_cap;
                     ++flat) {
                    // flat = ((bi*nc + ci)*ns + si)*nbinds + b — decode
                    // from the LOW digits up: binding first, then the
                    // variant axes
                    u32 bidx = flat % nbinds;
                    u32 rem = flat / nbinds;
                    u32 si = rem % ns; rem /= ns;
                    u32 ci = rem % nc; rem /= nc;
                    u32 bi = rem;

                    Inst tin{};
                    tin.op = row.op;
                    tin.bin = row.nbins ? row.bins[bi] : BinOp::Add;
                    tin.sar = row.nbins ? row.sars[bi] : false;
                    tin.cond = row.nconds ? row.conds[ci] : Cond::E;
                    tin.size = row.nsizes ? row.sizes[si] : 8;

                    // bind operands: mixed-radix decode of the binding index
                    for (u32 d = row.nops; d-- > 0;) {
                        if (!pools[d]) continue;
                        u32 sz = static_cast<u32>(pools[d]->size());
                        u32 di = bidx % sz;
                        bidx /= sz;
                        const Operand& v = (*pools[d])[di];
                        switch (row.ops[d].field) {
                            case 0: tin.a = v; break; // operand a (standalone)
                            case 1: // operand into b — KIND-AWARE narrow
                                // write: copy only the payload the pool
                                // operand's kind actually uses. A whole-
                                // operand assignment wipes the lea forms'
                                // parallel fields (disp in b.imm, SIB index
                                // in b.slot — the bug that silently zeroed
                                // every displacement and forced every Lea2
                                // index to rax); a forced-Reg write breaks
                                // the imm/slot operands (MovRImm/MovSR).
                                switch (v.k) {
                                    case Operand::K::Imm:
                                        tin.b.k = Operand::K::Imm;
                                        tin.b.imm = v.imm;
                                        break;
                                    case Operand::K::Slot:
                                        tin.b.k = Operand::K::Slot;
                                        tin.b.slot = v.slot;
                                        break;
                                    default:
                                        tin.b.k = Operand::K::Reg;
                                        tin.b.reg = v.reg;
                                        break;
                                }
                                break;
                            case 2: // Lea2's SIB index rides in b.slot
                                tin.b.slot = static_cast<i32>(v.reg);
                                break;
                            case 3: tin.b.imm = v.imm; break; // lea disp
                            default: break;
                        }
                    }
                    ++made_this_pop;
                    ++rep.constructions;
                    if (cands++ >= o.candidates_per_window) {
                        cand_cap = true;
                        break;
                    }

                    if (!row.valid || !row.valid(tin)) continue;
                    Effects ef;
                    row.effects(tin, ef);
                    bool dom_ok = (ef.gpr_read & ~e.avail_gpr) == 0 &&
                                  (!ef.reads_flags || e.avail_flags);
                    if (dom_ok && ef.slot_read >= 0) {
                        bool f = false;
                        for (u32 s = 0; s < c.nslots; ++s)
                            if (c.slots[s] == ef.slot_read &&
                                (e.avail_slots & (1u << s)))
                                f = true;
                        dom_ok = f;
                    }
                    if (!dom_ok) continue;

                    const i32 lat = row.latency ? row.latency(tin) : 1;
                    const i32 ncost = e.cost + lat;
                    const u32 nlen = e.len + 1;
                    if (ncost > c.orig_cost || nlen > c.orig_len + o.max_growth)
                        continue;

                    // -- in-place extension (journal) ------------------------
                    // The child state IS the parent state plus one write set;
                    // the 2.5KB full-state copy per construction is gone.
                    // Save the write set, simulate in place, fold the
                    // fingerprint delta over the SAME positions, then
                    // restore — win or lose, the parent ends pristine.
                    Journal jr;
                    journal_save(jr, st, ef, c.slots, c.nslots);
                    ++rep.simulations;
                    if (row.sim(st, c.slots, c.nslots, tin)) {
                        const u64 nfp = e.fp ^ journal_fp_delta(st, jr, c.nslots);
                        if (audit && fp_full(st, c.nslots) != nfp) {
                            std::fprintf(stderr,
                                         "[so-audit] FOLD MISMATCH after sim: "
                                         "op=%d (journal missed a write)\n",
                                         static_cast<int>(tin.op));
                            std::abort();
                        }
                        if (best.improve(nfp, ncost)) {
                            if (best.count > o.states_per_window) {
                                state_cap_hit = true;
                                journal_restore(st, jr);
                                if (audit && fp_full(st, c.nslots) != e.fp) {
                                    std::fprintf(stderr,
                                                 "[so-audit] RESTORE MISMATCH "
                                                 "(state cap): op=%d "
                                                 "(parent state corrupted)\n",
                                                 static_cast<int>(tin.op));
                                    std::abort();
                                }
                                break;
                            }
                            const u16 q_match = static_cast<u16>(
                                contract_matches(st, orig_final, c));
                            arena.push_back(ArenaNode{tin, e.node});
                            QEnt q{ncost, static_cast<u16>(nlen), seq++,
                                   static_cast<u32>(arena.size() - 1),
                                   e.avail_gpr | ef.gpr_write, e.avail_slots,
                                   (e.avail_flags || ef.writes_flags ||
                                    ef.undef_flags)
                                       ? u8(1)
                                       : u8(0),
                                   q_match, nfp};
                            if (ef.slot_write >= 0)
                                for (u32 s = 0; s < c.nslots; ++s)
                                    if (c.slots[s] == ef.slot_write)
                                        q.avail_slots |= (1u << s);
                            pq.push(q);
                        }
                    }
                    journal_restore(st, jr);
                    if (audit && fp_full(st, c.nslots) != e.fp) {
                        std::fprintf(stderr,
                                     "[so-audit] RESTORE MISMATCH: op=%d "
                                     "(parent state corrupted)\n",
                                     static_cast<int>(tin.op));
                        std::abort();
                    }
                }
                if (state_cap_hit || cand_cap) break;
            }
        }

        if (have_winner) {
            // commit gate: re-verify on FRESH batches (batch 0 is the
            // search's own directed batch — re-running it proves nothing)
            bool ok = true;
            for (u32 batch = 1; batch <= kVerifyBatches && ok; ++batch) {
                VState o2, c2;
                if (!simulate(o2, c, idx, &lf.code[c.w0], c.orig_len, batch)) {
                    ok = false; // original faults on this lane set: never
                                // certify the window at all
                    break;
                }
                if (!simulate(c2, c, idx, winner.data(),
                              static_cast<u32>(winner.size()), batch)) {
                    ok = false;
                    break;
                }
                if (!contract_ok(c2, o2, c)) ok = false;
            }
            if (ok) {
                // Tier 4 — the SMT equivalence proof (Z3). Refuted winners
                // are rejected HERE, after passing all the sampled lanes:
                // this is the tier that catches what sampling cannot see
                // (unsampled fault inputs, poison-flag coincidences).
                if (o.tier4) {
                    Tier4Query q;
                    q.lf = &lf;
                    q.w0 = c.w0;
                    q.w1 = c.w1;
                    q.cand = winner.data();
                    q.ncand = static_cast<u32>(winner.size());
                    q.seed = &seed0;
                    q.orig_final = &orig_final;
                    q.livein_gpr = c.livein_gpr;
                    q.livein_slots = c.livein_slots;
                    q.contract_gpr = c.contract_gpr;
                    q.contract_slots = c.contract_slots;
                    q.contract_flags = c.contract_flags;
                    for (u32 s = 0; s < c.nslots; ++s) q.slots[s] = c.slots[s];
                    q.nslots = c.nslots;
                    const Verdict v = o.tier4(q);
                    if (v == Verdict::Refuted) {
                        ok = false;
                        ++rep.tier4_refuted;
                    } else if (v == Verdict::Proven) {
                        ++rep.tier4_proven;
                    } else {
                        ++rep.tier4_unknown;
                        if (o.tier4_required) ok = false;
                    }
                }
            }
            if (ok) {
                if (winner.empty()) ++rep.erased;
                else ++rep.improved;
                i32 after = 0;
                for (const Inst& in : winner)
                    after += idx.by_op[static_cast<size_t>(in.op)]
                                ? (idx.by_op[static_cast<size_t>(in.op)]->latency
                                       ? idx.by_op[static_cast<size_t>(in.op)]->latency(in)
                                       : 1)
                                : 1;
                rep.committed_before += c.orig_cost;
                rep.committed_after += after;
                if (trace)
                    std::fprintf(stderr,
                                 "[so-commit] win [%u,%u) orig_cost=%d -> %d "
                                 "len=%zu contract gpr=%x slots=%x flags=%d\n",
                                 c.w0, c.w1, c.orig_cost, after, winner.size(),
                                 c.contract_gpr, c.contract_slots,
                                 c.contract_flags ? 1 : 0);
                commits.push_back(Commit{c.w0, c.w1, std::move(winner)});
            }
        } else if (pops >= o.pops_per_window || state_cap_hit || cand_cap) {
            ++rep.budget_exhausted;
        }
        if (trace)
            std::fprintf(stderr,
                         "[so-trace] window [%u,%u) done: winner=%d pops=%u "
                         "cands=%u states=%zu pq=%zu cap=%d orig_cost=%d\n",
                         c.w0, c.w1, have_winner ? 1 : 0, pops, cands,
                         best.count, pq.size(), state_cap_hit ? 1 : 0,
                         c.orig_cost);
    }

    // ---- apply commits back-to-front ---------------------------------------
    for (auto it = commits.rbegin(); it != commits.rend(); ++it) {
        lf.code.erase(lf.code.begin() + it->w0, lf.code.begin() + it->w1);
        lf.code.insert(lf.code.begin() + it->w0, it->prog.begin(), it->prog.end());
    }

    return rep;
}

} // namespace jules::superopt
