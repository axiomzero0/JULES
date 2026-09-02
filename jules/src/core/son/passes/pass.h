// Pass infrastructure: contracts, registry, modes, kill switches, telemetry.
//
// Every pass lives in its own translation unit (one file per catalog entry,
// 89 files under son/passes/ + machine passes under codegen/passes/).
// Registration happens through JULES_REGISTER_PASS; the scheduler orders by
// catalog number and honors:
//   * mode masks (AOT / JIT_BASELINE / JIT_OPTIMIZING)
//   * per-pass kill switches (--disable Name) and --only lists
//   * analysis invalidation contracts
//   * per-pass stats (time, node deltas, change counts) for --stats
#pragma once

#include "core/diagnostics/diag.h"
#include "core/son/analysis/analysis.h"
#include "core/son/passes/opt_levels.h"

namespace jules {

struct LinearModule; // codegen/linear.h (fwd decl to avoid a dependency edge)

enum class CompileMode : u8 { AOT, JitBaseline, JitOptimizing };
using ModeMask = u8;
inline constexpr ModeMask kModeAOT          = 1u << 0;
inline constexpr ModeMask kModeJitBaseline  = 1u << 1;
inline constexpr ModeMask kModeJitOptimizing = 1u << 2;
inline constexpr ModeMask kModeAll = kModeAOT | kModeJitBaseline | kModeJitOptimizing;

enum class Stage : u8 { Son, Linear };

struct PassOptions {
    CompileMode mode = CompileMode::AOT;
    OptLevel level = OptLevel::O2;      // default release preset (spec §6)
    OptLevel requested_level = OptLevel::O2; // pre-JIT-budget cap (reporting)
    FpMode fp = FpMode::Strict;         // --fp=strict|fast
    PgoMode pgo = PgoMode::Off;         // --pgo=...
    LtoMode lto = LtoMode::Full;        // single-module compiler: whole-program
    JitBudget jit_budget = JitBudget::Balanced; // --jit-budget (JIT modes)
    bool verify_each = false;   // run the graph verifier after every pass
    bool emit_ir = false;       // dump IR after every pass
    bool post_inline_cleanup = true;
    FlatMap<std::string, bool> disabled; // kill switches (name -> true)
    std::vector<std::string> only;       // if non-empty: run only these
};

struct PassStats {
    const char* name = "";
    int order = 0;
    bool ran = false;
    const char* skip_reason = ""; // "" | "disabled" | "mode" | "stage"
    u64 changes = 0;
    u32 nodes_before = 0, nodes_after = 0;
    double ms = 0.0;
};

class PassContext {
public:
    PassContext(Module& m, SymbolTable& s, Diagnostics& d, AnalysisManager& am,
                PassOptions& o)
        : mod(m), syms(s), diag(d), analysis(am), opts(o) {}

    Module& mod;
    SymbolTable& syms;
    Diagnostics& diag;
    AnalysisManager& analysis;
    PassOptions& opts;
    LinearModule* lin = nullptr; // non-null during Stage::Linear
};

class Pass {
public:
    virtual ~Pass() = default;
    virtual const char* name() const = 0;
    virtual int order() const = 0;            // 1..89 catalog number
    virtual const char* phase_name() const = 0;
    virtual Stage stage() const { return Stage::Son; }
    virtual AnalysisMask required() const { return 0; }
    virtual AnalysisMask invalidated() const { return 0; }
    virtual ModeMask modes() const { return kModeAll; }
    virtual bool parallelizable() const { return false; }
    // Returns true if the pass transformed anything.
    virtual bool run(PassContext& ctx) = 0;
};

// ---- registry -----------------------------------------------------------------
class PassRegistry {
public:
    struct Entry {
        int order = 0;
        Pass* (*factory)() = nullptr;
    };
    static PassRegistry& instance();
    void add(int order, Pass* (*factory)());
    // Creates passes sorted by catalog order; caller owns the pointers.
    std::vector<Pass*> create_all() const;
    size_t count() const { return entries_.size(); }

private:
    std::vector<Entry> entries_;
};

#define JULES_REGISTER_PASS(CLASS, ORDER, PHASE)                                     \
    static jules::Pass* jules_factory_##CLASS() { return new CLASS(); }              \
    static const bool jules_registered_##CLASS = [] {                               \
        jules::PassRegistry::instance().add(ORDER, &jules_factory_##CLASS);          \
        return true;                                                                \
    }();

// ---- scheduler ------------------------------------------------------------------
class PassManager {
public:
    explicit PassManager(PassContext& ctx) : ctx_(ctx) {}

    // Runs the whole pipeline (SoN passes, post-inline cleanup, linear stage).
    // Returns false on diagnostics errors.
    bool run();

    const std::vector<PassStats>& stats() const { return stats_; }

private:
    bool run_one(Pass& p, PassStats& st);
    bool should_skip(Pass& p, const char*& reason);
    void record_stats(Pass& p, PassStats& st, bool changed, u32 before, u32 after, double ms);

    PassContext& ctx_;
    std::vector<PassStats> stats_;
};

// Renders stats (for --stats) and the parallel-group plan (scheduler DAG info).
void render_pass_stats(std::FILE* out, const std::vector<PassStats>& stats);

} // namespace jules
