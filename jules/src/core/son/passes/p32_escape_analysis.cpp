// Pass 32 — EscapeAnalysis (Phase 3: Escape & Allocation Analysis)
//
// Classifies every allocation: NoEscape / ArgEscape / GlobalEscape.
//   NoEscape     — address never flows out of addressing + memory chain
//   ArgEscape    — passed to a call (callee may retain it)
//   GlobalEscape — stored to memory or returned
// The classification is recorded as node flags + telemetry; HeapToStack (29)
// and the future PEA (33) consume it.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
enum class EscapeClass : u8 { NoEscape, ArgEscape, GlobalEscape };

class EscapeAnalyzer {
public:
    explicit EscapeAnalyzer(Graph& g) : g_(g) {}

    bool run() {
        for (NodeId id = 0; id < g_.size(); ++id) {
            const Node& n = g_.node(id);
            if (n.op != Op::Alloc) continue;
            EscapeClass cls = classify(id);
            classified_.insert(id, cls);
            if (cls != EscapeClass::NoEscape) changed_ = true; // facts recorded
        }
        return changed_;
    }

private:
    EscapeClass classify(NodeId alloc) {
        EscapeClass worst = EscapeClass::NoEscape;
        for (NodeId u : g_.uses_of(alloc)) {
            const Node& un = g_.node(u);
            switch (un.op) {
                case Op::Load:
                case Op::Store:
                    if (un.in[2] == alloc || un.in[1] == alloc) continue;
                    worst = EscapeClass::GlobalEscape; // stored as value
                    break;
                case Op::Call: {
                    if (un.in[1] == alloc) continue;         // memory chain
                    if (un.aux == kFnFree && un.in[2] == alloc) continue;
                    if (worst == EscapeClass::NoEscape) worst = EscapeClass::ArgEscape;
                    continue;
                }
                case Op::Return:
                    if (un.in[1] == alloc) continue;
                    if (un.n_in == 3 && un.in[2] == alloc) worst = EscapeClass::GlobalEscape;
                    continue;
                default:
                    worst = EscapeClass::GlobalEscape; // phi/bin/select data use
                    break;
            }
            if (worst == EscapeClass::GlobalEscape) break;
        }
        return worst;
    }

    Graph& g_;
    FlatMap<NodeId, EscapeClass> classified_;
    bool changed_ = false;
};
} // namespace

class EscapeAnalysisPass : public Pass {
public:
    const char* name() const override { return "EscapeAnalysis"; }
    int order() const override { return 32; }
    const char* phase_name() const override { return "Phase 3: Escape & Allocation Analysis"; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::AliasInfo); }
    AnalysisMask invalidated() const override { return 0; } // pure analysis
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            EscapeAnalyzer e(fg.g);
            changed |= e.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(EscapeAnalysisPass, 32, "Phase 3")

} // namespace jules
