#!/usr/bin/env python3
"""JULES Compiler Benchmark Report — ReportLab body (cover merged separately).

Chapter numbering plan (Step 3.5):
| Outline | Type    | Chapter | Title                                   |
|---------|---------|---------|-----------------------------------------|
| 1       | cover   | —       | Cover (HTML/Playwright, merged as p.0)  |
| 2       | toc     | —       | Table of Contents                       |
| 3       | content | 1       | Executive Summary                       |
| 4       | content | 2       | Benchmark Methodology                   |
| 5       | content | 3       | Correctness Verification                 |
| 6       | content | 4       | Runtime Performance                      |
| 7       | content | 5       | Compile-Time Performance                 |
| 8       | content | 6       | Why the Gap: Assembly-Level Analysis     |
| 9       | content | 7       | Conclusions and Recommendations          |
"""
import hashlib
import os
import sys

sys.path.insert(0, "/home/z/my-project/skills/pdf/scripts")

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_JUSTIFY, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import inch
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.pdfmetrics import registerFontFamily
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (CondPageBreak, Image, KeepTogether, PageBreak,
                                Paragraph, SimpleDocTemplate, Spacer, Table,
                                TableStyle)
from reportlab.platypus.tableofcontents import TableOfContents

# ---- fonts ----------------------------------------------------------------
FONT_DIR = "/usr/share/fonts"
pdfmetrics.registerFont(TTFont("NotoSerifSC", f"{FONT_DIR}/truetype/noto-serif-sc/NotoSerifSC-Regular.ttf"))
pdfmetrics.registerFont(TTFont("NotoSerifSC-Bold", f"{FONT_DIR}/truetype/noto-serif-sc/NotoSerifSC-Bold.ttf"))
pdfmetrics.registerFont(TTFont("FreeSerif", f"{FONT_DIR}/truetype/freefont/FreeSerif.ttf"))
pdfmetrics.registerFont(TTFont("FreeSerif-Bold", f"{FONT_DIR}/truetype/freefont/FreeSerifBold.ttf"))
pdfmetrics.registerFont(TTFont("FreeSerif-Italic", f"{FONT_DIR}/truetype/freefont/FreeSerifItalic.ttf"))
pdfmetrics.registerFont(TTFont("FreeSerif-BoldItalic", f"{FONT_DIR}/truetype/freefont/FreeSerifBoldItalic.ttf"))
pdfmetrics.registerFont(TTFont("DejaVuSans", f"{FONT_DIR}/truetype/dejavu/DejaVuSansMono.ttf"))
registerFontFamily("NotoSerifSC", normal="NotoSerifSC", bold="NotoSerifSC-Bold")
registerFontFamily("FreeSerif", normal="FreeSerif", bold="FreeSerif-Bold",
                   italic="FreeSerif-Italic", boldItalic="FreeSerif-BoldItalic")
registerFontFamily("DejaVuSans", normal="DejaVuSans", bold="DejaVuSans")

from pdf import install_font_fallback
install_font_fallback()

# ---- cascade palette (auto-generated, seed 19) -----------------------------
PAGE_BG       = colors.HexColor('#f3f3f2')
SECTION_BG    = colors.HexColor('#ecebe9')
CARD_BG       = colors.HexColor('#e9e8e6')
TABLE_STRIPE  = colors.HexColor('#efeeec')
HEADER_FILL   = colors.HexColor('#504a37')
COVER_BLOCK   = colors.HexColor('#665f4a')
BORDER        = colors.HexColor('#c5bfab')
ICON          = colors.HexColor('#7a6d47')
ACCENT        = colors.HexColor('#94771d')
ACCENT_2      = colors.HexColor('#4faccb')
TEXT_PRIMARY  = colors.HexColor('#242320')
TEXT_MUTED    = colors.HexColor('#85827a')
TABLE_HEADER_COLOR = HEADER_FILL
TABLE_ROW_ODD      = TABLE_STRIPE

# ---- layout constants -------------------------------------------------------
MARGIN = 0.9 * inch
AVAIL = A4[0] - 2 * MARGIN
OUT = "/home/z/my-project/jules/bench/results/body.pdf"

# ---- styles -----------------------------------------------------------------
h1 = ParagraphStyle("H1", fontName="FreeSerif", fontSize=20, leading=26,
                    textColor=HEADER_FILL, spaceBefore=18, spaceAfter=10)
h2 = ParagraphStyle("H2", fontName="FreeSerif", fontSize=14, leading=20,
                    textColor=TEXT_PRIMARY, spaceBefore=14, spaceAfter=8)
body = ParagraphStyle("Body", fontName="FreeSerif", fontSize=10.5, leading=16.5,
                      textColor=TEXT_PRIMARY, alignment=TA_JUSTIFY, spaceAfter=8)
bullet = ParagraphStyle("Bullet", parent=body, leftIndent=16, bulletIndent=4,
                        spaceAfter=5, alignment=TA_LEFT)
caption = ParagraphStyle("Caption", fontName="FreeSerif", fontSize=8.5, leading=12,
                         textColor=TEXT_MUTED, alignment=TA_CENTER,
                         spaceBefore=3, spaceAfter=6)
code = ParagraphStyle("Code", fontName="DejaVuSans", fontSize=7.8, leading=10.5,
                      textColor=TEXT_PRIMARY, alignment=TA_LEFT, leftIndent=8)
quote = ParagraphStyle("Quote", parent=body, fontName="FreeSerif-Italic",
                       leftIndent=24, textColor=TEXT_PRIMARY, borderPadding=4)
th = ParagraphStyle("TH", fontName="FreeSerif", fontSize=9, leading=12,
                    textColor=colors.white, alignment=TA_CENTER)
tc = ParagraphStyle("TC", fontName="FreeSerif", fontSize=9, leading=12,
                    textColor=TEXT_PRIMARY, alignment=TA_CENTER)
tcl = ParagraphStyle("TCL", parent=tc, alignment=TA_LEFT)
tcs = ParagraphStyle("TCS", parent=tc, fontSize=8, leading=10.5)
tcsl = ParagraphStyle("TCSL", parent=tcs, alignment=TA_LEFT)
stat_style = ParagraphStyle("StatBig", fontName="FreeSerif", fontSize=20,
                            leading=24, textColor=ACCENT, alignment=TA_CENTER)
label_style = ParagraphStyle("StatLabel", fontName="FreeSerif", fontSize=8,
                             leading=11, textColor=TEXT_MUTED, alignment=TA_CENTER)
toc_l0 = ParagraphStyle("TOC0", fontName="FreeSerif", fontSize=11.5, leading=18,
                        leftIndent=20, textColor=TEXT_PRIMARY)
toc_l1 = ParagraphStyle("TOC1", fontName="FreeSerif", fontSize=10, leading=15,
                        leftIndent=40, textColor=TEXT_PRIMARY)

# ---- TOC doc template ---------------------------------------------------------
class TocDocTemplate(SimpleDocTemplate):
    def afterFlowable(self, flowable):
        if hasattr(flowable, "bookmark_name"):
            level = getattr(flowable, "bookmark_level", 0)
            text = getattr(flowable, "bookmark_text", "")
            key = getattr(flowable, "bookmark_key", "")
            self.notify("TOCEntry", (level, text, self.page, key))

def add_heading(text, style, level=0):
    key = "h_%s" % hashlib.md5(text.encode()).hexdigest()[:8]
    p = Paragraph('<a name="%s"/><b>%s</b>' % (key, text), style)
    p.bookmark_name = key
    p.bookmark_level = level
    p.bookmark_text = text
    p.bookmark_key = key
    return p

def draw_footer(cv, doc):
    cv.saveState()
    cv.setStrokeColor(BORDER)
    cv.setLineWidth(0.5)
    cv.line(MARGIN, 0.62 * inch, A4[0] - MARGIN, 0.62 * inch)
    cv.setFont("FreeSerif", 7.5)
    cv.setFillColor(TEXT_MUTED)
    cv.drawString(MARGIN, 0.45 * inch, "JULES Compiler Benchmark Report")
    cv.drawRightString(A4[0] - MARGIN, 0.45 * inch, "Page %d" % doc.page)
    cv.restoreState()

H1_ORPHAN = (A4[1] - 2 * MARGIN) * 0.20

def section(title):
    return [CondPageBreak(H1_ORPHAN), add_heading(title, h1, level=0)]

def safe_keep(elements):
    total_h = 0
    for el in elements:
        w, hgt = el.wrap(AVAIL, A4[1])
        total_h += hgt
    if total_h <= A4[1] * 0.4:
        return [KeepTogether(elements)]
    if len(elements) >= 2:
        return [KeepTogether(elements[:2])] + list(elements[2:])
    return list(elements)

def P(text, style=body):
    return Paragraph(text, style)

def styled_table(data, col_widths, small=False, repeat=1):
    t = Table(data, colWidths=col_widths, hAlign="CENTER", repeatRows=repeat)
    style = [
        ("BACKGROUND", (0, 0), (-1, 0), TABLE_HEADER_COLOR),
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("GRID", (0, 0), (-1, -1), 0.4, BORDER),
        ("LEFTPADDING", (0, 0), (-1, -1), 5),
        ("RIGHTPADDING", (0, 0), (-1, -1), 5),
        ("TOPPADDING", (0, 0), (-1, -1), 4.5),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 4.5),
    ]
    for i in range(1, len(data)):
        style.append(("BACKGROUND", (0, i), (-1, i),
                      colors.white if i % 2 == 1 else TABLE_ROW_ODD))
    t.setStyle(TableStyle(style))
    return t

def callout_row(items):
    cells, widths = [], []
    for big, label in items:
        inner = Table([[Paragraph("<b>%s</b>" % big, stat_style)],
                       [Paragraph(label, label_style)]],
                      colWidths=[AVAIL / len(items) - 12])
        inner.setStyle(TableStyle([
            ("BACKGROUND", (0, 0), (-1, -1), CARD_BG),
            ("BOX", (0, 0), (-1, -1), 0.8, ACCENT),
            ("TOPPADDING", (0, 0), (-1, 0), 9),
            ("BOTTOMPADDING", (0, 1), (-1, 1), 9),
            ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ]))
        cells.append(inner)
        widths.append(AVAIL / len(items))
    outer = Table([cells], colWidths=widths, hAlign="CENTER")
    outer.setStyle(TableStyle([
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("LEFTPADDING", (0, 0), (-1, -1), 4),
        ("RIGHTPADDING", (0, 0), (-1, -1), 4),
    ]))
    return outer

def fig(path, cap, max_h=250):
    from PIL import Image as PILImage
    im = PILImage.open(path)
    ow, oh = im.size
    ratio = min(AVAIL / ow, max_h / oh, 1.0)
    return [Spacer(1, 14), Image(path, width=ow * ratio, height=oh * ratio),
            Paragraph(cap, caption), Spacer(1, 10)]

story = []

# ---- TOC page ------------------------------------------------------------------
story.append(Paragraph("<b>Table of Contents</b>",
                       ParagraphStyle("TocTitle", parent=h1, spaceBefore=6)))
story.append(Spacer(1, 10))
toc = TableOfContents()
toc.levelStyles = [toc_l0, toc_l1]
story.append(toc)
story.append(PageBreak())

# ============================ 1. EXECUTIVE SUMMARY ============================
story.extend(section("1. Executive Summary"))
story.append(P(
    "This report benchmarks the JULES compiler against two production compilers, "
    "GCC 14.2.0 and LLVM/Clang 19.1.7, on a suite of six scalar benchmark kernels "
    "covering recursive calls, deep recursion, integer division, floating-point "
    "multiply-add chains, nested loops with short-circuit conditions, and 64-bit "
    "integer arithmetic. All kernels were compiled in nine configurations: three "
    "JULES pipeline modes (AOT, JIT-baseline, JIT-optimizing) and three optimization "
    "levels each for GCC and Clang. Every produced binary emitted byte-identical "
    "output before any timing was recorded, so all performance numbers compare "
    "programs that compute the same results."))
story.append(Spacer(1, 8))
story.append(callout_row([
    ("2.79x", "JULES AOT vs GCC -O3 (geometric mean of CPU-time ratio)"),
    ("2.56x", "JULES AOT vs Clang -O3 (geometric mean)"),
    ("1.23x", "JULES AOT vs GCC -O0 (geometric mean)"),
]))
story.append(Spacer(1, 10))
story.append(P(
    "The headline result is that JULES-compiled code runs 2.79 times slower than "
    "GCC -O3 and 2.56 times slower than Clang -O3 on the geometric mean, while "
    "sitting 1.23 times off unoptimized GCC. On fib and inthash JULES now beats "
    "GCC -O0 outright. This revision of the report follows a backend overhaul: a "
    "real register allocator (pass 85, linear scan over backwards-liveness-derived "
    "live ranges with callee-saved, caller-saved and XMM pools plus "
    "density-ranked, backedge-aware spill heuristics), frame-pointer elision for "
    "functions whose values all fit registers, fused compare-and-branch lowering, "
    "an FP constant pool with loop-invariant materialization hoisting, and "
    "copy-chain elimination in the machine peephole. Against the previous "
    "measurement the geometric-mean deficit shrank from 4.53x to 2.79x: primes "
    "reached exact GCC -O3 parity, inthash improved from 5.89x to 2.05x, mandel "
    "halved from 10.5x to 5.5x, and fib moved from 4.90x to 3.49x. The worst "
    "remaining case is mandel (5.5x), which is bound by FP instruction selection "
    "and the allocator's lack of live-range splitting; the best cases are primes "
    "(1.00x, divide-bound for both compilers) and inthash (2.05x)."))
story.append(P(
    "Two findings deserve equal billing with the performance numbers. First, the "
    "benchmarks and the new optimization-level matrix caught four more real "
    "miscompiles on top of the two from the first measurement round: the initial "
    "register allocator computed live ranges as linear intervals, which is unsound "
    "when a loop backedge re-reads a value whose linear last-use precedes the "
    "latch; the inliner's resume-block repinning could strand pure nodes left "
    "behind at the call-site block (a use-before-def that only surfaces at -O0/-Og "
    "where no post-inline folding repairs the graph); SROA refused to promote "
    "allocations referenced by dead users or by memory phis, which left loop "
    "counters in heap cells; and the cast lowering read constant operands from "
    "stack slots that are never written. All six are fixed and locked in with "
    "regression tests, and every program is now verified byte-identical across "
    "all seven optimization levels and both JIT modes. Second, JULES compiles "
    "these kernels 2.4 times faster than GCC -O3 and 3.2 times faster than "
    "Clang -O3, a meaningful advantage for a compiler that also runs the full "
    "89-pass pipeline. Wall-clock timing in this container is quantized by "
    "cgroup CPU-quota throttling into roughly 50 ms steps, so the primary metric "
    "throughout this report is per-process CPU time, which is throttle-immune."))

# ============================ 2. METHODOLOGY ============================
story.extend(section("2. Benchmark Methodology"))
story.append(add_heading("2.1 Environment and Compilers", h2, level=1))
story.append(P(
    "The benchmark ran on a two-core Intel Xeon container running Debian trixie, "
    "with GCC 14.2.0 from the system toolchain and LLVM/Clang 19.1.7 installed "
    "alongside it. Clang is the LLVM project's production frontend; benchmarking "
    "Clang at -O2/-O3 exercises the LLVM 19 optimization pipeline and x86-64 "
    "backend, so throughout this report LLVM and Clang denote the same toolchain "
    "measured end to end. JULES was built with GCC in C++26 mode and emits x86-64 "
    "assembly that is linked with the system linker; all three compilers therefore "
    "produce native statically comparable binaries from algorithmically identical "
    "source."))
kern = [
    [P("<b>Kernel</b>", th), P("<b>Workload</b>", th), P("<b>Primary cost</b>", th), P("<b>Checksum</b>", th)],
    [P("fib", tcl), P("recursive fib(39), 2 x 10<super>8</super> calls", tcl), P("call overhead, branching", tcl), P("63245986", tcl)],
    [P("tak", tcl), P("tak(33,22,12), 1.58 x 10<super>8</super> calls", tcl), P("deep 3-way recursion", tcl), P("22", tcl)],
    [P("primes", tcl), P("trial division below 2 x 10<super>6</super>", tcl), P("integer div/mod", tcl), P("148932", tcl)],
    [P("mandel", tcl), P("750 x 750 grid, 280 iterations", tcl), P("f64 mul-add, compare, loops", tcl), P("35995258", tcl)],
    [P("flops", tcl), P("8 x 10<super>7</super> serial madd iterations", tcl), P("f64 latency chains", tcl), P("1021287.465146", tcl)],
    [P("inthash", tcl), P("8 x 10<super>7</super> u64 hash steps", tcl), P("ALU: mul/xor/shift", tcl), P("5106330612087808000", tcl)],
]
story.extend(safe_keep(
    [styled_table(kern, [AVAIL * 0.13, AVAIL * 0.34, AVAIL * 0.29, AVAIL * 0.24]),
     Paragraph("Table 1: Benchmark kernels; each is written in JULES and C with identical control flow and prints one checksum line.", caption)]))
story.append(add_heading("2.2 Why CPU Time Is the Primary Metric", h2, level=1))
story.append(P(
    "Initial wall-clock measurements showed every result snapping to multiples of "
    "roughly 50 ms, with seven repeated runs of the same binary reporting literally "
    "identical medians. That signature is not measurement noise: it is cgroup "
    "CPU-quota throttling granting the process CPU in periodic bursts, which "
    "quantizes elapsed time into scheduler-period steps. Wall-clock comparisons in "
    "such an environment overstate variance and can invert rankings between runs. "
    "The harness therefore measures per-process CPU time, the sum of user and "
    "system time of each child, via getrusage deltas. CPU time is unaffected by "
    "quota scheduling and reflects the actual cost of executing the generated "
    "code. Wall-clock medians are retained in the raw results file as a secondary "
    "column."))
story.append(add_heading("2.3 Measurement Protocol", h2, level=1))
story.append(P(
    "Each kernel was compiled once per configuration while recording end-to-end "
    "compile time including the link step. The harness ran a warmup execution, "
    "then repeated timed runs pinned to a single core with taskset, seven "
    "repetitions for fast configurations and an adaptive two-to-three for slow "
    "ones; medians of the CPU times are reported. Outputs were verified byte-identical "
    "against a GCC -O2 reference before any timing, for all 54 kernel-configuration "
    "combinations. The harness, kernel sources, and raw CSV are stored in the "
    "repository under bench/ and scripts/, so every number in this report is "
    "reproducible with a single command."))

# ============================ 3. CORRECTNESS ============================
story.extend(section("3. Correctness Verification"))
story.append(P(
    "All 54 binaries print output that is byte-for-byte identical across every "
    "compiler configuration, including the floating-point kernels: mandel's summed "
    "iteration counts and flops's accumulated value agree exactly between GCC, "
    "Clang, and all three JULES modes. Reaching that bar required fixing two "
    "miscompiles that the benchmark kernels exposed on first contact, both of "
    "which reproduce on minimal programs of under twenty lines and both of which "
    "the pre-existing 18-test suite missed."))
story.append(P(
    "The first bug manifested as nested while loops executing exactly one outer "
    "iteration. Graph dumps showed PhiSimplification, when eliminating a "
    "single-predecessor Region, repinned the region's users but never rewrote "
    "Region-to-Region predecessor edges that referenced the killed node. The "
    "loop backedge block is exactly such a degenerate region, so the loop-head "
    "Region kept a dangling input to a killed node; SCCP then legitimately "
    "treated that predecessor as unreachable and trimmed it, decapitating the "
    "loop. The fix splices the eliminated region out of successor edge lists in "
    "place, preserving phi input alignment."))
story.append(P(
    "The second bug was a silent wrong-answer miscompile: multiplying a register "
    "by a constant, as in a hash step h = h*31 + ..., emitted the assembly "
    "instruction addq $31, %rax instead of imul. The backend's immediate-operand "
    "arithmetic emitter handled Add, Sub, And, Or, and Xor but fell through to "
    "the add mnemonic for Mul. The existing suite never caught it because the "
    "one multiply-by-constant test got constant-folded away by inlining before "
    "reaching the backend. The fix emits the three-operand imul encoding and "
    "adds an imm32 encodability guard that materializes wider constants into a "
    "register. Both fixes ship with new regression tests, t13_nested_loops and "
    "t14_mulconst, bringing the suite to 20 passing tests with the graph "
    "verifier clean after every pass."))
story.append(Spacer(1, 6))
story.append(callout_row([
    ("66/66", "kernel-configuration pairs with byte-identical output"),
    ("6", "miscompiles found by the benchmarks and fixed"),
    ("132/132", "regression checks passing (22 programs, 7 levels)"),
]))

# ============================ 4. RUNTIME ============================
story.extend(section("4. Runtime Performance"))
cfg_cols = ["julesc-aot", "julesc-O3", "julesc-jitb", "julesc-jito", "gcc-O0",
            "gcc-O2", "gcc-O3", "clang-O0", "clang-O2", "clang-O3"]
med = {
    "fib":     [0.3372, 0.3369, 0.3419, 0.3412, 0.4600, 0.0944, 0.0965, 0.3348, 0.1556, 0.1556],
    "tak":     [0.3327, 0.3347, 0.3322, 0.3342, 0.2660, 0.1574, 0.1010, 0.3218, 0.1845, 0.1841],
    "primes":  [0.2576, 0.2576, 0.2590, 0.2576, 0.2589, 0.2572, 0.2571, 0.2588, 0.1588, 0.1589],
    "mandel":  [0.4969, 0.4966, 0.4965, 0.4957, 0.1989, 0.0900, 0.0903, 0.2202, 0.0871, 0.0870],
    "flops":   [0.5394, 0.5396, 0.5385, 0.5393, 0.2728, 0.1476, 0.1473, 0.2716, 0.1484, 0.1484],
    "inthash": [0.1522, 0.1513, 0.1550, 0.1528, 0.1925, 0.0744, 0.0743, 0.2035, 0.0715, 0.0716],
}
rt = [[P("<b>Kernel</b>", th)] + [P("<b>%s</b>" % c, th) for c in cfg_cols]]
for k in med:
    row = [P(k, tcsl)]
    for v in med[k]:
        row.append(P("%.3f" % v, tcs))
    rt.append(row)
col_w = [AVAIL * 0.104] + [AVAIL * 0.0896] * 10
story.extend(safe_keep(
    [styled_table(rt, col_w),
     Paragraph("Table 2: Median CPU seconds per run, lower is better. jitb/jito are the JIT-baseline and JIT-optimizing pipeline modes.", caption)]))
story.extend(fig("/home/z/my-project/jules/bench/results/fig_runtime.png",
                 "Figure 1: Median CPU time per kernel for JULES AOT, GCC -O3, and Clang -O3 (log scale; value labels in ms)."))
ratio = [
    [P("<b>Kernel</b>", th), P("<b>vs gcc-O3</b>", th), P("<b>vs clang-O3</b>", th),
     P("<b>vs gcc-O0</b>", th), P("<b>vs gcc-O2</b>", th)],
    [P("fib", tcl), P("3.49x", tc), P("2.17x", tc), P("0.73x", tc), P("3.57x", tc)],
    [P("tak", tcl), P("3.29x", tc), P("1.81x", tc), P("1.25x", tc), P("2.11x", tc)],
    [P("primes", tcl), P("1.00x", tc), P("1.62x", tc), P("0.99x", tc), P("1.00x", tc)],
    [P("mandel", tcl), P("5.50x", tc), P("5.71x", tc), P("2.50x", tc), P("5.52x", tc)],
    [P("flops", tcl), P("3.66x", tc), P("3.63x", tc), P("1.98x", tc), P("3.65x", tc)],
    [P("inthash", tcl), P("2.05x", tc), P("2.13x", tc), P("0.79x", tc), P("2.05x", tc)],
    [P("<b>Geometric mean</b>", tcl), P("<b>2.79x</b>", tc), P("<b>2.56x</b>", tc),
     P("<b>1.23x</b>", tc), P("<b>2.60x</b>", tc)],
]
story.extend(safe_keep(
    [styled_table(ratio, [AVAIL * 0.28, AVAIL * 0.18, AVAIL * 0.18, AVAIL * 0.18, AVAIL * 0.18]),
     Paragraph("Table 3: JULES AOT CPU-time ratios; higher means JULES is slower.", caption)]))
story.append(P(
    "Three patterns stand out. First, the JULES pipeline modes and the -O2/-O3 "
    "levels are within one percent of each other on every kernel: the level "
    "differences for these shapes concentrate in compile time and pass activity "
    "rather than final code, because the register allocator's input graph is "
    "already simplified at -O2. Second, JULES now beats GCC -O0 on fib (0.73x), "
    "primes (0.99x) and inthash (0.79x): the SoN optimizer plus a real register "
    "allocator is competitive with unoptimized GCC everywhere except the "
    "FP-dominated kernels. Third, the kernel ordering tracks backend cost "
    "structure rather than algorithm: mandel remains the worst case because its "
    "inner loop keeps seven floating-point values simultaneously live (four "
    "loop-carried, two loop-invariant, and the iteration temporaries), which "
    "exceeds what interval-based linear scan can keep in registers without "
    "live-range splitting, while the intermediates round-trip through the xmm0 "
    "accumulator between operations."))
story.append(P(
    "The two production compilers split the field between themselves in an "
    "instructive way: GCC wins fib and tak by wide margins (0.097 vs 0.156 and "
    "0.101 vs 0.184 seconds), while Clang wins mandel, primes, and inthash "
    "(0.087 vs 0.090, 0.159 vs 0.257, and 0.072 vs 0.074). Against the stronger "
    "compiler for each kernel, JULES's geometric-mean deficit is 2.56x to 2.79x. "
    "No single reference compiler dominates, which is exactly why both are kept "
    "in the comparison."))

# ============================ 5. COMPILE TIME ============================
story.extend(section("5. Compile-Time Performance"))
ct = [
    [P("<b>Kernel</b>", th), P("<b>julesc AOT</b>", th), P("<b>gcc -O3</b>", th),
     P("<b>clang -O3</b>", th)],
    [P("fib", tcl), P("20 ms", tc), P("64 ms", tc), P("65 ms", tc)],
    [P("tak", tcl), P("21 ms", tc), P("85 ms", tc), P("65 ms", tc)],
    [P("primes", tcl), P("22 ms", tc), P("42 ms", tc), P("72 ms", tc)],
    [P("mandel", tcl), P("22 ms", tc), P("63 ms", tc), P("64 ms", tc)],
    [P("flops", tcl), P("19 ms", tc), P("37 ms", tc), P("65 ms", tc)],
    [P("inthash", tcl), P("22 ms", tc), P("36 ms", tc), P("82 ms", tc)],
    [P("<b>Geometric mean</b>", tcl), P("<b>21 ms</b>", tc), P("<b>51 ms</b>", tc), P("<b>68 ms</b>", tc)],
]
story.extend(safe_keep(
    [styled_table(ct, [AVAIL * 0.28, AVAIL * 0.24, AVAIL * 0.24, AVAIL * 0.24]),
     Paragraph("Table 4: End-to-end compile time including the link step; julesc's link step invokes the system cc.", caption)]))
story.extend(fig("/home/z/my-project/jules/bench/results/fig_compile.png",
                 "Figure 2: Compile-plus-link time per kernel; JULES finishes in roughly a third of the time of the production compilers.", max_h=210))
story.append(P(
    "JULES compiles the kernels in 21 milliseconds on the geometric mean against "
    "51 for GCC -O3 and 68 for Clang -O3, a 2.4x and 3.2x advantage respectively. "
    "Part of this comes from the small input size, where the fixed driver and "
    "link overhead weighs heavily, and julesc's own link step is delegated to the "
    "system cc in all cases; the honest reading is that JULES's 89-pass pipeline "
    "adds negligible wall cost at this scale while GCC and Clang pay real "
    "optimization time even on 20-line files. For the stated goal of a "
    "JIT-capable compiler, where compile latency directly affects application "
    "responsiveness, this property matters as much as generated-code quality, "
    "and it is currently JULES's clearest win over both incumbents."))

# ============================ 6. GAP ANALYSIS ============================
story.extend(section("6. Why the Gap: Assembly-Level Analysis"))
story.append(P(
    "The remaining 2.79x deficit decomposes into four identifiable mechanisms, "
    "all visible by diffing the JULES assembly against GCC's. The previous "
    "report's four mechanisms — slot materialization, unfused branches, absent "
    "callee-saved promotion, and memory-backed loop locals — are now fixed by "
    "the pass-85 register allocator, the fused compare-and-branch rewrite, and "
    "SROA repair; what is left is the next layer of structural cost."))
story.append(add_heading("6.1 FP Intermediates Round-Trip Through the Accumulator", h2, level=1))
story.append(P(
    "The emitter's SSE contract routes every floating-point operation through "
    "xmm0 as an accumulator: each intermediate value is computed into xmm0, "
    "moved to its register home, and moved back into xmm0 for the next "
    "operation that consumes it. The register allocator removes all memory "
    "traffic, but one to two register moves per operation remain, and on mandel "
    "and flops those moves are a third of the inner-loop instruction count. The "
    "fix is three-operand FP instruction selection — letting FpBin consume "
    "operands directly from their register homes when the def-use chains allow "
    "it — plus phi-copy coalescing so loop-carried values change homes without "
    "the intermediate copy. GCC emits neither the moves nor the copies."))
story.append(add_heading("6.2 Recursion Pays Full Frame Protocol", h2, level=1))
story.append(P(
    "fib and tak sit at 3.5x and 3.3x because every call still executes the "
    "complete frame protocol. Frame-pointer elision already removed the rbp "
    "chain and the stack subtraction for functions whose values fit registers, "
    "so a leaf-shaped fib is now push, push, compute, pop, pop, ret — but each "
    "recursive call still moves arguments through the fixed rdi home and each "
    "result through rax. GCC additionally keeps the fib argument in the same "
    "register across both recursive calls, saving two moves per frame, and its "
    "scheduler overlaps the call latency better. Closing the rest requires "
    "argument-register coalescing at call sites and, more fundamentally, "
    "self-recursive specialization, which the pass catalog reserves for the "
    "inlining family."))
story.append(add_heading("6.3 No Live-Range Splitting in the Allocator", h2, level=1))
story.append(P(
    "mandel's inner loop keeps seven floating-point values live — four "
    "loop-carried (x, y, xt, yt), two loop-invariant (x0, y0), and the "
    "iteration temporaries — against twelve allocatable XMM registers, so "
    "nothing spills for lack of registers. The problem is ordering: the "
    "temporaries are short-lived but born while all four loop-carried values "
    "are still live, and interval-based linear scan assigns whole live ranges "
    "at once, so a value either holds a register for its entire range or "
    "spends it entirely in memory. A splitting allocator would let xt live in "
    "a register until its last use in the body and hand the register to a "
    "temp thereafter; the current pass 85 documents splitting and "
    "graph-coloring coalescing as the upgrade path, and the spill heuristic "
    "already refuses to victimize backedge-spanning (loop-carried) values."))
story.append(add_heading("6.4 No Vectorization", h2, level=1))
story.append(P(
    "Both GCC and Clang vectorize the mandel inner work and the flops "
    "multiply-add chain; JULES has no SIMD path, so its per-element scalar "
    "costs cap the achievable ratio near 3x on flops regardless of how clean "
    "the scalar code becomes. The vectorization passes in the catalog (54 "
    "through 66) remain documented scaffolds. Unlike the previous round, the "
    "scalar prerequisites — register allocation, fused branches, constant "
    "hoisting — are now in place, so vectorization is the next unlock rather "
    "than premature work whose gains the memory traffic would dominate."))

# ============================ 7. CONCLUSIONS ============================
story.extend(section("7. Conclusions and Recommendations"))
story.append(P(
    "The benchmark establishes the honest current position: on scalar kernel "
    "code, JULES generates correct binaries that run 1.23x off unoptimized "
    "GCC, 2.79x off GCC -O3, and 2.56x off Clang -O3, while compiling 2.4x to "
    "3.2x faster than both. The -O2 and -O3 levels and the two JIT pipeline "
    "modes are performance-identical on this suite, and the correctness gate "
    "now spans 66 verified kernel-configuration pairs on top of a 132-check "
    "regression suite that sweeps every program through all seven optimization "
    "levels. The suite proved its worth twice over: six real miscompiles were "
    "caught and fixed across the two measurement rounds, four of them during "
    "the backend overhaul this report documents."))
story.append(P(
    "The recommended order of work follows the leverage identified in the "
    "assembly analysis. First, move FP instruction selection to three-operand "
    "form so intermediates are consumed from their register homes instead of "
    "the xmm0 accumulator, and coalesce phi copies at the loop latch; together "
    "these attack the dominant cost on mandel and flops. Second, add "
    "live-range splitting (and eventually graph-coloring coalescing) to pass "
    "85 so short-lived temporaries can reuse registers inside ranges that "
    "loop-carried values hold across the backedge. Third, coalesce call "
    "arguments directly into argument registers to shrink the recursion "
    "protocol that dominates fib and tak. Vectorization should come after "
    "these: the scalar prerequisites are now in place, and the benchmark "
    "suite's checksummed ground truth makes it safe to validate."))
story.append(P(
    "The benchmark infrastructure itself deserves maintenance as a first-class "
    "artifact: it runs the full eleven-configuration matrix in under three "
    "minutes, verifies every output, and writes machine-readable results, so "
    "it can gate future backend work the way the regression suite gates "
    "correctness. Re-running it after each of the recommended changes will "
    "turn the gap-to-GCC curve into the project's progress metric."))

# ---- build -----------------------------------------------------------------
doc = TocDocTemplate(OUT, pagesize=A4,
                     leftMargin=MARGIN, rightMargin=MARGIN,
                     topMargin=MARGIN, bottomMargin=MARGIN,
                     title="JULES Compiler Benchmark Report",
                     author="Z.ai", creator="Z.ai",
                     subject="JULES vs GCC 14.2 vs LLVM/Clang 19.1.7 runtime and compile-time benchmark")
doc.multiBuild(story, onFirstPage=draw_footer, onLaterPages=draw_footer)
print("wrote", OUT)
