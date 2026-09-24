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
byte-for-byte reproducible. Defaults: 768 pops/window (drain-sized:
after the construction caps fire, a pop is just simulate+fingerprint),
4096/fn, 800 constructions/pop, 2048/window, 2048 states/window, 16-inst
windows, +2 growth. All env-tunable: `JULES_SUPEROPT=0` (off),
`JULES_SUPEROPT_STATS=1`, `JULES_SUPEROPT_TRACE=1`,
`JULES_SUPEROPT_POPS / _FN_POPS / _CANDS / _WIN_CANDS / _STATS`.

## Honest limits

- **Vector equivalence is sampling, not proof.** 12 search lanes + 84
  commit lanes (corners, deterministic randoms, constant sweeps). The
  ISA table — semantics as executable data — is exactly the artifact
  that makes an exhaustive (SMT/BMC) equivalence checker possible as a
  future verifier; the engine would not change.
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

## Provenance

Instituted 2026-09-24 after the first review round under
`docs/agent_review_rule.md` caught, among others: a broken shift
placeholder, a wrong SHL carry-flag model, the destructive-operand
read-missing liveness hole, the frame-register (rbp/rsp) escape, the
state-dedup-keeps-first-program bug, the operand-bind wipe that zeroed
every lea displacement, and the never-fires-condition vector hole. The
rule is permanent: every change here ships with an independent review.
