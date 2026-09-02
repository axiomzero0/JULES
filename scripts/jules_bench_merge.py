#!/usr/bin/env python3
"""Merge cover + body into the final benchmark report PDF."""
from pypdf import PdfReader, PdfWriter

A4_W, A4_H = 595.28, 841.89

def normalize(page):
    box = page.mediabox
    w, h = float(box.width), float(box.height)
    if abs(w - A4_W) > 0.1 or abs(h - A4_H) > 0.1:
        page.scale_to(A4_W, A4_H)
    return page

writer = PdfWriter()
writer.add_page(normalize(PdfReader("/home/z/my-project/jules/bench/results/cover.pdf").pages[0]))
for page in PdfReader("/home/z/my-project/jules/bench/results/body.pdf").pages:
    writer.add_page(normalize(page))
writer.add_metadata({
    "/Title": "JULES Compiler Benchmark Report",
    "/Author": "Z.ai",
    "/Creator": "Z.ai",
    "/Subject": "JULES vs GCC 14.2 vs LLVM/Clang 19.1.7 runtime and compile-time benchmark",
})
out = "/home/z/my-project/download/JULES_Compiler_Benchmark_Report.pdf"
with open(out, "wb") as f:
    writer.write(f)
print("wrote", out, "pages:", len(writer.pages))
