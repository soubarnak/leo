# DOCX manuscript export

Use a book's **Export manuscript → Word document** menu, or a shelf's
**Export DOCX anthology** menu. The current chapter is saved before export.

Books use their saved chapter order. Anthologies use the shelf's visible saved
book order, keeping the first occurrence of each book. Missing or malformed
source material refuses the whole export rather than publishing a partial book.
Title pages and chapters start on separate pages. Chapter headings carry Word's
Heading 1 style. Prose keeps bold, italic, combined emphasis, inline line breaks,
paragraph alignment, and centered scene breaks. The same clean projection used
by TXT, Markdown and HTML removes unwritten ghosts, placeholders and Darling
anchors. DOCX includes no cover images, matching the existing desktop DOCX rule.

All package XML is validated before a stored ZIP package is published through
QSaveFile without direct-write fallback. Failed or cancelled exports preserve
an existing destination. Destinations inside the Library are refused.

## Independent reader results

On 2026-09-30, `independentDocxReader` in `leo-manuscript-export-test` opened both
book and anthology DOCX files through LibreOffice 26.8.0.3's headless Word reader
and converted them to HTML. Checks passed for title/subtitle, chapter headings,
page breaks, right/justified alignment, bold/italic/combined emphasis, inline
breaks, scene breaks, absence of private drafting text and cover images, shelf
order, and inclusion of duplicate shelf references only once. LibreOffice's
logical CSS `end`/`start` alignment is normalized for Qt's HTML reader.

The independent-reader test skips when LibreOffice is absent; the remaining
export and failure-preservation tests do not require it. Microsoft Word has not
been checked in this environment.
