// julesc — the JULES compiler driver.
//
//   julesc [options] input.jules
//     -o <file>        output executable (default: a.out)
//     -S               stop after emitting assembly (write .s, do not link)
//     -O<lvl>          optimization level: -O0 -Og -O1 -O2 -O3 -Os -Oz
//                      (bare -O = -O1; no flag defaults to -O2, the release
//                      preset — levels are compile-time budget presets)
//     --fp=M           strict (default) | fast — fast opts into FP semantics
//     --pgo=M          off | instrument | use=<f> | sample=<f> | live
//     --lto=M          none | thin | full (single module => full visibility)
//     --fto            summary-assisted visibility alias (accepted)
//     --jit-budget=B   fast | balanced | peak — JIT compile-latency class
//                      (caps the effective level; not an execution tier)
//     --emit-ir        dump IR after every pass
//     --emit-dot       write final graph as DOT
//     --stats          per-pass statistics
//     --verify         run the graph verifier after every pass
//     --list-passes    print the pass catalog and exit
//     --disable NAME   kill switch for one pass (repeatable)
//     --only a,b,c     run only the named passes
//     --mode M         aot | jit-baseline | jit-optimizing
#include "core/codegen/linear.h"
#include "core/diagnostics/diag.h"
#include "core/lexer/token.h"
#include "core/parser/ast.h"
#include "core/parser/parser.h"
#include "core/sema/sema.h"
#include "core/son/passes/pass.h"
#include "core/son/son.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace jules {
namespace driver {

namespace {

bool parse_kv(const std::string& a, const char* prefix, std::string& value) {
    size_t n = std::strlen(prefix);
    if (a.size() > n && a.compare(0, n, prefix) == 0) {
        value = a.substr(n);
        return true;
    }
    return false;
}

} // namespace

int run(int argc, char** argv) {
    std::string input, output = "a.out";
    bool emit_asm_only = false, emit_dot = false;
    PassOptions opts;
    opts.level = OptLevel::O2;          // default release preset (spec §6)
    opts.requested_level = OptLevel::O2;
    bool stats = false, list_passes = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string v;
        if (a == "-o" && i + 1 < argc) output = argv[++i];
        else if (a == "-S") emit_asm_only = true;
        else if (a == "-O" || a == "-O1") opts.requested_level = OptLevel::O1;
        else if (a.size() >= 3 && a.compare(0, 2, "-O") == 0) {
            if (!parse_opt_level(a, opts.requested_level)) {
                std::fprintf(stderr, "unknown optimization level '%s' (use -O0 -Og -O1 -O2 -O3 -Os -Oz; no -O4, no -Ofast)\n", a.c_str());
                return 2;
            }
        }
        else if (parse_kv(a, "--fp=", v)) {
            if (v == "strict") opts.fp = FpMode::Strict;
            else if (v == "fast") opts.fp = FpMode::Fast;
            else { std::fprintf(stderr, "unknown --fp mode '%s' (strict|fast)\n", v.c_str()); return 2; }
        }
        else if (parse_kv(a, "--pgo=", v)) {
            if (v == "off") opts.pgo = PgoMode::Off;
            else if (v == "instrument") opts.pgo = PgoMode::Instrument;
            else if (v == "live") opts.pgo = PgoMode::Live;
            else if (v.compare(0, 4, "use=") == 0) opts.pgo = PgoMode::Use;
            else if (v.compare(0, 7, "sample=") == 0) opts.pgo = PgoMode::Sample;
            else { std::fprintf(stderr, "unknown --pgo mode '%s' (off|instrument|use=<f>|sample=<f>|live)\n", v.c_str()); return 2; }
            if (opts.pgo == PgoMode::Instrument || opts.pgo == PgoMode::Use ||
                opts.pgo == PgoMode::Sample) {
                std::fprintf(stderr, "note: --pgo=%s accepted; profile plumbing (counters, profile format, evidence thresholds) is not implemented yet — building without profile data\n", v.c_str());
            }
        }
        else if (parse_kv(a, "--lto=", v)) {
            if (v == "none") opts.lto = LtoMode::None;
            else if (v == "thin") opts.lto = LtoMode::Thin;
            else if (v == "full") opts.lto = LtoMode::Full;
            else { std::fprintf(stderr, "unknown --lto mode '%s' (none|thin|full)\n", v.c_str()); return 2; }
        }
        else if (a == "--fto") opts.lto = LtoMode::Fto;
        else if (parse_kv(a, "--jit-budget=", v)) {
            if (v == "fast") opts.jit_budget = JitBudget::Fast;
            else if (v == "balanced") opts.jit_budget = JitBudget::Balanced;
            else if (v == "peak") opts.jit_budget = JitBudget::Peak;
            else { std::fprintf(stderr, "unknown --jit-budget '%s' (fast|balanced|peak)\n", v.c_str()); return 2; }
        }
        else if (a == "--emit-ir") opts.emit_ir = true;
        else if (a == "--emit-dot") emit_dot = true;
        else if (a == "--stats") stats = true;
        else if (a == "--verify") opts.verify_each = true;
        else if (a == "--list-passes") list_passes = true;
        else if (a == "--disable" && i + 1 < argc) opts.disabled.insert(argv[++i], true);
        else if (a == "--only" && i + 1 < argc) {
            std::string list = argv[++i];
            size_t p = 0;
            while (p < list.size()) {
                size_t c = list.find(',', p);
                if (c == std::string::npos) c = list.size();
                opts.only.push_back(list.substr(p, c - p));
                p = c + 1;
            }
        } else if (a == "--mode" && i + 1 < argc) {
            std::string m = argv[++i];
            if (m == "aot") opts.mode = CompileMode::AOT;
            else if (m == "jit-baseline") opts.mode = CompileMode::JitBaseline;
            else if (m == "jit-optimizing") opts.mode = CompileMode::JitOptimizing;
            else { std::fprintf(stderr, "unknown mode '%s'\n", m.c_str()); return 2; }
        } else if (!input.empty()) {
            std::fprintf(stderr, "unexpected argument '%s'\n", a.c_str());
            return 2;
        } else input = a;
    }

    // JIT compile-latency budget caps the effective level (spec §12); the
    // requested level is preserved for reporting. Not an execution tier.
    // AOT compilation has no runtime compile latency, so the budget only
    // engages for the JIT modes.
    if (opts.mode == CompileMode::AOT) {
        opts.level = opts.requested_level;
    } else {
        opts.level = cap_level_for_jit(opts.jit_budget, opts.requested_level);
        if (opts.level != opts.requested_level)
            std::fprintf(stderr, "note: --jit-budget=%s caps effective level %s -> %s\n",
                         opts.jit_budget == JitBudget::Fast ? "fast"
                         : opts.jit_budget == JitBudget::Balanced ? "balanced" : "peak",
                         opt_level_name(opts.requested_level), opt_level_name(opts.level));
    }

    if (list_passes) {
        std::vector<Pass*> all = PassRegistry::instance().create_all();
        std::fprintf(stdout, "%-4s %-46s %-6s %s\n", "#", "name", "stage", "phase");
        for (Pass* p : all) {
            std::fprintf(stdout, "%-4d %-46s %-6s %s\n", p->order(), p->name(),
                         p->stage() == Stage::Son ? "son" : "linear", p->phase_name());
            delete p;
        }
        return 0;
    }

    if (input.empty()) {
        std::fprintf(stderr, "usage: julesc [options] input.jules\n");
        return 2;
    }

    // ---- read source -----------------------------------------------------------
    std::FILE* f = std::fopen(input.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "error: cannot open '%s'\n", input.c_str());
        return 1;
    }
    std::string source;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) source.append(buf, n);
    std::fclose(f);

    Diagnostics diag;

    // ---- frontend ---------------------------------------------------------------
    std::vector<Token> toks;
    if (!lex_source(source, toks, diag)) {
        diag.render(stderr, source);
        return 1;
    }
    SymbolTable syms;
    ModuleAst ast;
    if (!parse_tokens(toks, ast, diag, syms)) {
        diag.render(stderr, source);
        return 1;
    }
    SemaModule sema;
    if (!run_sema(ast, sema, diag, syms)) {
        diag.render(stderr, source);
        return 1;
    }

    // ---- SoN graphs (FnId == index in sema.fns == index in mod.fns) ---------------
    Module mod;
    mod.syms = &syms;
    mod.fns.reserve(sema.fns.size());
    for (const SemaFn& sf : sema.fns) {
        FunctionGraph fg;
        fg.fid = static_cast<FnId>(mod.fns.size());
        fg.name = sf.name;
        fg.always_inline = sf.always_inline;
        fg.no_inline = sf.no_inline;
        fg.is_comptime = sf.is_comptime;
        fg.node_estimate = sf.node_estimate;
        const FnDecl& decl = ast.fns[sf.ast_index];
        if (!build_function(decl, sema, fg, syms, diag)) {
            diag.render(stderr, source);
            return 1;
        }
        mod.fns.push_back(std::move(fg));
    }

    // ---- pass pipeline -------------------------------------------------------------
    AnalysisManager am(mod);
    PassContext ctx(mod, syms, diag, am, opts);
    LinearModule lin;
    ctx.lin = &lin;

    PassManager pm(ctx);
    if (!pm.run()) {
        diag.render(stderr, source);
        return 1;
    }

    if (opts.emit_ir) {
        std::vector<SymbolId> fn_syms;
        fn_syms.reserve(mod.fns.size());
        for (const FunctionGraph& fg : mod.fns) fn_syms.push_back(fg.name);
        for (const FunctionGraph& fg : mod.fns) {
            std::fprintf(stdout, "; ---- final IR: %s ----\n", syms.name(fg.name).data());
            std::fputs(dump_graph_text(fg.g, syms, &fn_syms).c_str(), stdout);
        }
    }
    if (emit_dot) {
        std::vector<SymbolId> fn_syms;
        fn_syms.reserve(mod.fns.size());
        for (const FunctionGraph& fg : mod.fns) fn_syms.push_back(fg.name);
        for (const FunctionGraph& fg : mod.fns) {
            std::string fn = std::string(syms.name(fg.name)) + ".dot";
            std::FILE* df = std::fopen(fn.c_str(), "w");
            if (df) {
                std::fputs(
                    dump_graph_dot(fg.g, syms, syms.name(fg.name).data(), &fn_syms).c_str(),
                    df);
                std::fclose(df);
            }
        }
    }
    if (stats) render_pass_stats(stdout, pm.stats());

    // ---- assembly + link ------------------------------------------------------------
    std::string asm_text = serialize_module_asm(lin, syms);
    std::string asm_path = output + ".s";
    std::FILE* af = std::fopen(asm_path.c_str(), "w");
    if (!af) {
        std::fprintf(stderr, "error: cannot write '%s'\n", asm_path.c_str());
        return 1;
    }
    std::fwrite(asm_text.data(), 1, asm_text.size(), af);
    std::fclose(af);

    if (emit_asm_only) {
        std::fprintf(stdout, "wrote %s\n", asm_path.c_str());
        return 0;
    }

    std::string cmd = "cc -no-pie " + asm_path + " -o " + output;
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::fprintf(stderr, "error: link step failed (%d)\n", rc);
        return 1;
    }
    return 0;
}

} // namespace driver
} // namespace jules

int main(int argc, char** argv) { return jules::driver::run(argc, argv); }
