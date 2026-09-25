#!/usr/bin/env bash
# JULES test runner: compiles every tests/programs/*.jules, runs it, diffs
# against tests/expected/*.txt, and asserts that specific passes REPORTED
# activity for the programs designed to exercise them (self-verification
# that the optimizer is actually transforming, not just claiming to).
set -uo pipefail
cd "$(dirname "$0")/.."
JULESC=${JULESC:-./build/julesc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

run_test() {
    local name=$1
    local src="tests/programs/${name}.jules"
    local exp="tests/expected/${name}.txt"
    local out="$WORK/${name}.out"

    if ! timeout 30 $JULESC "$src" -o "$WORK/${name}.bin" > "$out.compile" 2>&1; then
        echo "FAIL $name (compile)"
        sed 's/^/    /' "$out.compile" | tail -5
        fail=$((fail + 1))
        return 1
    fi
    timeout 30 "$WORK/${name}.bin" > "$out" 2>&1
    local rc=$?
    if [ $rc -ne 0 ] && [ $rc -ne 1 ]; then
        echo "FAIL $name (run exit=$rc)"
        fail=$((fail + 1))
        return 1
    fi
    if ! diff -u "$exp" "$out" > "$out.diff"; then
        echo "FAIL $name (output)"
        sed 's/^/    /' "$out.diff" | head -10
        fail=$((fail + 1))
        return 1
    fi
    echo "PASS $name"
    pass=$((pass + 1))
    return 0
}

# Level-matrix runner: compile + run + diff at a specific -O level.
run_level() {
    local name=$1 lvl=$2
    local src="tests/programs/${name}.jules"
    local exp="tests/expected/${name}.txt"
    local out="$WORK/${name}_${lvl}.out"

    if ! timeout 30 $JULESC -"$lvl" "$src" -o "$WORK/${name}_${lvl}.bin" > /dev/null 2>&1; then
        echo "FAIL $name @ -$lvl (compile)"
        fail=$((fail + 1))
        return 1
    fi
    timeout 30 "$WORK/${name}_${lvl}.bin" > "$out" 2>&1
    if ! diff -q "$exp" "$out" > /dev/null 2>&1; then
        echo "FAIL $name @ -$lvl (output)"
        fail=$((fail + 1))
        return 1
    fi
    pass=$((pass + 1))
    return 0
}

# pass-activity assertion: sum of the `changes` column across ALL per-function
# stat tables must be > 0 (stats are emitted one table per function; taking the
# last table's value only would miss work done in earlier functions).
assert_pass_active() {
    local name=$1
    local pass_name=$2
    local stats
    stats=$(timeout 30 $JULESC --stats "tests/programs/${name}.jules" -o "$WORK/a.bin" 2>/dev/null |
            awk -v p="$pass_name" '$2 == p {v += $4} END {print v + 0}')
    if [ -z "$stats" ] || [ "$stats" = "0" ]; then
        echo "FAIL $name (pass '$pass_name' reported no changes)"
        fail=$((fail + 1))
        return 1
    fi
    echo "PASS $name [$pass_name changes=$stats]"
    pass=$((pass + 1))
}

# Kill-switch assertion (2026-09-18 audit): --disable must isolate a pass
# EVERYWHERE — including the post-inline cleanup re-run, which previously
# bypassed the gate (a disabled SROA/GVN/etc. silently ran again in the
# cleanup sweep). The `changes` sum across all stat tables must be 0 AND
# the pass must show at least one skipped(disabled) row.
assert_pass_disabled() {
    local name=$1
    local pass_name=$2
    local stats skipped
    stats=$(timeout 30 $JULESC --stats --disable "$pass_name" "tests/programs/${name}.jules" -o "$WORK/a.bin" 2>/dev/null |
            awk -v p="$pass_name" '$2 == p {v += $4} END {print v + 0}')
    skipped=$(timeout 30 $JULESC --stats --disable "$pass_name" "tests/programs/${name}.jules" -o "$WORK/a.bin" 2>/dev/null |
              awk -v p="$pass_name" '$2 == p && $3 ~ /^skipped/ {n++} END {print n + 0}')
    if [ -n "$stats" ] && [ "$stats" != "0" ]; then
        echo "FAIL $name (pass '$pass_name' reported changes=$stats despite --disable)"
        fail=$((fail + 1))
        return 1
    fi
    if [ -z "$skipped" ] || [ "$skipped" = "0" ]; then
        echo "FAIL $name (pass '$pass_name' never showed skipped(disabled))"
        fail=$((fail + 1))
        return 1
    fi
    echo "PASS $name [$pass_name fully disabled]"
    pass=$((pass + 1))
}

# Level matrix: every program must produce its expected output at every
# optimization level (levels are budget presets, never semantic changes).
# t08_tco is exempt at -O0/-Og: its 1M-deep tail recursion requires TCO,
# which the spec gates off at debug levels (the same program overflows the
# stack under `gcc -O0`).
LEVELS="O0 Og O1 O2 O3 Os Oz"
for t in tests/programs/*.jules; do
    name="$(basename "$t" .jules)"
    run_test "$name"
    for lvl in $LEVELS; do
        if [ "$name" = "t08_tco" ] && { [ "$lvl" = "O0" ] || [ "$lvl" = "Og" ]; }; then
            continue
        fi
        run_level "$name" "$lvl"
    done
done

# Strict borrow checker (opt-in `import strict;`): rejection tests.
# tests/reject/<name>.jules must FAIL to compile, and the diagnostics must
# contain the (single-line, fixed-string) substring in tests/reject/<name>.txt.
# This is the negative side of the strict-mode contract: without the import
# these programs compile freely (raw-pointer default); with it they must be
# rejected before any code is emitted.
run_reject() {
    local name=$1
    local src="tests/reject/${name}.jules"
    local exp="tests/reject/${name}.txt"
    local out="$WORK/rej_${name}.log"
    if timeout 30 $JULESC "$src" -o "$WORK/rej_${name}.bin" > "$out" 2>&1; then
        echo "FAIL $name (rejection test compiled successfully)"
        fail=$((fail + 1))
        return 1
    fi
    local pattern
    pattern=$(cat "$exp" 2>/dev/null)
    if [ -z "$pattern" ]; then
        echo "FAIL $name (missing expected-substring file $exp)"
        fail=$((fail + 1))
        return 1
    fi
    if ! grep -qF "$pattern" "$out"; then
        echo "FAIL $name (diagnostics missing '$pattern')"
        sed 's/^/    /' "$out" | tail -4
        fail=$((fail + 1))
        return 1
    fi
    echo "PASS $name [strict rejection: $pattern]"
    pass=$((pass + 1))
}
for r in tests/reject/*.jules; do
    [ -e "$r" ] || continue
    run_reject "$(basename "$r" .jules)"
done

# Optimizer self-verification: the pass must have actually transformed.
assert_pass_active t02_gvn GlobalValueNumbering
assert_pass_active t03_sccp SparseConditionalConstantPropagation
assert_pass_active t04_sroa ScalarReplacementOfAggregates
assert_pass_active t06_inline CostBasedInlining
assert_pass_active t08_tco TailRecursionElimination
assert_pass_active t09_licm LoopInvariantCodeMotion
assert_pass_active t18_unroll LoopUnrolling
assert_pass_active t19_predication Predication
assert_pass_active t24_vectorize LoopVectorizer
assert_pass_active t25_slp SLPVectorizer
assert_pass_active t20_accumulator TailRecursionElimination
assert_pass_active t29_minmax SIMDIntrinsicMatching
assert_pass_active t29_minmax IfConversion
assert_pass_active t31_idiom IdiomRecognition
assert_pass_active t32_fusion LoopFusion
assert_pass_active t33_interchange LoopInterchange
assert_pass_active t34_interleave InterleavedAccessRecognition
assert_pass_active t35_fission LoopFission
assert_pass_active t30_storemerge StoreMerging
# Masked vectorization (pass 61): the if-converted selects must pack and
# the mask blend must lower (i32 lane masks + f64 cmppd masks)
assert_pass_active t38_maskvec IfConversion
assert_pass_active t38_maskvec LoopVectorizer
assert_pass_active t38_maskvec MaskGeneration
# Spine-tail base widening: the accumulator transform must fire on the
# canonical descending shapes (fibrec widens through the folded table).
assert_pass_active t39_widenbase TailRecursionElimination
# Partial evaluation (pass 90): mixed const/dynamic call sites must
# specialize — the binding set dedups across the two c==1 sites and the
# c==2 / (x,y) bindings create their own variants.
assert_pass_active t40_static_pe PartialEvaluation

# ---- pass-scaffold completion round (2026-09-25): the activity asserts ----
# p15 (NarrowingTransform): extend-feed-trunc cancellation fires
assert_pass_active t43_narrow NarrowingTransform
# p27 (BitfieldLowering): nested segment-write canonicalization fires
assert_pass_active t44_bfmerge BitfieldLowering
# p33 (PartialEscapeAnalysis): allocation sinking into the single using arm
assert_pass_active t46_pea PartialEscapeAnalysis
# p57 (OuterLoopVectorizer): short-inner flattening (the SLP re-run packs)
assert_pass_active t47_outervec OuterLoopVectorizer
# p34/p72/p73 fire in their PGO flows (run_pgo_test asserts the use-build
# changes; the instrument-build assert for p34 is custom below)
assert_pass_disabled t43_narrow NarrowingTransform
assert_pass_disabled t46_pea PartialEscapeAnalysis

# ---- review-fix round (2026-09-25): the soundness regression locks ----
# p28: the folded direct-slot compare read must keep the coloring sound
# while the recolor still fires on the spill-heavy shape.
assert_pass_active t51_p28cmp StackSlotColoring
assert_pass_disabled t51_p28cmp StackSlotColoring
# p33 hazard site (store through the pointer outside the arm) must be
# REFUSED while the clean site still sinks: activity = the clean site.
assert_pass_active t52_pea_store PartialEscapeAnalysis
assert_pass_disabled t52_pea_store PartialEscapeAnalysis
# p15 mixed 32-bit domains: the transform must decline (exact-type
# identity), so the graph verifier stays clean on this shape.
if ! timeout 30 $JULESC --verify tests/programs/t54_mixednarrow.jules -o "$WORK/t54v.bin" > "$WORK/t54v.log" 2>&1; then
    echo "FAIL t54_mixednarrow (--verify compile)"
    fail=$((fail + 1))
else
    echo "PASS t54_mixednarrow [--verify clean on mixed 32-bit domains]"
    pass=$((pass + 1))
fi

# Kill-switch hold across the post-inline cleanup re-run (audit fix):
# cleanup-set passes must stay dead when disabled.
assert_pass_disabled t04_sroa ScalarReplacementOfAggregates
assert_pass_disabled t02_gvn GlobalValueNumbering
assert_pass_disabled t30_storemerge StoreMerging
# The PE/deopt family honors the same kill switches everywhere.
assert_pass_disabled t40_static_pe PartialEvaluation
assert_pass_disabled t40_static_pe PartialDeoptimization

# Superoptimization (pass 92): -O3-only search tier. The activity assert
# must compile at -O3 (the availability matrix gates it off elsewhere), and
# the kill switch must hold exactly like every other pass.
assert_pass_active_l3() {
    local name=$1
    local pass_name=$2
    local stats
    stats=$(timeout 30 $JULESC -O3 --stats "tests/programs/${name}.jules" -o "$WORK/a.bin" 2>/dev/null |
            awk -v p="$pass_name" '$2 == p {v += $4} END {print v + 0}')
    if [ -z "$stats" ] || [ "$stats" = "0" ]; then
        echo "FAIL $name (pass '$pass_name' reported no changes at -O3)"
        fail=$((fail + 1))
        return 1
    fi
    echo "PASS $name [$pass_name changes=$stats at -O3]"
    pass=$((pass + 1))
}
assert_pass_active_l3 t_superopt Superoptimization

# kill switch at -O3 (the level where the pass actually runs — a default-
# level assert would be vacuous since the availability matrix gates 92 to
# O3), plus the pass-internal JULES_SUPEROPT=0 env switch.
assert_pass_disabled_l3() {
    local name=$1
    local pass_name=$2
    local stats skipped
    stats=$(timeout 30 $JULESC -O3 --stats --disable "$pass_name" "tests/programs/${name}.jules" -o "$WORK/a.bin" 2>/dev/null |
            awk -v p="$pass_name" '$2 == p {v += $4} END {print v + 0}')
    skipped=$(timeout 30 $JULESC -O3 --stats --disable "$pass_name" "tests/programs/${name}.jules" -o "$WORK/a.bin" 2>/dev/null |
              awk -v p="$pass_name" '$2 == p && $3 ~ /^skipped/ {n++} END {print n + 0}')
    if [ -n "$stats" ] && [ "$stats" != "0" ]; then
        echo "FAIL $name (pass '$pass_name' reported changes=$stats despite --disable at -O3)"
        fail=$((fail + 1))
        return 1
    fi
    if [ -z "$skipped" ] || [ "$skipped" = "0" ]; then
        echo "FAIL $name (pass '$pass_name' never showed skipped(disabled) at -O3)"
        fail=$((fail + 1))
        return 1
    fi
    echo "PASS $name [$pass_name fully disabled at -O3]"
    pass=$((pass + 1))
}
assert_pass_disabled_l3 t_superopt Superoptimization

envstats=$(JULES_SUPEROPT=0 timeout 30 $JULESC -O3 --stats "tests/programs/t_superopt.jules" -o "$WORK/a.bin" 2>/dev/null |
           awk -v p="Superoptimization" '$2 == p {v += $4} END {print v + 0}')
if [ -n "$envstats" ] && [ "$envstats" != "0" ]; then
    echo "FAIL t_superopt (JULES_SUPEROPT=0 reported changes=$envstats)"
    fail=$((fail + 1))
else
    echo "PASS t_superopt [JULES_SUPEROPT=0 fully off]"
    pass=$((pass + 1))
fi

# Tier-4 verification (Z3): when the solver is present, the pass-92 commits
# on t_superopt must be PROVEN equivalent (t4: proven>0, refuted=0). When
# z3 is absent the assertion is skipped (the tier degrades to sampling).
if command -v z3 >/dev/null 2>&1 || [ -x "$HOME/.venv/bin/z3" ]; then
    t4=$(JULES_SUPEROPT_STATS=1 JULES_SUPEROPT_SMT_TIMEOUT=60 timeout 120 $JULESC tests/programs/t_superopt.jules -o "$WORK/a.bin" -O3 2>&1 |
         awk '/superopt.*t4:/ {for (i = 1; i <= NF; ++i) if ($i ~ /^proven=/) {split($i, a, "="); p += a[2]} if ($i ~ /^refuted=/) {split($i, a, "="); r += a[2]}} END {print p + 0, r + 0}')
    t4p=$(echo "$t4" | cut -d' ' -f1)
    t4r=$(echo "$t4" | cut -d' ' -f2)
    if [ "$t4p" = "0" ] || [ "$t4r" != "0" ]; then
        echo "FAIL t_superopt [tier-4 proven=$t4p refuted=$t4r — expected proven>0 refuted=0]"
        fail=$((fail + 1))
    else
        echo "PASS t_superopt [tier-4 Z3: proven=$t4p refuted=$t4r]"
        pass=$((pass + 1))
    fi
else
    echo "SKIP t_superopt [tier-4: no z3 binary on PATH]"
fi

# PGO round-trip (pass 43): instrument -> run (writes jules.prof) -> use.
# Asserts: the instrumented binary's output is UNCHANGED (instrumentation
# must not alter semantics), the profile exists and is non-trivial, the
# use build reports ProfileGuidedUnrolling activity, and the use binary's
# output matches the expected file.
run_pgo_test() {
    local name=$1
    local pass_name=${2:-ProfileGuidedUnrolling}
    local src="tests/programs/${name}.jules"
    local exp="tests/expected/${name}.txt"
    local dir="$WORK/${name}_pgo"
    mkdir -p "$dir"

    if ! timeout 30 $JULESC --pgo=instrument "$src" -o "$dir/inst.bin" > "$dir/inst.log" 2>&1; then
        echo "FAIL $name (pgo instrument compile)"
        fail=$((fail + 1))
        return 1
    fi
    (cd "$dir" && timeout 30 ./inst.bin > inst.out 2>&1)
    if ! diff -q "$exp" "$dir/inst.out" > /dev/null; then
        echo "FAIL $name (pgo instrument output)"
        fail=$((fail + 1))
        return 1
    fi
    if [ ! -s "$dir/jules.prof" ]; then
        echo "FAIL $name (pgo profile not written)"
        fail=$((fail + 1))
        return 1
    fi
    # profile sanity: magic JPG1 + a counter count > 0
    local n
    n=$(od -An -t u4 -j 4 -N 4 "$dir/jules.prof" | tr -d ' ')
    if [ -z "$n" ] || [ "$n" = "0" ]; then
        echo "FAIL $name (pgo profile empty)"
        fail=$((fail + 1))
        return 1
    fi

    local changes
    changes=$(timeout 30 $JULESC --pgo=use="$dir/jules.prof" --stats "$src" -o "$dir/use.bin" 2>/dev/null |
              awk -v p="$pass_name" '$2 == p {v += $4} END {print v + 0}')
    if [ -z "$changes" ] || [ "$changes" = "0" ]; then
        echo "FAIL $name (pass '$pass_name' reported no changes in use mode)"
        fail=$((fail + 1))
        return 1
    fi
    timeout 30 "$dir/use.bin" > "$dir/use.out" 2>&1
    if ! diff -q "$exp" "$dir/use.out" > /dev/null; then
        echo "FAIL $name (pgo use output)"
        fail=$((fail + 1))
        return 1
    fi

    # negative: a malformed profile must warn, not break the build, and the
    # output must still be correct (the driver degrades to no-profile mode)
    echo "JUNK" > "$dir/bad.prof"
    if timeout 30 $JULESC --pgo=use="$dir/bad.prof" "$src" -o "$dir/bad.bin" > "$dir/bad.log" 2>&1; then
        timeout 30 "$dir/bad.bin" > "$dir/bad.out" 2>&1
        if ! diff -q "$exp" "$dir/bad.out" > /dev/null; then
            echo "FAIL $name (pgo bad-profile output)"
            fail=$((fail + 1))
            return 1
        fi
        if ! rg -q 'warning: profile' "$dir/bad.log"; then
            echo "FAIL $name (pgo bad-profile warning missing)"
            fail=$((fail + 1))
            return 1
        fi
    else
        echo "FAIL $name (pgo bad-profile compile)"
        fail=$((fail + 1))
        return 1
    fi

    echo "PASS $name [$pass_name changes=$changes, profile counters=$n]"
    pass=$((pass + 1))
}
run_pgo_test t36_pgo
run_pgo_test t41_partial_deop PartialDeoptimization
run_pgo_test t42_range_deop PartialDeoptimization
# t46 closes the PEA feedback loop end-to-end: the instrument build writes
# the alloc-site blocks (pass 34), the use build reads them (pass 33) and
# the sinking still fires — output must survive both builds.
run_pgo_test t46_pea PartialEscapeAnalysis
# t48/t49 generate the profiles the guard-family detection asserts below
# consume; the ladder creation (PartialDeoptimization) is the activity
# each flow is asserted on.
run_pgo_test t48_guardhoist PartialDeoptimization
run_pgo_test t49_guardweaken PartialDeoptimization

# t42 RANGE deopt (pass 91): the standard flow above proves the ladder
# fires on profile-derived hulls and the output survives; the adversarial
# round below FORCES the range guards to fail. A range hull from a real
# profile covers every observed value, so failure is induced by shrinking
# the recorded [min, max] counters below reality (scripts/t42_shrink_hull.py:
# c [3,9] -> [4,8], x [0,199] -> [1,198]) — c == 3 / c == 9 / x == 199 then
# transfer DOWN the ladder (generic / rung-1) instead of the innermost
# variant, and the output must stay byte-identical.
t42_range_deop_adversarial() {
    local src="tests/programs/t42_range_deop.jules"
    local exp="tests/expected/t42_range_deop.txt"
    local dir="$WORK/t42_range_deop_adv"
    mkdir -p "$dir"
    cp "$dir/../t42_range_deop_pgo/jules.prof" "$dir/real.prof"
    if ! python3 scripts/t42_shrink_hull.py "$dir/real.prof" "$dir/adv.prof" \
         > "$dir/patch.log" 2>&1; then
        echo "FAIL t42_range_deop (adversarial profile patch)"
        fail=$((fail + 1))
        return 1
    fi
    if ! timeout 30 $JULESC --pgo=use="$dir/adv.prof" --stats "$src" \
         -o "$dir/adv.bin" > "$dir/adv.log" 2>&1; then
        echo "FAIL t42_range_deop (adversarial compile)"
        fail=$((fail + 1))
        return 1
    fi
    timeout 30 "$dir/adv.bin" > "$dir/adv.out" 2>&1
    if ! diff -q "$exp" "$dir/adv.out" > /dev/null; then
        echo "FAIL t42_range_deop (adversarial output — forced deopt broke semantics)"
        fail=$((fail + 1))
        return 1
    fi
    echo "PASS t42_range_deop [adversarial: shrunk hulls force the range guards to deopt]"
    pass=$((pass + 1))
}
# the adversarial round runs only after the standard t42 round succeeded
# (it consumes t42's profile); guarded so one failure doesn't cascade.
if [ -d "$WORK/t42_range_deop_pgo" ]; then
    t42_range_deop_adversarial
fi


# p34 (AllocationSiteProfiling): fires only in instrument mode — assert
# the instrument-build's stats directly (run_pgo_test asserts use builds).
p34stats=$(timeout 30 $JULESC --pgo=instrument --stats tests/programs/t46_pea.jules -o "$WORK/p34.bin" 2>/dev/null |
           awk -v p="AllocationSiteProfiling" '$2 == p {v += $4} END {print v + 0}')
if [ -z "$p34stats" ] || [ "$p34stats" = "0" ]; then
    echo "FAIL t46_pea (pass 'AllocationSiteProfiling' reported no changes in instrument build)"
    fail=$((fail + 1))
else
    echo "PASS t46_pea [AllocationSiteProfiling instrument changes=$p34stats]"
    pass=$((pass + 1))
fi

# p63 (AutoSOATransform): opt-in --soa; the default build must NOT fire.
soastats=$(timeout 30 $JULESC --soa --stats tests/programs/t50_soa.jules -o "$WORK/soa.bin" 2>/dev/null |
           awk -v p="AutoSOATransform" '$2 == p {v += $4} END {print v + 0}')
if [ -z "$soastats" ] || [ "$soastats" = "0" ]; then
    echo "FAIL t50_soa (pass 'AutoSOATransform' reported no changes under --soa)"
    fail=$((fail + 1))
else
    echo "PASS t50_soa [AutoSOATransform --soa changes=$soastats]"
    pass=$((pass + 1))
fi
soaoff=$(timeout 30 $JULESC --stats tests/programs/t50_soa.jules -o "$WORK/soaoff.bin" 2>/dev/null |
         awk -v p="AutoSOATransform" '$2 == p {v += $4} END {print v + 0}')
if [ -n "$soaoff" ] && [ "$soaoff" != "0" ]; then
    echo "FAIL t50_soa (AutoSOATransform fired WITHOUT --soa)"
    fail=$((fail + 1))
else
    echo "PASS t50_soa [AutoSOATransform opt-in honored]"
    pass=$((pass + 1))
fi
timeout 30 "$WORK/soa.bin" > "$WORK/soa.out" 2>&1
if ! diff -q tests/expected/t50_soa.txt "$WORK/soa.out" > /dev/null 2>&1; then
    echo "FAIL t50_soa (--soa build output)"
    fail=$((fail + 1))
else
    echo "PASS t50_soa [--soa build output]"
    pass=$((pass + 1))
fi

# t53 (review-fix round): the hazard allocation (pointer passed as the
# first effect's argument) must be REFUSED while the clean array still
# transforms — activity under --soa = the clean array, output must be
# byte-identical with and without the flag.
t53stats=$(timeout 30 $JULESC --soa --stats tests/programs/t53_soa_call.jules -o "$WORK/t53soa.bin" 2>/dev/null |
           awk -v p="AutoSOATransform" '$2 == p {v += $4} END {print v + 0}')
if [ -z "$t53stats" ] || [ "$t53stats" = "0" ]; then
    echo "FAIL t53_soa_call (pass 'AutoSOATransform' reported no changes under --soa)"
    fail=$((fail + 1))
else
    echo "PASS t53_soa_call [AutoSOATransform --soa changes=$t53stats]"
    pass=$((pass + 1))
fi
timeout 30 "$WORK/t53soa.bin" > "$WORK/t53soa.out" 2>&1
if ! diff -q tests/expected/t53_soa_call.txt "$WORK/t53soa.out" > /dev/null 2>&1; then
    echo "FAIL t53_soa_call (--soa build output)"
    fail=$((fail + 1))
else
    echo "PASS t53_soa_call [--soa hazard refused, clean array transformed]"
    pass=$((pass + 1))
fi
if ! timeout 30 $JULESC tests/programs/t53_soa_call.jules -o "$WORK/t53off.bin" > /dev/null 2>&1; then
    echo "FAIL t53_soa_call (default compile)"
    fail=$((fail + 1))
else
    timeout 30 "$WORK/t53off.bin" > "$WORK/t53off.out" 2>&1
    if ! diff -q tests/expected/t53_soa_call.txt "$WORK/t53off.out" > /dev/null 2>&1; then
        echo "FAIL t53_soa_call (default build output)"
        fail=$((fail + 1))
    else
        echo "PASS t53_soa_call [default build output]"
        pass=$((pass + 1))
    fi
fi

# p72/p73 (detection mode): the use builds of their PGO flows must report
# the detected hoistable/weakenable guard sites.
p72stats=$(timeout 30 $JULESC --pgo=use="$WORK/t48_guardhoist_pgo/jules.prof" --stats tests/programs/t48_guardhoist.jules -o "$WORK/p72.bin" 2>/dev/null |
           awk -v p="GuardHoisting" '$2 == p {v += $4} END {print v + 0}')
if [ -z "$p72stats" ] || [ "$p72stats" = "0" ]; then
    echo "FAIL t48_guardhoist (GuardHoisting detected nothing)"
    fail=$((fail + 1))
else
    echo "PASS t48_guardhoist [GuardHoisting detected=$p72stats]"
    pass=$((pass + 1))
fi
p73stats=$(timeout 30 $JULESC --pgo=use="$WORK/t49_guardweaken_pgo/jules.prof" --stats tests/programs/t49_guardweaken.jules -o "$WORK/p73.bin" 2>/dev/null |
           awk -v p="GuardWeakening" '$2 == p {v += $4} END {print v + 0}')
if [ -z "$p73stats" ] || [ "$p73stats" = "0" ]; then
    echo "FAIL t49_guardweaken (GuardWeakening detected nothing)"
    fail=$((fail + 1))
else
    echo "PASS t49_guardweaken [GuardWeakening detected=$p73stats]"
    pass=$((pass + 1))
fi

# p35 (MaterializationPointInsertion): the JIT-mode deopt manifest carries
# the sunk allocation's materialization recipe.
if [ -d "$WORK/p35run" ]; then rm -rf "$WORK/p35run"; fi
mkdir -p "$WORK/p35run"
p35src="$(pwd)/tests/programs/t46_pea.jules"
p35c="$(cd "$(dirname "$JULESC")" && pwd)/$(basename "$JULESC")"
if (cd "$WORK/p35run" && timeout 30 "$p35c" --mode jit-optimizing "$p35src" -o jit.bin) > /dev/null 2>&1; then
    if grep -q '^materialize ' "$WORK/p35run/jules_deopt_manifest.txt" 2>/dev/null; then
        echo "PASS t46_pea [materialization recipe in the deopt manifest]"
        pass=$((pass + 1))
    else
        echo "FAIL t46_pea (no materialization recipe in the manifest)"
        fail=$((fail + 1))
    fi
else
    echo "FAIL t46_pea (jit-optimizing compile)"
    fail=$((fail + 1))
fi

echo
echo "results: $pass passed, $fail failed"
[ $fail -eq 0 ]
