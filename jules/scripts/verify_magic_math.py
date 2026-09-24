#!/usr/bin/env python3
"""Verify the exact-division math before it goes into C++ (Task 23).

Checks, over exhaustive edges + randomized samples + every d in a sweep:
  1. UNSIGNED magic division (W=64): find (M, s, form) per divisor d,
     prove q == x // d for all sampled x.
       Form A (fits):    q = (mulhi(x, M) >> s)            with M < 2^64
       Form B (no-fit):  q = (mulhi(x, M') + x) >> s        with M' = M - 2^64,
                         justified by: mulhi(x, 2^64 + M') == x + mulhi(x, M')
  2. SIGNED division via abs-wrapper: t = (x ^ s) - s (u64), q_u = magic(t, |d|),
     q = (q_u ^ s) - s, then negate if d < 0.
  3. MOD via recombination: r = x - q * d (mod 2^64 arithmetic).
  4. POW2 identities (IR-level, p12):
       unsigned: x / 2^k  == x >> k
                 x % 2^k  == x & (2^k - 1)
       signed:   x / 2^k  == (x + ((x >> 63) & (2^k - 1))) >>a k
                 x % 2^k  == x - ((x / 2^k) << k)      [composed from the q above]
  5. Emits the edge-case table used to generate tests/expected/t_magic_div.txt.

Exit code 0 only if every check passes.
"""
import random
import sys

M64 = (1 << 64) - 1
SIGN = 1 << 63


def mulhi(a, b):
    return ((a * b) >> 64) & M64


def derive_unsigned(d, W=64):
    """Granlund-Montgomery unsigned magic for width W. Returns (M, s, form):
      form 'A' (M < 2^W):   q = mulhi(x, M) >> s
      form 'B' (M >= 2^W):   t = mulhi(x, M - 2^W)
                             q = (t + ((x - t) >> 1)) >> (s - 1)

    Theorem applied (Hacker's Delight 10-9 / GM'94):
      M = ceil(2^(W+s) / d), e = M*d - 2^(W+s) (0 <= e < d).
      Form A valid iff M < 2^W and e <= 2^s.
      Form B (M >= 2^W, which happens iff 2^s >= d) computes
      floor(x*M / 2^(W+s)) exactly: with t = mulhi(x, M - 2^W),
      floor(x*M / 2^W) = x + t (unbounded integer), and
          (x + t) / 2^s == (t + floor((x - t)/2)) / 2^(s-1)
      with 0 <= t <= x - 1 (since M - 2^W < 2^W), so every step fits in
      W-bit machine arithmetic without wrap.
    Cap: s <= W - 1 (shift counts must fit the encoding); divisors that
    would need s = W (only d within 2 of 2^W) are rejected -> idiv fallback.
    """
    two_w = 1 << W
    for s in range(0, W):
        num = ((1 << (W + s)) + d - 1) // d   # M = ceil(2^(W+s) / d)
        e = num * d - (1 << (W + s))
        if num < two_w:
            if e <= (1 << s):
                return num, s, 'A'
        else:
            # M >= 2^W  <=>  2^s >= d (s >= 1 guaranteed for d >= 2)
            return num - two_w, s, 'B'
    return None


def umagic_div(x, d, derived):
    M, s, form = derived
    if form == 'A':
        t = mulhi(x, M)
        return (t >> s) & M64
    t = mulhi(x, M)                     # M here is M' = M_true - 2^64
    q = (t + ((x - t) >> 1)) & M64      # no wrap: 0 <= t <= x - 1
    return (q >> (s - 1)) & M64


def check_unsigned(d, W=64):
    der = derive_unsigned(d, W)
    if der is None:
        # rejected only for giants (d > 2^62-ish): impl keeps idiv there
        return d > (1 << 62)
    xs = [0, 1, d - 1, d, d + 1, 2 * d, 2 * d - 1, 2 * d + 1,
          M64, M64 - 1, M64 - d, SIGN, SIGN - 1, SIGN + 1]
    # boundary sweep: multiples of d (+-1) up to 1000*d, and 2^k*d (+-1)
    for k in range(1, 1001):
        xs += [k * d - 1, k * d, k * d + 1]
    for k in range(0, 64):
        b = (1 << k) * d
        if b <= M64:
            xs += [b - 1, b, b + 1, min(2 * b - 1, M64)]
    xs += [random.getrandbits(64) for _ in range(4000)]
    for x in xs:
        x &= M64
        got = umagic_div(x, d, der)
        want = x // d
        if got != want:
            print(f"UNSIGNED FAIL d={d} x={x}: got {got} want {want} "
                  f"(M={der[0]:#x} s={der[1]} form={der[2]})")
            return False
    return True


def smagic_div(x, d):
    """Signed trunc division via abs-wrapper. d != 0, |d| != 1, x full i64.
    The sign mask is explicit (Python ints are unbounded; only an in-range
    i64 x would give x >> 63 == 0 / -1 by itself)."""
    s = M64 if x < 0 else 0          # 0 or 0xFFFF...F
    t = ((x ^ s) - s) & M64        # |x| as u64 (INT64_MIN -> 2^63)
    ad = abs(d)
    der = derive_unsigned(ad, 64)
    if der is None:
        return True, None          # giant divisor: impl keeps idiv
    qu = umagic_div(t, ad, der)
    q = ((qu ^ s) - s) & M64       # apply x's sign
    q = q - (1 << 64) if q >= SIGN else q   # to signed
    if d < 0:
        q = (-q) & M64
        q = q - (1 << 64) if q >= SIGN else q
    want = abs(x) // abs(d) * (-1 if (x < 0) != (d < 0) else 1)
    return (q == want), q


def check_signed(d):
    I64_MIN, I64_MAX = -(1 << 63), (1 << 63) - 1
    xs = [0, 1, -1, d - 1, d + 1, -d, -d - 1, 2 * d, -2 * d,
          I64_MAX, I64_MIN, I64_MIN + 1, I64_MAX - 1]
    xs = [x for x in xs if I64_MIN <= x <= I64_MAX]  # stay in i64 domain
    xs += [random.randint(I64_MIN, I64_MAX) for _ in range(3000)]
    for x in xs:
        ok, q = smagic_div(x, d)
        if not ok:
            print(f"SIGNED FAIL d={d} x={x}")
            return False
        if q is None:
            continue
        # mod recombination: r = x - q*d  (mod-2^64 wrap arithmetic is exact)
        r = (x - q * d) & M64
        r = r - (1 << 64) if r >= SIGN else r
        want_r = x - (abs(x) // abs(d)) * d * (-1 if (x < 0) != (d < 0) else 1)
        if r != want_r:
            print(f"MOD FAIL d={d} x={x}: got {r} want {want_r}")
            return False
    return True


def check_pow2_identities():
    for k in range(1, 63):
        m = (1 << k) - 1
        xs = [0, 1, -1, 2, -2, m, m + 1, -m, -(m + 1), -SIGN, SIGN - 1]
        xs += [random.randint(-(1 << 63), (1 << 63) - 1) for _ in range(3000)]
        for x in xs:
            s = -1 if x < 0 else 0
            # div: q = (x + (s & m)) >>a k
            q = (x + (s & m)) >> k
            want = abs(x) // (1 << k) * (-1 if x < 0 else 1)
            want = -(-x // (1 << k)) if x < 0 else x // (1 << k)
            if q != want:
                print(f"POW2 DIV FAIL k={k} x={x}: {q} != {want}")
                return False
            # mod: r = x - (q << k)
            r = x - (q << k)
            if r != x - want * (1 << k):
                print(f"POW2 MOD FAIL k={k} x={x}")
                return False
            # unsigned
            xu = x & M64
            if xu >> k != xu // (1 << k):
                print(f"POW2 UDIV FAIL k={k}")
                return False
            if xu & m != xu % (1 << k):
                print(f"POW2 UMOD FAIL k={k}")
                return False
    return True


def main():
    random.seed(20260924)
    ok = True
    # unsigned divisor sweep: small, odd, even, near 2^63, near 2^64
    divisors = [3, 5, 6, 7, 9, 10, 11, 12, 13, 100, 255, 256, 257, 997,
                1000, 1009, 4095, 4097, 65535, 65537, 1000003, 1000000007,
                1 << 31, (1 << 31) - 1, (1 << 32) - 1, (1 << 32) + 1,
                (1 << 63) - 1, 1 << 63, (1 << 63) + 1, M64, M64 - 1,
                2147483647, 4294967291, 9223372036854775783]
    divisors += [random.getrandbits(64) | 1 for _ in range(40)]
    divisors += [random.getrandbits(64) for _ in range(40)]
    for d in divisors:
        if d < 3:
            continue
        if not check_unsigned(d):
            ok = False
    print(f"unsigned: {'OK' if ok else 'FAILED'} ({len(divisors)} divisors, "
          f"~2000 samples each)")

    sdivisors = [3, 5, 7, 9, 10, 11, 13, -3, -5, -7, -9, -10, -13, 100, -100,
                1000, -1000, 1000000007, -(1 << 31), (1 << 31) - 1, (1 << 31),
                 (1 << 62), (1 << 62) + 1, (1 << 63) - 1, -((1 << 62) + 1)]
    sdivisors += [random.randint(2, (1 << 63) - 1) for _ in range(30)]
    sdivisors += [-random.randint(2, (1 << 63) - 1) for _ in range(30)]
    for d in sdivisors:
        if not check_signed(d):
            ok = False
    print(f"signed+mod: {'OK' if ok else 'FAILED'} ({len(sdivisors)} divisors)")

    if not check_pow2_identities():
        ok = False
    print("pow2 identities: OK" if ok else "pow2 identities: check output")

    print("ALL CHECKS PASSED" if ok else "FAILURES DETECTED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
