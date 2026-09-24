#!/usr/bin/env python3
"""Expected-output oracle for tests/programs/t_magic_div.jules (Task 23).

Mirrors the JULES program exactly, with C truncation semantics for signed
division (and Python floor division carefully avoided via int(x/d) math).
"""
M64 = (1 << 64) - 1


def sdiv(x, d):
    q = abs(x) // abs(d)
    return q if (x < 0) == (d < 0) else -q


def smod(x, d):
    return x - sdiv(x, d) * d


acc = 0
for i in range(4):
    x = i * 111111111 - 333333333
    acc += sdiv(x, 7)
    acc += sdiv(abs(x), 7)  # cf_helper: |x| then /7
    acc += sdiv(x, 1000000007) + smod(x, 1000000007)
    acc += sdiv(x, -7) + smod(x, -7)

big = (1 << 64) - 1
big2 = 1 << 63
acc += big // 10 + big % 10
acc += big2 // 3 + big2 % 3
acc += big // 1000000007 + big % 1000000007

acc += 100 // 8 + sdiv(-100, 8) + smod(-100, 8) + 100 % 8
acc += sdiv(-101, 32) + smod(-101, 32)

a = -(1 << 31)
acc += sdiv(a, 7) + smod(a, 7) + sdiv(a, 3) + smod(a, 3)
b = (1 << 32) - 1
acc += b // 10 + b % 10

# wrap to i64 printing semantics
print(acc & M64 if acc < 0 or acc > M64 else acc)
