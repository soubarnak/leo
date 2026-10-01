# PDF export

A writer exports a book as a searchable PDF from the book's context menu (Export manuscript → PDF book). Pango and Cairo render an application-owned model of the manuscript; Library HTML is never printed directly.

Page layout follows the NEO baseline:

- A separate cover page (imported cover, seeded artwork or generated artwork, scaled to fit), then a separate title page, then a fresh page for each chapter.
- A4 by default; Letter when the system locale territory is the United States, Canada, Mexico or the Philippines. All pages have one-inch margins and no page numbers.
- Chapter headings, a larger opening letter, scene breaks (`* * *`), bold and italic runs, and centered, right-aligned or justified paragraphs. Long paragraphs break between lines and continue on the next page.
- Ghost prompts, placeholders and Darling anchors are left out, as in every other export.

Text uses DejaVu Serif with DejaVu Sans behind it, then fontconfig fallback for other scripts. DejaVu is preferred because some installed Noto Arabic builds extract as broken characters; the `fonts-dejavu-core` package is a dependency. Zero-width joiners are not kept in extracted text, so an emoji sequence copies as its separate emoji.

## Failure behavior

Rendering happens in memory. If any character has no installed font, the export stops with an error and no PDF is written. The destination is replaced only after a complete render, through the same atomic save as the other exports. A failed export leaves an existing file unchanged.

## Validation

`tests/manuscript_export_test.cpp` exports synthetic books and reads the PDF with two independent readers in `tests/pdf_reader_check.py`: pypdf and pdfminer.six. Tests check page count and size for A4 and Letter, cover/title/chapter starts, hidden drafting content, the absence of page numbers, a paragraph that crosses pages, and Latin, Bengali, Arabic, combining-mark and emoji text. Arabic is checked with pdfminer because pypdf returns right-to-left text in stream order. Install both with `pip install pypdf pdfminer.six` and point `LEO_PDF_READER_PYTHON` at that interpreter; the PDF reader tests skip when the libraries are missing.

## Not yet verified

Page counts and continuation points are not yet compared against NEO output on copied manuscripts, and Debian stable with its own font set has not been run. Both are release gates in the parity matrix.
