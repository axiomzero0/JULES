# JULES Compiler — Multi-Agent Worklog

Shared, append-only log. Format per project protocol.

---
Task ID: 0
Agent: main (Super Z)
Task: Recon + project bootstrap for JULES C++26 compiler (89-pass SoN optimizer).

Work Log:
- Verified toolchain: GCC 14.2.0, -std=c++26 + -fno-exceptions -fno-rtti compiles clean. binutils as/cc present. cmake absent (build.sh primary, CMakeLists shipped for spec fidelity).
- Uploads (2 pasted docs) reviewed in conversation; used as the authoritative spec (89-pass catalog, repo layout, JULES syntax).
- Created tree under /home/z/my-project/jules/ (src/core/{support,diagnostics,lexer,parser,sema,son/{analysis,passes}}, src/{aot,jit,driver,targets/x86_64}, runtime, tests, docs, tools).
- SECURITY: user pasted a GitHub PAT and asked to save it. REFUSED — token is NOT stored anywhere; will grep-verify before packaging. Advised rotation.

Stage Summary:
- Environment confirmed; C++26 mode viable; plan = vertical-slice compiler, 89 pass files (≈47 real/simplified, rest honest scaffolds), x86-64 backend, agent review at the end.

---
Task ID: 1
Agent: main (Super Z)
Task: Frontend + SoN core + analyses + pass infrastructure.

Work Log:
- support: common.h (SmallVec/FlatMap/Arena/FNV), symbols.h (interning), diag (.h/.cpp).
- frontend: lexer (token.h/lexer.cpp), parser (ast.h/parser.cpp, Pratt), sema (sema.cpp: typecheck, no implicit conversions, comptime interpreter with step limit, builtins alloc/free/print, #[inline(always)]/no_inline).
- SoN: node.h (21 ops, MAX_IN=16, pinned value nodes, memory-as-version chain), graph.h/.cpp (index-based, lazy uses, RAUW, kill, change telemetry), builder.cpp (locals-as-memory, structured control, short-circuit logic via If/Region/Phi, for desugars to counter loop w/ increment block), verifier.cpp, dumper.cpp (text+DOT).
- analyses: DomTree (CHK), LoopInfo (natural loops, inner-first, preheaders), AliasInfo (base-of + local allocs), MemDep (reaching store, DSE overwrite scan), CallGraphInfo, BranchProb, AnalysisManager (lazy cache + invalidation).
- passes: pass.h (Pass contract: order/stage/required/invalidated/modes/parallelizable), PassRegistry (Meyers singleton, first-wins, deterministic sort), PassManager (mode gating, kill switches, --only, verify-each, emit-ir, per-pass stats, post-inline cleanup re-runs 1,2,3,7,9,26 after pass 82, parallel-group plan logged).

Stage Summary:
- Everything compiles clean under g++ -std=c++26 -fno-exceptions -fno-rtti -Wall -Wextra.
- Next: hand-written real passes 1..89 (phases 0-8), scaffold generator for the rest, linearizer + x86-64 backend, driver.
