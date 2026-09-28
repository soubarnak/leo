# Synthetic PDF comparison, 2026-09-28

Host: Debian testing (forky/sid), XFCE/X11. Baseline: NEO 0.7.9 functions with Electron 43. Native probes: Qt 6.10.2, Pango 1.58.0, Cairo 1.18.4. Scratch Noto core/emoji fonts were supplied to both native probes. Only the synthetic `pdf-PROTOTYPE` book was used. See [probe instructions](../README.md).

| Path | A4 pages | Letter pages | Cover/title/chapter starts | Default page numbers |
| --- | ---: | ---: | --- | --- |
| NEO baseline | 6 | 6 | Separate pages, as intended | None |
| Qt `QTextDocument::print()` of baseline HTML | 11 | 10 | Cover missing; title and chapters flow together or split badly | Automatic |
| Pango/Cairo structured layout | 6 | 7 | Separate pages, as intended | None |

`pdfinfo` reports A4 as 595.92 × 842.88 pt for baseline/Pango and 595 × 842 pt for Qt; Letter is 612 × 792 pt for all. The NEO source uses locale country to choose A4/Letter and one-inch print margins. Both native probes set one-inch content margins. Cover art is inset farther than body text, matching the synthetic baseline direction. The preview pairs show that Pango/Cairo can place a cover, title, chapter headings, opening initial, scene break, emphasis and center/right-aligned prose. Exact sizes and spacing still differ.

Pagination is **not** equal. On A4, NEO's second chapter continues at paragraphs 14 and 29 on the next two pages; Pango/Cairo continues at 11 and 24. On Letter, NEO continues at 12 and 26; Pango/Cairo continues at 10, 22 and 34. The Pango result is one page longer on Letter. These differences are implementation work, not accepted parity.

With scratch fonts, Pango reports zero unknown glyphs. Its rendered Bengali, Arabic and emoji are visible, and `pdftotext` extracts Bengali and the full `👩🏽‍💻` sequence. `pdftotext` breaks the Arabic word into separated letters and bidirectional controls; the baseline extracts it as a contiguous word. Qt direct print extracts Bengali but loses the emoji and does not recover contiguous Arabic. The baseline extracts all strings, though its rendered mixed-script line shows missing glyph boxes on this host. Copy/search behavior in independent PDF readers and supported Debian stable fonts remain release gates.

Failure probe: writing either native output to a nonexistent directory returned nonzero and created no PDF (`Qt: 4`, `Pango/Cairo: 7`). The throwaway Pango code also returns `8` if its layout reports unknown glyphs, but it currently writes the sample before that check. A production exporter must render to a private temporary file, validate page count/size, glyph coverage and text extraction, then atomically publish; any failure must preserve an existing destination and show a clear error.

This is an application-stack decision probe, not a production PDF renderer. It does not exercise imported raster covers, every legacy document shape, all font packages on Debian stable, a full book/anthology export from the UI, email snapshots, long unbreakable paragraphs, widow/orphan rules, or independent-reader accessibility. The Qt comparison tests `QTextDocument::print()` directly; it does not rule out a separately engineered Qt drawing pipeline.
