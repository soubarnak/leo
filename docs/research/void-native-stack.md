# Native editor stack investigation

Date: 2026-09-26. Status: **primary-source research complete; native editor prototype remains required**.

Canonical decision: [Compare native editor stacks against Leo's document semantics](https://github.com/soubarnak/leo/issues/2).

## Scope and evidence limits

The confirmed target is a native desktop UI without Electron or a webview, initially on the user's Void Linux x86_64/glibc machine with Wayland and niri. This phase produces research and an implementation plan, not the application.

The initial restricted session could not fetch documentation. After network access was restored, research resumed through the CLI and official documentation over HTTPS. Context7 successfully resolved Qt, GTK, and PySide6 before their respective documentation queries. Qt used `/websites/doc_qt_io_qt-6`, GTK `/gnome/gtk`, and PySide6 `/websites/doc_qt_io_qtforpython-6`. The browser denial was not retried. Sources below were read on the date above; current documentation does not establish compatibility with an arbitrary older installed package version.

## What the replacement must support

These observations come from the checked-out application, rather than promises in a framework's marketing material:

| Requirement | Local evidence | Consequence for the native design |
|---|---|---|
| Rich editable prose with bold, italic, paragraph alignment, and scene breaks | `app.js`, `wireChapterBody`, `styleKeepScroll`, `parasFromHtml`; `index.html` editable manuscript and auxiliary fields | The editor must expose structured runs and paragraphs, selection, and editable formatting. A plain text widget alone cannot establish parity. |
| Manuscript metadata embedded in HTML | `app.js`, `wireChapterBody`, `syncOutlineGhosts` area, `reconcileMarks`; searches for `data-sid`, `data-sec-id`, `data-sec-brk`, `.ghost`, `.ph-mark` | Preserving visible words alone is insufficient. Placeholder identities, outline section identities, and scene semantics must survive save/reload and edits. |
| Typing and structural undo interact | `app.js`, `snapshotStructure`, `structuralUndo`, `resetNativeUndo`, `wireChapterBody` | Chapter changes, Darlings, replacement, and typing must have a documented history policy. The current app explicitly clears browser history after structural changes to avoid replay against rearranged content. |
| Composition input must not trigger editor shortcuts | `app.js`, `wireChapterBody` composition handlers and keydown guard | Test preedit/commit/cancel with a real input method before judging editor viability. |
| Visual drop caps and bundled cover fonts | `styles.css`, `@font-face` rules and `.chapter-body:not(.cap-off) p:first-of-type::first-letter`; `app.js`, font settings | Decorative layout must preserve caret placement, selection, copy, and text order. Existing WOFF2 assets need a verified native font-loading or replacement strategy and a license audit. |
| PDF generation currently relies on a browser | `main.js`, `renderPDF`; `app.js`, `buildHtml` | Replace browser pagination and CSS with a native export renderer. Retaining a hidden browser for export would require explicitly revisiting the no-webview constraint. |
| Export excludes editing metadata deliberately | `app.js`, `parasFromHtml` removes ghost paragraphs, related unwritten scene breaks, placeholder marks, and old Darling anchors | Define one semantic export projection reused by PDF, EPUB, DOCX, HTML, Markdown, and text. Do not serialize the live editor view indiscriminately. |
| Files remain human-readable | `main.js`, book layout comment and `createBook` writes; `README.md`, “Your files” | Preserve the existing HTML/JSON library through an explicit compatibility codec. Do not silently replace the library with toolkit-generated markup or a database. |

Function names are included because line numbers will move as the implementation evolves. Source files: [app.js](../../app.js), [main.js](../../main.js), [styles.css](../../styles.css), [index.html](../../index.html), [README.md](../../README.md), [package.json](../../package.json).

## Recommendation

**Select Qt 6 Widgets with C++ and CMake as the first implementation candidate**, conditional on passing the native editor prototype. Qt documents a structured rich-text editor and a paginated document/PDF path within the same toolkit. This reduces the number of document-layout components Leo must integrate compared with the GTK approach below. This is an architectural judgment supported by the documented capabilities, not proof of feature parity or a measured performance ranking. Use native Widgets and Qt Gui; WebEngine is unnecessary for the documented PDF path and outside the confirmed constraint. [QTextDocument](https://doc.qt.io/qt-6/qtextdocument.html), [QPdfWriter](https://doc.qt.io/qt-6/qpdfwriter.html).

| Candidate | Documented fit and tradeoff | Recommendation |
|---|---|---|
| Qt 6 Widgets + C++ | QTextDocument exposes paragraphs, formats, cursors, document history, layout, and pagination; QPdfWriter consumes QPainter drawing commands. CMake module/link examples are provided in the class documentation. Semantic metadata and CSS fidelity still require application work. [Document](https://doc.qt.io/qt-6/qtextdocument.html), [PDF](https://doc.qt.io/qt-6/qpdfwriter.html). | First candidate. Keep one native language across domain operations and any required editor/layout extension. Avoid an additional language boundary unless evidence justifies it. |
| Qt 6 Widgets + PySide6 | Official Python bindings expose the same Qt APIs. Custom text objects are demonstrated using QPyTextObject, so missing custom-object support is not a valid reason to reject Python. Binding documentation warns that automatically translated snippets can contain errors. [Overview](https://doc.qt.io/qtforpython-6/), [official text-object example](https://doc.qt.io/qtforpython-6/examples/example_widgets_richtext_textobject.html), [interface](https://doc.qt.io/qtforpython-6/PySide6/QtGui/QTextObjectInterface.html). | Viable alternative if Python maintenance is preferred. It still needs the same semantic codec, history integration, typography proof, and a Python/binding deployment plan. No unmeasured claim that Python is too slow. |
| GTK4 + Rust | TextView/TextBuffer support tags, marks, and undo; GTK printing asks the application to render pages through Cairo. Rust bindings are safe bindings, but GTK objects stay on the main thread. [Text](https://docs.gtk.org/gtk4/section-text-widget.html), [printing](https://docs.gtk.org/gtk4/class.PrintOperation.html), [bindings](https://gtk-rs.org/gtk4-rs/stable/latest/docs/gtk4/). | Credible fallback. Its marks are attractive for text anchors, but Leo would need to assemble and validate more of the document-to-page pipeline. Rust does not remove that layout/serialization work. |

Reject the Qt candidate if the prototype cannot preserve editing semantics and drop-cap behavior without taking over most of the editor. In that case, revisit the document-layout design before assuming a switch of toolkit fixes the problem. Native controls may adapt; writing behavior and file compatibility remain required.

## Findings that change the design

### HTML and semantic markers

Qt documents an HTML 4 subset and explicitly says not all attributes are supported. Its CSS `float` support is limited to tables and images. QTextEdit also warns that loading then exporting HTML may change the markup. These are sufficient reasons to reject `setHtml()`/`toHtml()` as the persistence contract for Leo's `data-sid`, `data-sec-id`, ghost classes, and related metadata. The docs do not promise arbitrary attribute preservation; this is a missing guarantee, rather than a measured claim that every such attribute is always discarded. [HTML subset](https://doc.qt.io/qt-6/richtext-html-subset.html), [QTextEdit](https://doc.qt.io/qt-6/qtextedit.html).

Use an explicit legacy HTML decoder/encoder and a small application-owned semantic model. Display it through Qt blocks/runs/formats; do not save generic toolkit HTML over a book. GTK tags can represent formatting and locked regions, and marks maintain positions as text changes; those are useful editing primitives, not evidence of a compatible HTML serializer. [GTK text model](https://docs.gtk.org/gtk4/section-text-widget.html).

### Undo and anchors

Qt's document undo includes character/block insertion and deletion and formatting changes. Replacing the document via `setHtml()` resets history. Critically, `QTextBlock::setUserData()` data is deleted with the block and is **not** restored when that deletion is undone. Therefore block user data must not be the sole authoritative store for outline/placeholder identities. [QTextDocument](https://doc.qt.io/qt-6/qtextdocument.html), [QTextBlock::setUserData](https://doc.qt.io/qt-6/qtextblock.html#setUserData).

Custom text objects are another tempting marker implementation, but Qt explicitly warns that copy/paste ignores them. Any such choice requires a tested clipboard format and a plain-text fallback. PySide exposes the same limitation. [QTextObjectInterface](https://doc.qt.io/qt-6/qtextobjectinterface.html), [Python interface](https://doc.qt.io/qtforpython-6/PySide6/QtGui/QTextObjectInterface.html).

GTK exposes undo/redo and begin/end user actions for grouping operations. Neither toolkit's document history is evidence that a cross-chapter edit also restores the separate Darlings, stickies, and chapter-order data. Application transactions must establish that invariant. [GTK undo](https://docs.gtk.org/gtk4/section-text-widget.html), [user actions](https://docs.gtk.org/gtk4/method.TextBuffer.begin_user_action.html).

### Native PDF, typography, and covers

QPdfWriter generates PDF directly from QPainter commands and supports multiple pages. QTextDocument's `print()` can print an already paginated document; for an unpaginated document it makes a copy and paginates it, adding default 2 cm margins and page numbers. Those defaults differ from Leo's explicit browser PDF settings, so the export implementation must set layout deliberately and test it. [QPdfWriter](https://doc.qt.io/qt-6/qpdfwriter.html), [QTextDocument::print](https://doc.qt.io/qt-6/qtextdocument.html#print).

GTK supports PDF export through PrintOperation, with page drawing performed by the application using Cairo. Pango is GTK's text-layout/font component. Both candidates can form a native export pipeline; Qt's integrated paginated rich document is the reason to evaluate it first. [PDF export](https://docs.gtk.org/gtk4/method.PrintOperation.set_export_filename.html), [printing](https://docs.gtk.org/gtk4/class.PrintOperation.html), [GTK architecture](https://www.gtk.org/docs/architecture/).

Do not assume the CSS floated first letter can simply transfer to QTextDocument. Prototype a decoration/layout approach that leaves the initial letter as real editable text; custom drawing must not alter its selection/copy/accessibility semantics. QFontDatabase documents application font loading for TrueType, TrueType collections, and OpenType, whereas the repo's bundled CSS fonts are WOFF2. Obtain appropriately licensed native font assets or validate a conversion step; package and compare the resulting faces before accepting cover/type parity. [Font loading](https://doc.qt.io/qt-6/qfontdatabase.html#addApplicationFont), [HTML float limit](https://doc.qt.io/qt-6/richtext-html-subset.html), [local font declarations](../../styles.css).

### IME, accessibility, and spellcheck

QTextEdit implements input-method event handling. Qt's input event contract distinguishes preedit from committed text and specifies that preedit should not enter undo history. Preserve the standard editor's event path and prevent Leo shortcuts from consuming composition. Qt's standard widgets expose accessibility metadata, while custom widgets must expose or enhance it themselves. These capabilities do not prove correct operation with the user's input method or screen reader under niri. [QTextEdit](https://doc.qt.io/qt-6/qtextedit.html), [input event contract](https://doc.qt.io/qt-6/qinputmethodevent.html), [accessibility](https://doc.qt.io/qt-6/accessible.html).

GTK TextView documents input-method preedit rendering and implements Accessible/AccessibleText in current documentation. GTK's architecture supports Wayland through GDK. Match the chosen API version to the installed GTK package and test on the actual compositor. [TextView](https://docs.gtk.org/gtk4/class.TextView.html), [architecture](https://www.gtk.org/docs/architecture/).

Leo's spellcheck is application-owned and deliberately invoked on demand (`wireChapterBody`, spellcheck section in `app.js`; `nspell` and `dictionary-en-us` in `package.json`). Preserve this interaction, ignore/custom-dictionary behavior, and suggestion replacement through a separately selected native spelling backend. No claim is made that either base editor automatically replaces this feature; backend selection and dictionary/license compatibility remain an implementation dependency.

### Licensing and maintenance

Qt documents commercial and LGPLv3 licensing, with some modules available under GPL rather than LGPL. Qt for Python describes LGPLv3/GPLv3/commercial availability and additional third-party license notices. Choosing Widgets does not authorize indiscriminately adding every Qt module. Prefer distro shared Qt libraries and audit the exact selected modules, fonts, dictionary, and redistributed components before packaging. This report does not decide redistribution compliance for the final dependency set. [Qt licensing](https://doc.qt.io/qt-6/licensing.html), [PySide overview](https://doc.qt.io/qtforpython-6/), [third-party notices](https://doc.qt.io/qtforpython-6/licenses.html).

The gtk4 Rust binding crate is MIT licensed; that statement applies to the bindings, not every underlying GTK/Pango/Cairo library or asset. Its documentation also states GTK objects are not Send/Sync and GTK calls belong on the initialized main thread. Worker scheduling remains necessary for I/O and expensive export work in the GTK candidate. [Binding license and threading](https://gtk-rs.org/gtk4-rs/stable/latest/docs/gtk4/).

The C++ recommendation prioritizes direct access to Qt's document/layout APIs and a single application language over Python's iteration convenience. It does not assert C++ is inherently safer: ownership discipline and tests remain necessary. Exact Void package dependencies and integration decisions belong to the map's separate Void integration research; no package version is inferred from these toolkit docs.

## Proposed invariants regardless of toolkit

1. A persisted book retains stable book/chapter/section/placeholder/Darling identities, ordering, semantic formatting, and auxiliary content. Loading and saving must not silently discard unsupported input.
2. Separate persistence from display. Use a deliberately small document model for prose runs, paragraphs, scene breaks, outline ghosts, and placeholder anchors; keep toolkit objects out of the on-disk contract. Preserve additional legacy content or surface an explicit compatibility failure rather than dropping it.
3. Define edits as transactions across text and book structure. Select a single history policy with explicit behavior for typing, chapter split/join, Darlings, outline changes, and replace-all. Reuse toolkit history only where it can preserve that policy.
4. Build export content from the semantic document, with editing-only decorations removed. PDF needs its own pagination proof; successful HTML import is not proof that PDF matches the current behavior.
5. Keep keyboard navigation, accessible names, visible focus, screen-reader behavior, and IME composition as acceptance requirements. A custom-painted editor is acceptable only if it preserves these properties; it is not the default shortcut to typography parity.

These are recommended design constraints derived from the local behavior above, not claims that an existing toolkit already supplies them.

## Smallest useful proof before committing the stack

The subsequent prototype should implement one chapter editor and one export page, not the entire bookshelf UI. Use copied fixtures containing mixed bold/italic runs, alignment, scene breaks, a placeholder, an outline ghost and written outline section, a Darling restoration location, emoji, combining characters, Bengali text, and right-to-left text. The extra scripts are input robustness probes; they are not claims about the current app's guaranteed language coverage.

- Import/save/reopen: compare semantic structure, identity relationships, text, and formatting; confirm the original fixture is untouched.
- Edit/history: type, compose, paste, split/join chapters, cut/restore a Darling, replace-all, then undo across the boundary. Assert restored manuscript and associated metadata agree.
- Typography: select across the first letter, edit immediately around it, resize, change font, and copy; verify visual treatment never duplicates or drops a character.
- Accessibility and Wayland: inspect keyboard focus, screen-reader reading order, input method preedit/commit/cancel, clipboard, scaling, fullscreen, and file dialogs in the real niri session.
- PDF: render chapter headings, page breaks, scene breaks, emphasis, non-ASCII text, cover/title pages, and the supported paper size; compare semantic content and intended layout with the current app's output. Specify tolerances rather than promising identical font rasterization.
- Packaging: build and install through a disposable package/root or other agreed reversible staging method. Record actual package versions and run-time dependencies. Repeat on a clean Void x86_64/glibc environment before release.

No benchmark or prototype has been run for this note. Do not mark these checks complete from documentation alone.

## Decision status

The research question is resolution-ready: Qt 6 Widgets/C++/CMake is the recommended candidate because of its documented rich-document and native paginated PDF path. PySide6 remains viable with equivalent editor limitations; GTK4/Rust remains a fallback with more application-owned pagination integration. None is a drop-in substitute for Leo's HTML semantics.

The subsequent proof is [Validate native editing and choose the stack](https://github.com/soubarnak/leo/issues/5), after [Settle library compatibility, durability and Pocket coexistence](https://github.com/soubarnak/leo/issues/4) is resolved. That prototype must settle metadata/history, editable drop caps, IME/accessibility, and export layout before the stack is locked. Spelling backend selection, full dependency/license inventory, concrete package versions, and large-manuscript measurements remain implementation planning details; no feature-parity guarantee or successful build is claimed by this research.
