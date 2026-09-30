# EPUB 3 export

Use **Export manuscript → EPUB book** on a book or **Export EPUB anthology**
on a shelf. Leo saves the active chapter before exporting.

Reading order follows saved chapter order and shelf book order. Repeated shelf
references appear once. Anthologies include each book's cover, title, author,
subtitle and chapters, with a nested table of contents. The first book supplies
the anthology's package cover; its shelf name supplies the publication title.

EPUB uses the same clean prose projection as the other manuscript exports:
bold, italic, combined emphasis, inline breaks, paragraph alignment and scene
breaks survive. Unwritten ghosts, their associated scene breaks, placeholders
and Darling anchors are omitted. Typography uses reader-adjustable serif text
and proportional spacing. Imported cover art takes priority, with seeded
abstract artwork as fallback, matching the desktop export rule. Generated
painted covers are not used for publication.

The package targets EPUB 3.0, with an XHTML navigation document, ordered spine,
manifest, cover-image property, UUID, language and UTC modification metadata.
The ZIP begins with the exact uncompressed mimetype entry. Generated XML is
checked before publication; QSaveFile atomically replaces the destination
without direct-write fallback. Missing or malformed source material refuses the
whole export. Failed or cancelled exports preserve an existing destination,
and destinations inside the Library are refused.

## Validation

The `epubBook` test rows cover book export, abstract-cover fallback and shelf
anthologies through ManuscriptExport's public APIs. A Python ZIP/XML reader
checks package structure, TOC targets, reading order, typography and omitted
scaffolding. When EbookLib is installed in that interpreter, it independently
opens the package and reads the spine and TOC. Pillow additionally checks cover
pixels. Set `LEO_EPUB_READER_PYTHON` to select that interpreter.

Set `LEO_EPUBCHECK_JAR` to an EPUBCheck JAR to run EPUB 3 conformance validation
in each row. EPUBCheck 5.4.0 and EbookLib were used during implementation on
2026-09-30. These tools are test dependencies only, not required for export.
EPUBCheck is optional in ordinary test runs; internal XML checks always run.
