# Native manuscript import

Right-click a shelf and choose **Import manuscript…**. Review the detected title,
chapter titles, scene-break count and omissions, then choose OK to create an
editable book on that shelf. Cancel creates nothing. The source is read only.

Supported inputs are UTF-8 TXT, Markdown (`.md`, `.markdown`) and unencrypted
DOCX with stored or deflated ZIP entries. Input and DOCX main-document XML are
limited to 16 MiB. Malformed archives, XML, invalid UTF-8 and NUL text are refused.

Detection rules:

- The filename supplies the default title. A leading Markdown `#` heading or
  DOCX paragraph with `Title` style supplies the book title.
- Markdown headings at levels 2–6, DOCX `Heading1`, and short lines starting with
  Chapter, Part, Prologue or Epilogue supply chapter titles. These lines become
  chapter metadata rather than duplicate body headings.
- DOCX page breaks begin chapters; empty chapters are skipped.
- Standalone lines containing 1–7 asterisks, hashes, bullets, tildes, asterisms
  or dashes become scene-break paragraphs.
- Other nonempty lines/paragraphs become prose. Unicode, bold and italic are
  retained. TXT is escaped as literal text.

Markdown raw HTML is refused, including foreign semantic IDs. Markdown links,
images and list syntax are reduced to text; the preview reports that limitation.
DOCX headers, footers and styling beyond bold/italic are omitted with a warning.
Tables, drawings, embedded objects, fields, tracked deletions, footnote/endnote
references and alternate content cause refusal with a named omission.

No external links or archive paths are followed. Imported markup is regenerated
from text and supported formatting; book and chapter IDs are newly generated.
Books use the existing NEO-compatible `book.json`, chapter HTML and supporting
JSON files, through the same staged creation/recovery flow as a new native book.
The shelf's pen name supplies the author. Source author front matter stays in
prose. A failed parse leaves the existing Library unchanged.
