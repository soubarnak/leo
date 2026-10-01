#!/usr/bin/env python3
"""Independent PDF reader for export tests: prints JSON with page sizes and per-page text.

Uses pypdf and pdfminer.six separately so one extractor's quirks cannot hide a bad PDF.
Exit 3 when either library is missing so tests can skip rather than fail.
"""
import io
import json
import sys

try:
    from pypdf import PdfReader
    from pdfminer.high_level import extract_text
    from pdfminer.pdfpage import PDFPage
except ImportError:
    sys.exit(3)

path = sys.argv[1]
reader = PdfReader(path)
pages = []
with open(path, "rb") as handle:
    miner_pages = list(PDFPage.get_pages(handle))
for index, page in enumerate(reader.pages):
    box = page.mediabox
    pages.append({
        "width": float(box.width),
        "height": float(box.height),
        "pypdf": page.extract_text() or "",
        "pdfminer": extract_text(path, page_numbers=[index]) if index < len(miner_pages) else "",
        "images": len(page.images),
    })
json.dump({"pages": pages}, sys.stdout, ensure_ascii=False)
