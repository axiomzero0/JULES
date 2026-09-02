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

---
Task ID: 2
Agent: main (Super Z)
Task: Resume after context loss; drive the JULES test suite to fully green (was 14/18 with 4 failures + latent verifier violations).

Work Log:
- Recovered state from filesystem (worklog had only Tasks 0-1; tree already contained the full 89-pass pipeline, julesc binary, 12 tests). Initial run: 14 passed / 4 failed.
- BUG 1 (correctness, builder): emit_call captured the Call's memory input (ins[1] = cur_mem_) BEFORE evaluating arguments, so nested calls (print(heap_cell())) were orphaned from the memory chain; the linearizer then scheduled printf before the call producing its argument (t05_ptr printed a stack address). Fixed by filling ins[1] after the argument loop.
- BUG 2 (dumper): Call fn names resolved aux (FnId = module function index) through the symbol table whose index 0 is reserved for "" — every call mislabeled by one. Fixed by threading fn_syms (FnId -> SymbolId) through dump_graph_text/dot from both call sites.
- Pass manager: added SCCP (8) to the post-inline cleanup re-run set {26,30,23,1,2,3,7,8,9} — mirrors LLVM IPSCCP/Graal; SCCP could never fire at its original slot because locals-as-memory leaves Loads (= BOTTOM) until passes 21-26.
- BUG 3 (SCCP, latent, exposed by the re-run): Const nodes pinned to not-yet-executable blocks were never given lattice values, so branch conditions using them stalled at TOP; optimistic phi values (entry inputs only) were then frozen and rewritten as constants, decapitating loops (t04 lost its backedge, t09 exited 34). Fixed by pre-seeding every Const (constants are block-independent) + drain-end fallback marking both projections for still-TOP conditions.
- BUG 4 (SCCP cleanup): killing dead branch projections left pinned stores/consts/jumps with dead control inputs (verifier: "uses a killed node"). Added phase-5 cascade (kill subgraph pinned to removed control) + Stop input compaction for killed returns.
- BUG 5 (SROA, latent): alloc removal used replace_uses_in_slot(alloc, entry, slot 1), which misses mem phis carrying the alloc in value slots > 1 (t07 phis kept dead alloc inputs). Fixed with replace_uses_as_memory (rewrites all phi value slots).
- BUG 6 (type system edge): ty_ptr() has no ptr-to-ptr in the MVP lattice, so slots for pointer-typed locals got ty_none and violated Alloc/Load/Store pointer invariants. Slots now degrade to *i64 (same address width; Load result carries the real type).
- Test harness: assert_pass_active awk summed only the last per-function stats table (stats are emitted per function); now sums across all tables. t06 gained a medium callee (blend) so CostBasedInlining (78) has work that AlwaysInline (77) doesn't preempt.
- Verified: 18/18 tests (12 programs + 6 pass-activity assertions: GVN=10, SCCP=5, SROA=20, CBI=4, TCO=1, LICM=1 changes), --verify clean on every program after every pass, all three modes (aot/jit-baseline/jit-optimizing) produce correct output, PAT grep scan clean.

Stage Summary:
- Test suite fully green; graph verifier clean; dumper honest. Remaining known gaps are documented SCAFFOLD/VACUOUS passes in docs/pass_status.md (matrix updated with SCCP activation note). No task was delegated to subagents this session.

---
Task ID: 3
Agent: main (Super Z)
Task: Benchmark JULES vs GCC and LLVM/Clang (user: "continue. and benchmark vs GCC and LLVM and CLANG").

Work Log:
- Environment: gcc 14.2.0 present; no clang -> installed LLVM/Clang 19.1.7 user-locally via apt-get download + dpkg -x (clang-19, libclang-cpp19, libclang-common-19-dev) into tmp/clang-root. No root needed.
- Wrote 6 kernel pairs (JULES + identical C) in jules/bench/kernels: fib, tak, primes, mandel, flops, inthash; sizes calibrated so gcc -O3 runs ~0.06-0.26 s CPU (tak args (33,22,12) chosen for 157.9M calls; fib(39); primes<2M; mandel 750x750x280; 80M iters for flops/inthash).
- BUG 7 (correctness, found by kernels on first contact): nested while loops executed once. Root cause chain: PhiSimplification's single-pred Region elimination repinned users with in[0]==id but never rewrote Region->Region predecessor edges referencing the killed node (outer-loop backedge Region). SCCP's unreachable-pred trim then legitimately removed the dead backedge -> loop decapitated; downstream passes folded the graph to garbage. Fixed: splice eliminated region out of successor pred lists in place (any input slot), preserving phi alignment. Regression: t13_nested_loops.
- BUG 8 (correctness, silent miscompile): reg*const lowered as add $imm (ArithRImm emitter lacked a Mul case; t09 passed only because inlining+SCCP constant-folded its 2*17). Fixed: emit imul 3-operand immediate form; added imm32-encodability guard in emit_int_bin with register materialization fallback for wider constants. Regression: t14_mulconst.
- Suite now 20/20 green; --verify clean on all programs; all three modes correct.
- Harness (scripts/jules_bench.py): 9 configs (julesc aot/jit-baseline/jit-optimizing, gcc/clang -O0/-O2/-O3), output verification vs gcc -O2 reference, warmup + adaptive reps pinned to one core. Discovered container cgroup CPU-quota throttling quantizes wall time into ~50 ms steps (identical medians across 7 runs) -> switched primary metric to per-process CPU time via getrusage(RUSAGE_CHILDREN) deltas; wall kept as secondary CSV column.
- Final numbers (CPU-time medians, 7 reps): julesc-aot geomean 4.53x vs gcc-O3, 4.22x vs clang-O3, 2.03x vs gcc-O0, 4.28x vs gcc-O2. Per kernel vs gcc-O3: fib 4.90x (1.04x vs gcc-O0), tak 3.47x, primes 1.86x, mandel 10.53x, flops 4.43x, inthash 5.89x. The three JULES modes are within ~1% of each other on every kernel. Compile-time geomean: julesc 22 ms vs gcc-O3 48 ms vs clang-O3 71 ms (2.2x/3.2x faster). All 54 config x kernel outputs byte-identical (incl. f64 kernels).
- Gap analysis from asm: every SoN node round-trips a stack slot; conditions lower via setcc/movzx/test/jcc (6 instr vs gcc's cmp+jge); no callee-saved register promotion (pass 85 is the lever); no SIMD; memory-backed loop-carried f64 locals hurt mandel most.
- Report: charts (scripts/jules_bench_charts.py), ReportLab body + Template-01 HTML cover (cascade palette seed 19), merged to download/JULES_Compiler_Benchmark_Report.pdf (9 pages, pdf_qa WARN-only: 4 cosmetic table-centering notes from the symmetric stat-box row). Cover HTML also delivered per HTML+PDF dual-delivery rule. Repo: bench/README.md + results committed (4 commits total this task).

Stage Summary:
- Two real miscompiles found+fixed with regressions (t13, t14); suite 20/20.
- Benchmarks reproducible end-to-end: ./build.sh + python3 scripts/jules_bench.py.
- JULES honest position: ~4.5x off -O3 codegen, ~2x off -O0, 2-3x faster compilation; roadmap = register allocation (p85), fused compare-and-branch, f64 loop-local promotion, then vectorization.
