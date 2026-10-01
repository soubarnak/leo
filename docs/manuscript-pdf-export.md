# PDF export

A writer exports a book as a searchable PDF from the book's context menu (Export manuscript → PDF book), or a whole shelf from the shelf's context menu (Export PDF anthology). An anthology starts with a title page showing the shelf name, then each book in shelf order with its own cover, title page and chapters. A book listed twice on a shelf appears once. The same layout, font and failure rules apply. Pango and Cairo render an application-owned model of the manuscript; Library HTML is never printed directly.

Page layout follows the NEO baseline:

- A separate cover page (imported cover, seeded artwork or generated artwork, scaled to fit), then a separate title page, then a fresh page for each chapter.
- A4 by default; Letter when the system locale territory is the United States, Canada, Mexico or the Philippines. All pages have one-inch margins and no page numbers.
- Chapter headings, a larger opening letter, scene breaks (`* * *`), bold and italic runs, and centered, right-aligned or justified paragraphs. Long paragraphs break between lines and continue on the next page.
- Ghost prompts, placeholders and Darling anchors are left out, as in every other export.

Text uses DejaVu Serif with DejaVu Sans behind it, then fontconfig fallback for other scripts. DejaVu is preferred because some installed Noto Arabic builds extract as broken characters; `fonts-dejavu-core` is a dependency. `fonts-noto-core` and `fonts-noto-color-emoji` are recommended: without them a Library containing Bengali or emoji cannot be exported, and the export stops with a font error. Zero-width joiners are not kept in extracted text, so an emoji sequence copies as its separate emoji.

## Failure behavior

Rendering happens in memory. The export checks that cairo finished without error, that every cover, title and chapter page was produced, and that the file has a PDF header and cross-reference trailer. Page size, page count and extracted text are verified by the automated tests with independent readers rather than at export time, because Cairo compresses its page objects. If any character has no installed font, the export stops with an error and no PDF is written. The destination is replaced only after a complete render, through the same atomic save as the other exports. A failed export leaves an existing file unchanged.

## Validation

`tests/manuscript_export_test.cpp` exports synthetic books and reads the PDF with two independent readers in `tests/pdf_reader_check.py`: pypdf and pdfminer.six. Tests check page count and size for A4 and Letter, cover/title/chapter starts, hidden drafting content, the absence of page numbers, a paragraph that crosses pages, and Latin, Bengali, Arabic, combining-mark and emoji text. Arabic is checked with pdfminer because pypdf returns right-to-left text in stream order. Install both with `pip install pypdf pdfminer.six` and point `LEO_PDF_READER_PYTHON` at that interpreter; the PDF reader tests skip when the libraries are missing.

## NEO baseline

Layout copies NEO 0.7.9's print CSS: 13pt text with 1.7 line height, a 465pt column cap (Letter pages are narrower than the page), 2em first-line indent, an opening letter at 1.8 times body size, and headings and scene breaks with NEO's margins. The cover is placed at the top margin and scaled to fit, as in NEO.

The synthetic fixture in `tests/fixtures/pdf` is compared with NEO's own output for the same book. Both agree with the baseline on page count (six pages on A4 and on Letter) and on where chapter 2 continues: paragraphs 14 and 29 on A4, 12 and 26 on Letter.

Intended differences from NEO, which need maintainer approval:

- Chapter headings and the author line are not upper-cased or letter-spaced, so they extract and search as written. NEO's PDF text extracts as "C H A P T E R".
- Scene breaks read `* * *` in dark gray rather than `***` at 8px letter spacing.
- Text is DejaVu Serif rather than the system's Georgia fallback, so line breaks inside paragraphs can differ slightly.

## Debian stable run

Run 2026-10-01 in a rootless podman container on Debian 13.7 (trixie), Qt 6.8.2, GCC 14.2:

- `dpkg-buildpackage` builds the package and all 14 test targets pass, including 23 export tests with Debian's `python3-pypdf` and `python3-pdfminer`.
- A clean trixie container with `leo-writer_0.2.0_amd64.deb` installed (plus its recommended fonts) passes every PDF test, including the NEO baseline comparison on A4 and Letter.
- With only the declared dependencies installed (DejaVu fonts), the Bengali and emoji fixtures fail with "No installed font covers every character in this book", which is why the Noto fonts are now recommended.

## Not yet verified

Copied manuscripts have not been compared with NEO, and nobody has inspected rendered pages in a viewer. Both are release gates in the parity matrix.
