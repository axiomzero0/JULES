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
assert_pass_active t30_storemerge StoreMerging

echo
echo "results: $pass passed, $fail failed"
[ $fail -eq 0 ]
