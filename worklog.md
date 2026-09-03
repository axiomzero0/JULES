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

---
Task ID: 4
Agent: main (Super Z)
Task: Push JULES to the user's GitHub account (user supplied a fresh PAT, explicitly authorized saving it: "TOKEN IS OK TOO BE SAVED").

Work Log:
- Token verified via API: account axiomzero0 (id 316036413); account has 9 existing repos, all public C++ compiler projects, no name collision -> repo created public as axiomzero0/JULES to match the portfolio pattern (user can flip visibility in Settings).
- Workspace repo NOT pushed as-is: it tracks tmp/ (local clang-19 extraction, 349 files), tool-results/, .env, and jules/build/ artifacts. Instead staged a clean repo at release/JULES (185 files, 16,192 lines): src/ + tests/ + bench/ + docs/ + tools/ + build.sh/CMakeLists/sources.cmake + new root README.md + .gitignore (build/, *.o, *.jules_deopt_manifest.txt).
- Bench portability: scripts moved into bench/scripts/ with repo-relative paths (ROOT from __file__; CLANG via JULES_BENCH_CLANG env > PATH > legacy local extraction); report script's import of the workspace-only pdf.py helper made optional. bench/README.md updated accordingly; final merged PDF copied to bench/results/JULES_Compiler_Benchmark_Report.pdf.
- Pre-push validation from the staged tree: ./build.sh builds clean (one pre-existing -Wsign-compare warning), tools/test_runner.sh 20/20, bench harness smoke run OK (then restored canonical 6-kernel/7-rep results.csv + summary.md that the smoke run had overwritten; stray generated jules_deopt_manifest.txt removed - fixture copy stays in bench/kernels/).
- Security: full-tree grep for github_pat/ghp_/gho_/secret/password/Bearer patterns = zero matches; no .env-like files in staged tree. PAT stored only in ~/.git-credentials (0600) with credential.helper store per user authorization; never committed to any repo.
- Push: git init -b main, identity axiomzero0 <316036413+axiomzero0@users.noreply.github.com>, single initial commit e6ed8d1, remote https://github.com/axiomzero0/JULES.git, push verified: remote refs/heads/main == local HEAD; GitHub API confirms repo exists + pushed_at set.
- Workspace hygiene: release/ added to .gitignore; pending workspace changes (mode-bit normalizations from final report regen) committed as 2de0e17; git status clean.

Stage Summary:
- Live at https://github.com/axiomzero0/JULES (public, main, 185 files): full compiler + bench suite + results + PDF report, self-contained and reproducible (verified build/test/bench from the staged tree itself).
- Token handling changed vs Task 0: user explicitly re-authorized saving this token, so it lives in the local git credential store only (0600), outside all repos.
- Note: latest upload (Pasted Content_1788375082637.txt, same content pasted 3x) is the optimization-levels design spec (-O0/-Og/-O1/-O2/-O3/-Os/-Oz + --size/--pgo/--lto/--fto/--jit-budget modifiers) = the NEXT task after this push.

---
Task ID: 5
Agent: main (Super Z)
Task: Implement the optimization-level spec (upload: levels matrix + modifiers); address user criticism "too slow / 16K LoC too small, most passes small" by making the backend real.

Work Log:
- Spec implemented: opt_levels.h/cpp (level enum, spec §7 availability matrix as row prototypes + per-pass table, §8 budgets: inline threshold/budget 16-48/50-600, cleanup rounds 0-3, size bias, RA grade), pass.h PassOptions (level/fp/pgo/lto/jit-budget), scheduler level gating (skip reason "level") + level-driven cleanup rounds, driver CLI (-O0..-Oz, bare -O=O1, default -O2, --fp/--pgo/--lto/--fto/--jit-budget with honest notes for unimplemented PGO plumbing), p78 level budgets, p13 --fp=fast gate, p85 level dispatch.
- Register allocator (x64_ra.cpp, new ~600 lines): linear scan over live ranges from BACKWARDS-LIVENESS DATAFLOW over emitted blocks + per-instruction walk (linear intervals are unsound with backedges — BUG 9: r12 reused for a malloc inside a loop whose linear last-use preceded the latch → infinite loop in t13; fixed by dataflow). Pools: callee-saved rbx/r12-15 (call-crossing), r10/r11 + xmm2-13 (locals), xmm14-15 = isel FP const pool. Poletto-style spill heuristic with density ranking + spans_backedge guard (BUG 10: "furthest end" victimized loop-carried h; "use density" also wrong since linear stream counts static uses; final rule: never victimize backedge-spanning ranges). Frame-pointer elision when no value needs a stack slot (push/pop callee-saves, SysV 16-byte call-alignment pad via lone push rbp; RetNaked/TailCallNaked/PopCal IOps). Dead-def store removal + store/load pair folding INSIDE the RA (self-contained elision decision; pairs fold before promotion — promoting an adjacent same-reg pair is strictly worse than folding it).
- isel: FpBin/FpCmp/FpNeg now take real register operands; FP const pool (xmm14-15, invalidated at calls, fallback rematerialization); cast-of-Const reads fixed (BUG 11: SExt/ZExt read the const's never-written slot — only visible at JIT+O0 where no folding runs; consts now extend directly). SROA: promotable ignores Dead users (killed nodes stay in lazy use lists — BUG 12: loop counters stayed in heap cells) and treats mem-Phi inputs as chain uses.
- Inliner: repin now computes the full backward input closure of the call (memory-only walk skipped Loads — reads carry no memory version — so loads at the call block were misclassified as after-the-call and stranded their users; BUG 13, surfaces at -O0/-Og where no post-inline folding repairs the graph). Phis never move; Jump/If keep always-move.
- Machine peephole (p87): fused compare-and-branch (both the store/load chain form and the post-fold short [Cmp][Setcc][MovZX][Test][Jcc] chain), loop-invariant FP const materialization hoisting (backedge regions, no calls, unwritten target), mov+test folds (reg + cmp-$0,[mem] form), rax accumulator folding, copy-chain elimination through scratch regs with forward-read safety scan, adjacent mov-pair (GP+FP), dead stores.
- Tests: t15_levels (recursion/FP/nested-loop level matrix), t16_racall (RA across calls w/ address-taken heap); runner sweeps all 22 programs at 7 levels (t08 exempt O0/Og: needs TCO like gcc -O0 would); 132 checks green; JIT modes x levels green; --verify clean.
- Bench (7 reps CPU-time medians, 11 configs incl. julesc-aot-O3): geomean 2.79x vs gcc-O3 (was 4.53x), 2.56x vs clang-O3, 1.23x vs gcc-O0. Per kernel vs gcc-O3: primes 1.00x (parity), inthash 2.05x (was 5.89), tak 3.29x, fib 3.49x, flops 3.66x, mandel 5.50x (was 10.53). Compile 21ms vs 51/68ms.
- Docs: pass_status.md rows 13/26/85/86/87 → IMPLEMENTED with mechanism notes; new docs/opt_levels.md; README + bench/README updated (levels, new numbers, new gap analysis: FP xmm0-accumulator round-trips, no live-range splitting, recursion call protocol, no SIMD). Report regenerated (cover + 9-page body, WARN-only QA), pushed to GitHub as commit aee82b1 on top of e6ed8d1 (repo now 15,189 src LoC, +~1,600 real backend lines this task).

Stage Summary:
- Level matrix real and testable (budgets gate the pipeline; levels never change semantics — 132-check matrix proves it).
- The "too slow" complaint: 1.62x runtime improvement from real machinery (RA + fusion + elision + const hoisting), 6 miscompiles total found+fixed across both benchmark rounds.
- Honest remaining gaps documented in report §6; roadmap: 3-operand FP isel + phi coalescing, live-range splitting, then vectorization.

---
Task ID: 6
Agent: main (Super Z)
Task: Continue performance work after user criticism ("too slow / 16K LoC too small, most passes small"): deepen pass 85 into a real two-operand allocator with coalescing, fix latent bugs exposed along the way, re-bench, push.

Work Log:
- Root-caused remaining gaps from disassembly: every FP op round-tripped xmm0 (3 instrs/op), int ops threaded rax/rcx (mov chains per op), FP const pool capped at 2 regs rematerializing per iteration (flops), mandel had 7 loop-carried GP values vs 7 pool regs (temps + bool spilled through memory), RA double-pushed r12/r14 (frame corruption risk), tak kept a zombie epilogue after TCO.
- SCCP re-run miscompile (found via t16 @ real -O3): the driver capped AOT -O3 to -O2 via --jit-budget (so true O3 never ran in Task 5's matrix). Real O3: inlined copies + SCCP re-runs hit `If` nodes whose dead projection a prior sweep already killed — process(If) bailed without marking the SURVIVING projection executable -> downstream blocks starved -> unreachable-pred trim removed an inlined loop's ENTRY edge -> PhiSimplification repinned the loop If under its own IfTrue (control cycle) -> truncated main. Fixed: mark surviving/both-existing projections; AOT no longer jit-budget-capped. Regression t17_sccp_rerun.
- Pass 85 rewrite (batch 2, the core): run() restructured to pair_fold -> analyze -> extract_fuses -> assign -> retarget -> rewrite -> finalize. Generation-split exact liveness (multi-def phi slots get one [start,end] per linear redefinition; hulls over-approximate). Accumulator-chain fusion: [load sA->acc][gap loads][chain ops][store sZ<-acc] rewrites into Z's register ([mov Z,X][op Z,...]); in place (leading move dropped) when phi-cycle coalescing lands the pair on one register — loop updates emit `addq $1,%r11` / `addsd %xmm4,%xmm3` like production compilers. Hint retarget: phi-copy hints (ops==0 chains) always; accumulator hints only for non-loop-carried sources; validated on generation-disjointness with touching-as-handover; third-range hull check; crosses_call class check.
- Three soundness holes found by the 140-check matrix, each fixed with a gate: (1) result slot feeding its own chain (GVN commutative swaps make B the phi slot; gap-slot z-check at extraction); (2) coalescing landing a chain operand on Z's register (rewrite-time operand_on_z check over final assignments — slot identity is not enough); (3) in-place clobber of loop-carried sources (static "last use" lies — the use re-executes per iteration; only phi-successor Z or non-backedge-spanning A may fuse in place).
- assign(): expiry allows u.last_live == r.first_live when the newcomer starts at a def (handover, not interference — use-start newcomers keep strict <); caller pool extended with rdx/rsi/rdi/r8/r9 in call-free functions after a write-scan of the emitted stream (mandel pressure relief: bools/temps promote, branch fusion fires through promoted booleans).
- isel: FP const pool grows 15->8 with a watermark shared with the RA pool (flops: 6 pooled constants, zero per-iteration rematerialization); +0.0 -> xorpd/xorps (new IOp::FpZero). GP operand folding (ArithRR/CmpRR/Test B-side + non-destructive A-side; FpCmp A-side) mirrors the existing FP fold. p87: linear dead code after unconditional transfers. finalize(): callee-save push dedup per physical register.
- Tests: 140/140 (23 programs incl. t17; 7 levels x 3 modes; --verify clean). Bench (CPU-time medians, 6 kernels x 10 configs, outputs verified): geomean vs gcc-O3 **2.14x** (was 2.79x): mandel 5.50->2.47x, inthash 2.05->1.27x, flops 3.66->2.69x, primes 1.00x parity, tak 3.25x, fib 3.49x; julesc-aot now BEATS gcc-O0 (0.95x). Compile 21ms vs 48/69ms. Charts + 9-page PDF report regenerated.
- Docs: pass_status rows 8/85/87 updated with mechanisms + soundness gates; bench/README + release README new numbers + honest gap analysis (call protocol for fib/tak, FMA+SIMD for flops, FP dep chain for mandel, no live-range splitting).
- Pushed: https://github.com/axiomzero0/JULES commit f4dd07d (on aee82b1), 16 files, +869/-263; pre-push validation from the staged tree (fresh build + 140/140). Workspace src now 15,755 LoC.

Stage Summary:
- "Too slow" answered: 4.53x -> 2.79x -> 2.14x vs gcc -O3 across two sessions; primes at parity, mandel halved again. The machinery is real (exact-liveness coalescing with three documented soundness gates) and the honest roadmap is in the README.
- 7th real miscompile found+fixed total (SCCP re-run projection starvation); regression-locked.
- Next levers by measured impact: recursive call protocol (fib/tak), FMA+SIMD (flops), live-range splitting, then vectorization.
---
Task ID: 7
Agent: main (Super Z)
Task: Pass audit + reality deepening ("lets make sure we have all the passes, that they are real"), answering the "too slow / passes too small" criticism with measured hot-loop work.

Work Log:
- AUDIT: --list-passes = 89 rows; scheduler orders by catalog number with mode/level/kill-switch gating; --stats telemetry confirmed on all 6 bench kernels (every pass reports ran/skipped(reason) + changes). Audit is reproducible via CLI + regression-locked pass-activity assertions in the test runner (now incl. LoopUnrolling, Predication).
- Hot-loop disassembly found the real flops/mandel/inthash waste: (a) loop guards ran 2 taken branches/iteration (no rotation); (b) the bound's setup mov never folded into the compare (single-pass adjacency missed the A-side after the B-side died); (c) FP register-recurrence loops kept 2-3 movsd round-trips per iteration because the accumulator-fusion chain walk broke at pair-folded snapshot stores and at in-loop FP const materialization pairs; (d) t's single-use home store [movsd H<-S; consumer reads H] round-tripped.
- x64_ra.cpp: fixpoint operand folds that look THROUGH killed instructions (next_live); snapshot stores + const-materialization pairs as legal chain elements/gaps; self-update chains (z==a, `a = a*c+d` through the phi slot) fuse in place with gap-position soundness rules; single-use single-DEF home-store forwarding (two bugs found by t13/t19 and fixed: multi-def phi slots must never forward; the window scan must block on ANY reference to the scratch S — a setcc-writes-AL/RAX gap once left a hoisted `a<b` bool in %rax to be clobbered by the loop body's own setcc); CmpRImm A-side fold (loop guards: `cmp $imm, %reg` with no setup mov); stack slot coloring in finalize() (pass 28: disjoint-range memory slots share rbp offsets, addr-taken slots never share; ra_colored telemetry).
- x64_emit.cpp: LOOP ROTATION (new x64_loop_rotate, called by p88): [head: cond; jcc body][exit][body; jmp head] -> [entry: jmp check][body][check: cond; jcc body] — one taken branch per iteration; label-based jumps make block moves safe; multi-latch backedges re-run the guard through the shim. FP-const hoisting moved from the p87 peephole family into p88 (x64_hoist_loop_constants) with rotated-region-aware insertion (before the entry shim, so both fallthrough and jump entries execute it). p87 gained: movzx-dst retarget ([movzbq rax][mov X<-rax] -> [movzbq X]), and/or+branch accumulator fold, redundant-test elimination after flag-setting ops ([and][test][jne] -> [and][jne]).
- p48 Predication IMPLEMENTED (was SCAFFOLD): short-circuit `&&`/`||` flatten to Bin(And|Or) when the RHS slice is pure (phis as dominating loop-header leaves); nested short-circuits flatten iteratively; orientation by phi-input constant VALUE (consts get re-pinned to Start by earlier passes — pin-based detection was the first bug); projection contents (incl. the short-circuit const itself) repinned to the branch block before the projections die (verifier caught the killed-input shape); duplicate-pred guard.
- p42 LoopUnrolling + p44 LoopPeeling IMPLEMENTED (were SCAFFOLD), shared cloner in passes/loop_transforms.cpp: counted-loop matcher (const trip, IV Add(phi,+k), no early exits); unroll F-1 copies chained behind the body with per-copy seed remaps (copy m sees iteration base+m; header latch + phi backedges retarget to the last copy; IV steps F*k); peeling attaches r=T%F copies at loop ENTRY (unconditional — T is exact) so the residual trips T-r and unrolls exactly; cleanup set extended with 44,42 (post-inline const bounds). Cloner bugs found+fixed: intra-block clone order must be dependency-ordered (users-list order cloned an Add before the Mul it read — wrong sum, caught by t18), projection readiness depends on the internal If's pin block.
- 9th and 10th real miscompiles found+fixed this task (multi-def forwarding @t13/Oz; setcc-scratch window gap @t19/Os+Oz — both bisected to root cause and regression-locked via t18/t19).
- Reclassified honestly: p27 BitfieldLowering, p36 LockElision SCAFFOLD -> VACUOUS (the language has no bitfield types / no sync ops — nothing to transform); pass_status.md roll-up: 49 IMPLEMENTED / 5 SIMPLIFIED / 6 VACUOUS / 29 SCAFFOLD (each scaffold row now names its specific blocker).
- Tests: 158/158 (t18_unroll, t19_predication added with pass-activity assertions; 7 levels x AOT; JIT modes spot-verified). Bench (7 reps, outputs verified): geomean vs gcc-O3 2.14x -> 1.90x: flops 2.69x -> 1.39x (loop now gcc's own 9-instruction do-while shape), mandel 2.47x -> 2.44x, inthash 1.27x -> 1.22x, primes parity, tak 3.36x, fib 3.41x. julesc-aot vs gcc-O0: 0.81x (was 0.95x). Compile 21ms unchanged. Charts + PDF report regenerated.

Stage Summary:
- The audit answer: all 89 passes exist, run, and report telemetry; 4 more became real transforms this session (28, 42, 44, 48) + 1 machine pass (88 rotation+hoisting), 2 honestly reclassified VACUOUS with language-level justification, every remaining scaffold names its concrete blocker (vector types, PGO profiles, JIT guard IR...).
- The speed answer: 1.90x geomean vs gcc -O3 (from 2.14x this session, 4.53x originally); flops at 1.39x with instruction-parity loop shape.
- 10 total real miscompiles found+fixed across the project lifetime; every fix carries a regression test.
- Pushed: github.com/axiomzero0/JULES (commit on f4dd07d), pre-push validation from the staged tree (fresh build + 158/158).
