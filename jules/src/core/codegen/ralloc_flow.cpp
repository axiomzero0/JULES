// Phase 1 — exact min-cost live-range selection (network flow).
//
// Problem: given weighted spans on a segment chain and a per-segment
// register capacity, select the maximum-weight subset whose pointwise
// overlap respects the capacity. This decides, globally and exactly, which
// live ranges keep registers and which are spilled — the minimum-cost
// spill set for the machine model.
//
// Why flow: the constraint system (one inequality per segment, variables
// with consecutive-ones support) is an interval program — totally
// unimodular. Its differenced form is a node-arc incidence matrix, so the
// integral optimum is a min-cost flow:
//
//   nodes   = segment boundaries (compressed: span endpoints + capacity
//             changes; interior slack flow is provably constant there)
//   slack   = arc (p+1 -> p), bounds [0, cap[p]]     [register idle]
//   span v  = arc (end_v -> start_v), bounds [0, 1],
//             cost -weight_v                        [register busy]
//   supply  = b(p) = cap[p] - cap[p-1] (caps outside the chain are 0)
//
// Solved as min-cost max-flow from a super source (supplies) to a super
// sink with successive shortest paths. The initial network is acyclic
// (every chain and span arc runs right-to-left, super arcs run S+ -> node
// -> S-), so shortest paths are well-defined; augmenting along shortest
// paths preserves the no-negative-residual-cycle invariant, and integral
// capacities make the optimum integral. Every max flow of the supply
// value corresponds to a feasible selection and vice versa, with cost
// -sum(selected weights): the min-cost flow IS the optimum selection.
#include "core/codegen/ralloc.h"

#include <algorithm>
#include <cstdint>
#include <queue>
#include <vector>

namespace jules::ralloc {

namespace {

struct FlowNet {
    struct Arc {
        int to = 0;
        i64 cap = 0;   // residual capacity
        i64 cost = 0;  // per-unit cost
    };
    int n = 0;
    std::vector<Arc> arcs;              // paired: arcs[i^1] is the reverse
    std::vector<std::vector<int>> adj;  // node -> arc ids

    explicit FlowNet(int nodes) : n(nodes), adj(static_cast<size_t>(nodes)) {}

    int add_arc(int u, int v, i64 cap, i64 cost) {
        int id = static_cast<int>(arcs.size());
        arcs.push_back(Arc{v, cap, cost});
        arcs.push_back(Arc{u, 0, -cost});
        adj[static_cast<size_t>(u)].push_back(id);
        adj[static_cast<size_t>(v)].push_back(id + 1);
        return id;
    }
};

// Shortest augmenting path (SPFA — arcs may carry negative costs, but the
// no-negative-cycle invariant holds along the SSP sequence). Returns the
// augmenting path as arc ids, or empty when the sink is unreachable.
std::vector<int> spfa_path(const FlowNet& net, int src, int dst) {
    const size_t n = static_cast<size_t>(net.n);
    std::vector<i64> dist(n, INT64_MAX / 4);
    std::vector<int> parent_arc(n, -1);
    std::vector<bool> in_queue(n, false);
    std::queue<int> q;
    dist[static_cast<size_t>(src)] = 0;
    q.push(src);
    in_queue[static_cast<size_t>(src)] = true;
    while (!q.empty()) {
        int u = q.front();
        q.pop();
        in_queue[static_cast<size_t>(u)] = false;
        const i64 du = dist[static_cast<size_t>(u)];
        for (int id : net.adj[static_cast<size_t>(u)]) {
            const FlowNet::Arc& a = net.arcs[static_cast<size_t>(id)];
            if (a.cap <= 0) continue;
            const i64 nd = du + a.cost;
            if (nd < dist[static_cast<size_t>(a.to)]) {
                dist[static_cast<size_t>(a.to)] = nd;
                parent_arc[static_cast<size_t>(a.to)] = id;
                if (!in_queue[static_cast<size_t>(a.to)]) {
                    q.push(a.to);
                    in_queue[static_cast<size_t>(a.to)] = true;
                }
            }
        }
    }
    if (parent_arc[static_cast<size_t>(dst)] < 0) return {};
    std::vector<int> path;
    for (int v = dst; v != src;) {
        int id = parent_arc[static_cast<size_t>(v)];
        path.push_back(id);
        v = net.arcs[static_cast<size_t>(id ^ 1)].to; // reverse arc's head = tail
    }
    std::reverse(path.begin(), path.end());
    return path;
}

} // namespace

std::vector<i32> ralloc_flow_select(const std::vector<RaSpan>& spans,
                                    const std::vector<u32>& caps) {
    std::vector<i32> selected;
    if (spans.empty() || caps.empty()) return selected;

    // --- compress the segment chain -----------------------------------
    // Nodes exist only where the flow can turn: span endpoints and
    // capacity changes. Between two consecutive boundaries no span starts
    // or ends and the capacity is constant, so the slack flow is constant
    // there (conservation with zero supply) and the merged slack arc is
    // exact with the minimum capacity on the interval.
    const u32 S = static_cast<u32>(caps.size());
    std::vector<u32> bounds;
    bounds.push_back(0);
    bounds.push_back(S);
    for (const RaSpan& sp : spans) {
        u32 s = sp.start, e = sp.end;
        if (e <= s) continue; // degenerate: no occupancy
        if (s >= S) continue;
        if (e > S) e = S;
        bounds.push_back(s);
        bounds.push_back(e);
    }
    for (u32 p = 0; p < S; ++p)
        if (caps[p] != (p ? caps[p - 1] : 0u)) bounds.push_back(p);
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

    // node_of[p] = compressed node index of boundary p (-1: not a node)
    std::vector<int> node_of(S + 1, -1);
    for (size_t i = 0; i < bounds.size(); ++i)
        node_of[bounds[i]] = static_cast<int>(i);
    // segment capacity between consecutive boundaries
    auto seg_cap = [&](size_t i) {
        u32 lo = bounds[i], hi = bounds[i + 1];
        u32 c = caps[lo];
        for (u32 p = lo + 1; p < hi; ++p) c = std::min(c, caps[p]);
        return c;
    };

    const int nn = static_cast<int>(bounds.size());
    FlowNet net(nn + 2);
    const int SRC = nn, SNK = nn + 1;

    // slack arcs: (node(p+1) -> node(p)) for consecutive boundaries
    for (int i = 0; i + 1 < nn; ++i)
        net.add_arc(i + 1, i, static_cast<i64>(seg_cap(static_cast<size_t>(i))), 0);

    // span arcs: (node(end) -> node(start)), capacity 1, cost -weight.
    // Spans whose endpoints are not compressed boundaries cannot exist
    // (their endpoints were added to the boundary set).
    std::vector<int> span_arc;
    span_arc.reserve(spans.size());
    std::vector<const RaSpan*> kept;
    for (const RaSpan& sp : spans) {
        if (sp.end <= sp.start || sp.start >= S) continue;
        u32 e = std::min(sp.end, S);
        int ns = node_of[sp.start];
        int ne = node_of[e];
        if (ns < 0 || ne < 0) continue; // defensive: out of model
        span_arc.push_back(net.add_arc(ne, ns, 1, -static_cast<i64>(sp.weight)));
        kept.push_back(&sp);
    }

    // supplies: every chain/span arc runs right-to-left (higher node index
    // to lower), so a node ABSORBS flow when capacity grows to its right.
    // Source supply of the node at boundary q is cap[q-1] - cap[q]:
    //   node 0: -cap[0] (sink), node S: +cap[S-1] (source).
    // (Verified by hand: a supply unit entering at the right travels
    // left through slack arcs (register idle) and span arcs (register
    // busy, profit charged); the shortest such path chains spans the way
    // an optimal selection chains reusable registers.)
    std::vector<i64> supply(nn, 0);
    {
        auto supply_at = [&](u32 q) -> i64 {
            if (q == 0) return -static_cast<i64>(caps[0]);
            if (q >= S) return static_cast<i64>(caps[S - 1]);
            return static_cast<i64>(caps[q - 1]) - static_cast<i64>(caps[q]);
        };
        for (int i = 0; i < nn; ++i)
            supply[static_cast<size_t>(i)] = supply_at(bounds[static_cast<size_t>(i)]);
    }
    for (int i = 0; i < nn; ++i) {
        if (supply[static_cast<size_t>(i)] > 0)
            net.add_arc(SRC, i, supply[static_cast<size_t>(i)], 0);
        else if (supply[static_cast<size_t>(i)] < 0)
            net.add_arc(i, SNK, -supply[static_cast<size_t>(i)], 0);
    }

    // --- successive shortest paths to max flow -------------------------
    for (;;) {
        std::vector<int> path = spfa_path(net, SRC, SNK);
        if (path.empty()) break;
        i64 bottleneck = INT64_MAX;
        for (int id : path)
            bottleneck = std::min(bottleneck, net.arcs[static_cast<size_t>(id)].cap);
        for (int id : path) {
            net.arcs[static_cast<size_t>(id)].cap -= bottleneck;
            net.arcs[static_cast<size_t>(id ^ 1)].cap += bottleneck;
        }
    }

    // --- extract the selection -----------------------------------------
    for (size_t k = 0; k < kept.size(); ++k) {
        // selected <=> the span arc carries flow <=> its reverse has residual
        const FlowNet::Arc& rev =
            net.arcs[static_cast<size_t>(span_arc[k] ^ 1)];
        if (rev.cap > 0) selected.push_back(kept[k]->vreg);
    }
    std::sort(selected.begin(), selected.end());
    return selected;
}

} // namespace jules::ralloc
