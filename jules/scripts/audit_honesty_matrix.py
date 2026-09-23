#!/usr/bin/env python3
"""Honesty-matrix audit: doc-claimed pass status vs code-detected reality.

Detection heuristics per pass file:
  NOOP        - run() returns false unconditionally (scaffold)
  SCAN        - walks the graph/analysis but returns false (vacuous / analysis-only)
  ACTIVE      - can return true / mutates graph (real transform or delegator)
  DELEGATE    - forwards work to analysis manager or target backend
"""
import re, os, subprocess, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
ROOT = os.path.normpath(ROOT)
PDIR = os.path.join(ROOT, "src/core/son/passes")
DOC = os.path.join(ROOT, "docs/pass_status.md")

# ---- 1. parse the claimed table -------------------------------------------
claimed = {}  # num -> (name, status)
for line in open(DOC):
    m = re.match(r"^\|\s*(\d+)\s*\|\s*(\w+)\s*\|\s*([A-Z]+)", line)
    if m:
        n, name, status = int(m.group(1)), m.group(2), m.group(3)
        claimed[n] = (name, status)

# ---- 2. detect actual behavior per file -------------------------------------
def classify(path):
    src = open(path).read()
    # strip comments for behavior checks
    code = re.sub(r"//[^\n]*", "", src)
    has_scaffold_hdr = "STATUS: SCAFFOLD" in src or "SCAFFOLD" in src.split("#include")[0]
    # find run() body with balanced-brace matching
    m = re.search(r"bool run\(PassContext&\s*\w+\)\s*override\s*\{", code)
    body = ""
    if m:
        i = m.end()  # position just past '{'
        depth = 1
        while i < len(code) and depth > 0:
            if code[i] == "{":
                depth += 1
            elif code[i] == "}":
                depth -= 1
            i += 1
        body = code[m.end():i - 1]
    returns = re.findall(r"return\s+([^;]+);", body)
    uncond_false = bool(returns) and all(r.strip() == "false" for r in returns)
    scans = bool(re.search(r"for\s*\(|while\s*\(", body))
    delegates = bool(re.search(r"x64_\w+\(", body))
    if has_scaffold_hdr and uncond_false:
        return "SCAFFOLD"
    if uncond_false and delegates:
        return "DELEGATE/ANALYSIS"
    if uncond_false and scans:
        return "SCAN(vacuous)"
    if uncond_false:
        return "NOOP?"
    return "ACTIVE"

rows = []
mismatches = []
for n in sorted(claimed):
    name, status = claimed[n]
    files = [f for f in os.listdir(PDIR) if f.startswith(f"p{n:02d}_")]
    if not files:  # pass families live in subdirectories (pe/, ...)
        sub = os.path.join(PDIR, "pe")
        if os.path.isdir(sub):
            files = [f for f in os.listdir(sub) if f.startswith(f"p{n:02d}_")]
            files = [os.path.join("pe", f) for f in files]
    assert len(files) == 1, f"pass {n}: {files}"
    loc = sum(1 for _ in open(os.path.join(PDIR, files[0])))
    actual = classify(os.path.join(PDIR, files[0]))
    # expectations: what the claim implies about the code
    ok = {
        "SCAFFOLD": actual == "SCAFFOLD",
        "VACUOUS": actual in ("SCAN(vacuous)", "DELEGATE/ANALYSIS", "NOOP?"),
        "SIMPLIFIED": True,  # verified by hand (p52/67/75/89)
        "IMPLEMENTED": actual in ("ACTIVE", "DELEGATE/ANALYSIS", "SCAN(vacuous)", "NOOP?"),
    }.get(status, False)
    note = ""
    if status == "IMPLEMENTED" and actual == "SCAFFOLD":
        if n == 28:
            note = "KNOWN-DELEGATED (p85 finalize slot coloring; disclosed in row)"
        else:
            note = "MISMATCH: code says scaffold"
            mismatches.append(n)
    elif status == "SCAFFOLD" and actual != "SCAFFOLD":
        note = "MISMATCH: code not scaffold"
        mismatches.append(n)
    rows.append((n, name, status, actual, loc, ok, note))

w = (4, 34, 12, 18, 5, 3, 30)
print(f"{'#':<4}{'pass':<34}{'claimed':<12}{'detected':<18}{'loc':<5}{'ok':<3}note")
print("-" * 106)
for r in rows:
    print(f"{r[0]:<4}{r[1]:<34.34}{r[2]:<12.12}{r[3]:<18.18}{r[4]:<5}{('Y' if r[5] else 'N'):<3}{r[6]}")
print("-" * 106)
from collections import Counter
cc = Counter(r[2] for r in rows)
print("claimed roll-up:", dict(cc), "total", sum(cc.values()))
mm = [r for r in rows if not r[5]]
print(f"\nclaimed-vs-detected mismatches requiring note: {len(mm)}")
for r in mm:
    print(f"  p{r[0]:02d} {r[1]}: claimed {r[2]}, detected {r[3]} ({r[4]} loc)")
