// Offline validator: ralloc_flow_select vs exhaustive search.
//
// Random instances (and adversarial hand cases) of weighted span selection
// with per-segment capacities; brute force enumerates every subset, keeps
// the feasible ones (overlap <= cap everywhere), and compares the maximum
// weight against the flow's selection. Also asserts the flow's own
// selection is feasible.
//
// Build: g++ -std=c++20 -O2 -Isrc scripts/ra_flow_selftest.cpp src/core/codegen/ralloc_flow.cpp -o /tmp/ra_flow_selftest
#include "core/codegen/ralloc.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace jules;
using namespace jules::ralloc;

static u64 instance_weight(const std::vector<RaSpan>& spans,
                           const std::vector<i32>& ids) {
    u64 w = 0;
    for (i32 id : ids)
        for (const RaSpan& s : spans)
            if (s.vreg == id) w += s.weight;
    return w;
}

static bool feasible(const std::vector<RaSpan>& spans, const std::vector<i32>& ids,
                     const std::vector<u32>& caps) {
    std::vector<u32> occ(caps.size(), 0);
    for (i32 id : ids)
        for (const RaSpan& s : spans)
            if (s.vreg == id)
                for (u32 p = s.start; p < s.end && p < caps.size(); ++p) occ[p]++;
    for (size_t p = 0; p < caps.size(); ++p)
        if (occ[p] > caps[p]) return false;
    return true;
}

static u64 brute(const std::vector<RaSpan>& spans, const std::vector<u32>& caps) {
    const size_t n = spans.size();
    u64 best = 0;
    for (size_t mask = 0; mask < (size_t(1) << n); ++mask) {
        std::vector<i32> ids;
        u64 w = 0;
        for (size_t i = 0; i < n; ++i)
            if (mask & (size_t(1) << i)) {
                ids.push_back(spans[i].vreg);
                w += spans[i].weight;
            }
        if (feasible(spans, ids, caps) && w > best) best = w;
    }
    return best;
}

int main() {
    std::mt19937 rng(0xC0FFEE);

    struct Case { std::vector<RaSpan> spans; std::vector<u32> caps; };
    std::vector<Case> cases;

    // adversarial hand cases -------------------------------------------
    // the swap case greedy-by-weight misses: C alone (9) < A+B (10)
    {
        Case c;
        c.caps = {1, 1};
        c.spans = {{0, 1, 1, 5}, {1, 2, 2, 5}, {0, 2, 3, 9}};
        cases.push_back(c);
    }
    // chained reuse: [0,2)+[1,3) beats the heavy [0,3) at K=1... (10 vs 9)
    {
        Case c;
        c.caps = {1, 1, 1};
        c.spans = {{0, 2, 1, 10}, {0, 1, 2, 6}, {1, 2, 3, 6}, {2, 3, 4, 6}};
        cases.push_back(c);
    }
    // capacity 0 must select nothing overlapping it
    {
        Case c;
        c.caps = {0, 2};
        c.spans = {{0, 2, 1, 100}, {1, 2, 2, 1}};
        cases.push_back(c);
    }

    // random cases ------------------------------------------------------
    for (int t = 0; t < 20000; ++t) {
        Case c;
        size_t S = 1 + rng() % 8;
        c.caps.resize(S);
        for (size_t i = 0; i < S; ++i) c.caps[i] = rng() % 4;
        size_t n = 1 + rng() % 10;
        for (size_t i = 0; i < n; ++i) {
            RaSpan s;
            s.start = rng() % S;
            s.end = 1 + s.start + rng() % (S - s.start);
            s.vreg = static_cast<i32>(i);
            s.weight = 1 + rng() % 50;
            c.spans.push_back(s);
        }
        cases.push_back(c);
    }

    u64 checked = 0, bad = 0;
    for (const Case& c : cases) {
        std::vector<i32> sel = ralloc_flow_select(c.spans, c.caps);
        u64 flow_w = instance_weight(c.spans, sel);
        u64 opt_w = brute(c.spans, c.caps);
        bool sel_ok = feasible(c.spans, sel, c.caps);
        ++checked;
        if (!sel_ok || flow_w != opt_w) {
            ++bad;
            std::printf("MISMATCH feas=%d flow=%llu opt=%llu caps=[", (int)sel_ok,
                        (unsigned long long)flow_w, (unsigned long long)opt_w);
            for (u32 cp : c.caps) std::printf("%u,", cp);
            std::printf("] spans:");
            for (const RaSpan& s : c.spans)
                std::printf(" [%u,%u)#%d/%u", s.start, s.end, s.vreg, s.weight);
            std::printf("\n");
            if (bad > 5) break;
        }
    }
    std::printf("%llu instances: %llu mismatches\n", (unsigned long long)checked,
                (unsigned long long)bad);
    return bad ? 1 : 0;
}
