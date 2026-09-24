# The JULES Superoptimizer (pass 92)

A bounded, deterministic search over post-88 x86-64 MIR windows — the
third machine-optimization tier (84 hand emitters → 85-88 machine passes →
92 search), running only at `-O3`.

## The contract this component answers

> "we are making a superoptimizer, in its own directory for jules.
> and dont just hard code anything"

Enforced structurally:

1. **`src/superopt/` is its own directory and the engine it contains has
   ZERO instruction knowledge.** No opcode names, no patterns, no known
   wins, no latency numbers. It is a search machine: regions, liveness,
   definedness, budgets, test vectors, a priority queue.
2. **Every instruction fact is DATA in the ISA table**
   (`src/targets/x86_64/x64_super_isa.cpp`): one row per MIR opcode —
   operand shapes, variant axes (bin op / condition / size / lea scale),
   exact architectural semantics as a `simulate` closure over a
   multi-lane vector state, liveness `effects`, and latency referenced
   from `DpIsel`'s existing cost classes (no duplicated numbers).
3. **Optimizations are OUTPUTS, never inputs.** The `x*5 → lea
   [x+x*4]`, `x+x → lea [x*2]` and dead-window erasures the search
   finds are results of the search. If a named pattern appears in code,
   the contract is broken.

## How it works

For each function: build the label-region CFG of `lf.code`, run backward
liveness (GPRs, flags, slots) over it, then slide a window over every
maximal run of searchable instructions. Per window:

- **Contract** = locations live after the window (real dataflow, not
  guesses). Only those must match.
- **Search** = Dijkstra-flavored best-first over concrete machine states.
  Every location holds one value per test vector, so a state IS a
  candidate's behavior on the whole lane set. Lazy deletion (best-cost
  map + expanded set) keeps the cheapest program per state — a plain
  dedup kept whichever program was constructed first and let an
  expensive `imul` block its cheaper `lea` twin forever.
- **Pop order**: goal-distance first (`match` = contract locations
  already equal to the original's final state; `match == all` IS the
  goal test), then cost, length, insertion order. Goals surface in a
  handful of pops instead of behind hundreds of equal-cost states.
- **Definedness rule**: a candidate may only read locations live at
  window entry or written earlier in the candidate program. Flags are
  NEVER available at entry (their runtime values are unmodellable).
  Windows whose original violates the rule are skipped, not optimized.
- **Faults / undefined flags**: faulting candidates are discarded;
  windows whose original faults are skipped. Flag lanes the
  architecture leaves undefined (imul's ZF/SF/PF, multi-bit shift OF,
  idiv's everything) carry deterministic poison values, and their
  effects rows set `undef_flags` so windows with such flags live-out
  are skipped. Count-0 shifts pass flags through (`reads_flags`), so
  liveness never drops a flag the original observes.
- **Test vectors** (the anti-"condition never fires" policy): half the
  lanes are corner/random seeded; the other half are CONSTANT-SWEEP —
  every location holds one of the window's own immediate values, so
  every `cmp $K`-style condition fires on some lane. Without this, a
  16-instruction conditional cascade looks like an identity on random
  garbage and the search deletes it (found the hard way — five suite
  miscompiles).
- **Operand pools come from the program**: candidate registers = the
  original window's read+write set (the classic reduction — without it,
  a window next to a call drags the call's whole conservative
  argument-read set into the pool, 8 registers, 12288 Lea2 bindings,
  and the winner sits beyond every budget); immediates = the window's
  constants + {0} + corners.
- **Commit gate**: the winner is re-verified on 7 fresh lane batches
  (84 lanes) before replacing; commits apply back-to-front.

Budgets are pop/candidate/state counts — never wall-clock — so a run is
byte-for-byte reproducible. Defaults: 768 pops/window, 4096/fn, 800
constructions/pop, 2048/window, 2048 states/window, 16-inst windows, +2
growth. All env-tunable: `JULES_SUPEROPT=0` (off),
`JULES_SUPEROPT_STATS=1`, `JULES_SUPEROPT_TRACE=1`,
`JULES_SUPEROPT_POPS / _FN_POPS / _CANDS / _WIN_CANDS / _STATES`.

## The verification ladder (incremental verification)

Verification is TIERED — strongest last, and each tier only pays for the
survivors of the previous one. This is what makes deep search affordable:
99.99% of candidates die at Tier 1 for ~300ns each; a handful of winners
reach the solver.

| Tier | What | Cost | Outcome |
|------|------|------|----------|
| 1 | In-search vector equivalence (the goal test itself) | ~300ns/candidate | wrong candidates discarded during search |
| 2 | Fresh-batch re-verification (commit gate) | 84 lanes/winner | sampling-level confidence before commit |
| 3 | Concrete cross-probe (`JULES_SUPEROPT_SMT_SELFTEST=1`) | one z3 query/window | SMT encoder == simulator, bit-for-bit, or abort |
| 4 | SMT equivalence proof (Z3, `x64_super_smt.cpp`) | one z3 query/commit | Proven / Refuted / Unknown |

Tier 4 encodes the original window and the candidate as two symbolic
programs over the SAME inputs (live-ins shared, entry flags shared) and
asks Z3 whether the live-out contract can differ on ANY input — a proof,
not a sample. It is strictly stronger than sampling in two documented
ways: **faults are exact** (a candidate that fails to fault wherever the
original faults — or faults where it must not — on any unsampled input,
is refuted), and **undefined flags are unconstrained** (a candidate may
not exploit the simulator's deterministic poison values, which model UB
as more defined than the architecture makes it).

Policy: `JULES_SUPEROPT_SMT=0` (off) / `1` (best-effort when z3 is found —
the default; Unknown degrades to the Tier-2 verdict) / `2` (proof
required — Unknown rejects the commit; also the REPRODUCIBILITY mode:
under load a timed-out proof flips an `=1` commit decision, so
byte-identical -O3 outputs under contention require `=2` or `=0`). The
solver is a subprocess (`JULES_Z3_BIN` overrides discovery, per-query
timeout `JULES_SUPEROPT_SMT_TIMEOUT`, default 5s); no link dependency,
graceful absence.

**The differential lock** (Tier 3): the SMT emitters are a second semantic
source next to the sim closures. When self-test is on, every attempted
window's ORIGINAL is re-evaluated through the SMT encoder on the batch-0
concrete inputs and must agree with the simulator bit-for-bit on the
contract, else abort. The full suite passes under this lock — the two
sources are pinned together.

## Throughput

`JULES_SUPEROPT_TPUT=1` prints pops / constructions / simulations per
second (wall clock for the RATES only — search behavior stays
pop-count-driven and deterministic).

Measured on the throughput kernel (`tests/programs/t_super_tput.jules`,
elevated budgets, identical exploration — same pops/construction counts):

| metric | before | after the restructure |
|--------|--------|----------------------|
| pops/sec | 0.23M | 1.09M (4.75x) |
| constructions/sec | 0.64M | 3.07M |
| simulations/sec | 0.43M | 2.02M |

The restructure: (1) the per-construction full-state fingerprint (240+
serially-chained splitmix64 calls, ~1.3us — the dominant cost) became an
incremental XOR-fold of per-position Zobrist terms, updated only over the
write set; (2) the per-construction 2.5KB `VState` copy became an
in-place simulation with a write-set journal (the ISA row's `Effects` IS
the write set — the audit mode proves it on demand); (3) the per-pop
re-seed (312 splitmix64 calls) + full program re-simulation — paid by
every pop, ~99% of them stale lazy-Dijkstra re-pops — is gone: the seed
is computed once per window and only non-stale pops replay their program;
(4) the fingerprint travels with the queue entry; (5) dedup maps are flat
open-addressing tables; (6) hot-loop `getenv` was hoisted.

The audit mode (`JULES_SUPEROPT_AUDIT=1`) recomputes the full fold after
every in-place simulation and after every restore, and against the
replayed state at every pop — proving the journal covers exactly what the
sims write. The full suite passes in audit mode too.

Remaining levers toward the throughput targets (documented, in order):
SIMD lanes for the sims and the fold terms (12 lanes = 2 zmm vectors per
location — the closures are ready to vectorize), lane-count-adaptive
verification strength for the JIT path, and window-level parallelism for
AOT (windows are already independent search problems).

## Honest limits

- **Tier 2 alone is sampling, not proof** — but the ladder now ends in a
  proof when the solver is present: Tier 4's SMT equivalence check on
  every commit, with the Tier-3 differential lock pinning the encoder to
  the simulator. Without z3 the pass honestly degrades to the sampled
  verdict (telemetry shows it).
- **The search is incomplete by design.** Budgets bound it
  deterministically; a window that exhausts them keeps its original
  code. Missed wins are acceptable; wrong code is not.
- **Goal-first ordering trades minimality for reachability.** The
  accepted winner is strictly better than the original (cost, then
  length), but not necessarily the minimal equivalent — cost-first
  ordering left depth-2 goals buried behind the cost-1 mass and the
  pop budget died before reaching them.
- **Fingerprint dedup is 64-bit.** A collision can lose a win, never
  invent one (the goal test compares exact states).
- **Tier-4 proofs are bounded by the solver** (`JULES_SUPEROPT_SMT_TIMEOUT`,
  default 5s/window): a timeout is an honest Unknown, never a silent pass.
  This is the ONE wall-clock dependence in the pass: with the solver at
  `=1`, a machine under load can flip a commit decision that the same
  query decides the other way when it completes. Use `=2` (or `=0`) when
  bit-reproducible outputs are required under contention.
- **Both-fault equivalence ignores slot state at fault time**: when both
  programs fault on an input, the contract is not compared (both trap);
  a difference in a dead slot store before the shared fault is
  observable only to a SIGFPE handler or debugger walking the frame.
  The sampled tiers are stricter (any lane fault = reject) and remain
  first in the ladder.

## Provenance

Instituted 2026-09-24 after the first review round under
`docs/agent_review_rule.md` caught, among others: a broken shift
placeholder, a wrong SHL carry-flag model, the destructive-operand
read-missing liveness hole, the frame-register (rbp/rsp) escape, the
state-dedup-keeps-first-program bug, the operand-bind wipe that zeroed
every lea displacement, and the never-fires-condition vector hole. The
rule is permanent: every change here ships with an independent review.
