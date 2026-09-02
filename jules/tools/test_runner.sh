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

# pass-activity assertion: `changes` column of --stats must be > 0
assert_pass_active() {
    local name=$1
    local pass_name=$2
    local stats
    stats=$(timeout 30 $JULESC --stats "tests/programs/${name}.jules" -o "$WORK/a.bin" 2>/dev/null |
            awk -v p="$pass_name" '$2 == p {v=$4} END {print v}')
    if [ -z "$stats" ] || [ "$stats" = "0" ]; then
        echo "FAIL $name (pass '$pass_name' reported no changes)"
        fail=$((fail + 1))
        return 1
    fi
    echo "PASS $name [$pass_name changes=$stats]"
    pass=$((pass + 1))
}

for t in tests/programs/*.jules; do
    run_test "$(basename "$t" .jules)"
done

# Optimizer self-verification: the pass must have actually transformed.
assert_pass_active t02_gvn GlobalValueNumbering
assert_pass_active t03_sccp SparseConditionalConstantPropagation
assert_pass_active t04_sroa ScalarReplacementOfAggregates
assert_pass_active t06_inline CostBasedInlining
assert_pass_active t08_tco TailRecursionElimination
assert_pass_active t09_licm LoopInvariantCodeMotion

echo
echo "results: $pass passed, $fail failed"
[ $fail -eq 0 ]
