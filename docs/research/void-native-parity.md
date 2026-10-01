# Void native rebuild: existing behavior

Inspected 2026-09-26 at commit `6dd75296d17644098ff53701d324ba7baf70da49`.
This is a source audit, not a claim that every current feature works correctly.

## Confirmed scope

- Native desktop UI; no Electron or embedded webview.
- First target: Void Linux, x86_64, glibc, Wayland, niri.
- This phase produces research and an implementation plan. No rewrite is implemented here.
- Complete desktop feature parity, existing library files and the writing experience
  are required. Native controls and Linux shortcuts may look different (confirmed).

## Baseline

The app identifies itself as NEO 0.7.9. It uses Electron, vanilla JavaScript,
HTML and CSS. `package.json:63` already defines Linux AppImage builds for x64
and arm64, and `.github/workflows/build.yml:109` contains a Linux build job.
These are packaging capabilities, not evidence of successful Void validation.

The main process owns filesystem and OS operations; `preload.js:3` exposes
the bridge used by the renderer. A native rewrite must replace both the UI
and the platform services behind this bridge.

## Feature inventory and acceptance evidence to collect

| Area | Existing behavior | Source | Required proof for the rebuild |
| --- | --- | --- | --- |
| Library | Bookshelves; drag/reorder books and shelves; rename/delete shelves; pen names; safe author reassignment | `app.js:238`, `app.js:762`, `app.js:803` | Reopen a multi-author library with identical membership and ordering; author deletion retains books |
| Onboarding | Author, pantser/plotter mode, font and drop-cap selection | `app.js:144` | Both writing modes open the expected view and persist preferences |
| Editor | Continuous pages, chapter titles and numbering, reorder/navigation, paragraph/scene/chapter creation through Enter, split/join/delete, bold/italic/alignment, cleaned paste, smart punctuation | `app.js:921`, `app.js:1033`, `app.js:1445`, `app.js:1646` | Replay representative edit sequences including selection, composition, Unicode and undo/redo |
| Revision | Cross-chapter find/replace, replace-all, explicit spellcheck, suggestions and custom dictionary, placeholders/stickies, notes, chapter/section outline and ghost paragraphs | `app.js:1796`, `app.js:2407`, `app.js:2590`, `app.js:3125`, `app.js:3345` | Markers and outline links survive edits, save/reopen and undo; exports omit unresolved drafting scaffolding |
| Darlings | Cut passage preservation and anchored restoration; deleted chapters preserved; structural undo | `app.js:998`, `app.js:2193`, `app.js:2213`, `app.js:2706`, `app.js:3015` | Restore with matching anchors and when anchors/chapter disappear; no text lost or duplicated |
| Presentation | Night/paper modes, brighter controls, hover/pinned panels, fonts/drop caps, zoom, typewriter scrolling, fullscreen, help/shortcuts | `main.js:742`, `app.js:2068`, `app.js:3523`, `app.js:3788` | Keyboard access, focus, scaling, IME, clipboard and fullscreen on niri; inspect both themes |
| Progress | Book/chapter/selection counts, daily/book goals, 30-day chart, configurable writing-day boundary, word-target sprints | `app.js:2762`, `app.js:2802`, `app.js:3557`, `app.js:3705` | Stable counts, day-boundary behavior and persisted history; sprint completion/cancellation |
| Covers | Seeded artwork and typography, imported images, optional manuscript-derived AI art at 1,000 words, repaint/mode selection, request deduplication | `app.js:473`, `app.js:649`, `main.js:216`, `main.js:310`, `art.js:110` | Every cover mode works; failed/cancelled requests preserve prior cover; credentials stay outside library |
| Import | DOCX/TXT/Markdown; title/chapter/scene heuristics | `main.js:483`, `app.js:3297` | Fixture imports preserve text/formatting and document the same heuristic boundaries |
| Export | TXT/Markdown/HTML/PDF/DOCX/EPUB 3; shelf anthologies in EPUB/DOCX/PDF | `app.js:3945`, `app.js:4141`, `app.js:4232`, `app.js:4347` | Open outputs in independent readers; inspect pagination, TOC, ordering, typography and cover rules |
| Email provenance | Timestamped PDF, SHA-256 text fingerprint, Gmail draft/manual attachment, desktop mail draft | `main.js:434`, `app.js:4449` | Matching digest and usable saved PDF; real Linux mail workflow with explicit attachment outcome |
| Operations | Autosave, ZIP backups, error log, single instance, release check and packaged updates | `main.js:620`, `main.js:914`, `main.js:943`, `app.js:2881` | Crash/restart and close-during-save fixtures; backup restore; second-instance behavior; correct fork update source |

## Storage semantics that must survive

The library defaults to the OS Documents directory plus `NEO Library`.
`library.json` holds library organization and preferences, while `_catalog.txt`
is regenerated for human browsing (`main.js:19`, `main.js:43`, `main.js:990`).

Each book uses `book.json`, ordered `chapters/<id>.html`, `notes.html`,
`outline.html`, `darlings.json`, and `stickies.json`. Metadata includes chapter
notes, section notes, daily counts, covers and reading position (`main.js:98`,
`app.js:2809`, `app.js:2896`, `app.js:3015`).

HTML is not merely presentation: `.scene-break`, `.ghost`, `.ph-mark`,
`data-sec-id`, and `data-sec-brk` encode writing relationships. Loading HTML
into a native rich-text widget and saving its generic HTML output is not yet
a proven lossless conversion (`app.js:2590`, `app.js:3945`).

Exports strip placeholders and unwritten outline ghosts with their associated
scene breaks. Actual scene breaks, bold/italic/alignment survive. Single-chapter
stories omit chapter headings (`app.js:3945`). AI paintings remain on the shelf;
exports use a user cover or seeded art (`app.js:4193`).

Darlings use 60-character surrounding anchors, with chapter/book-end fallback
if the original location cannot be found (`app.js:2193`, `app.js:2213`).

Pocket shares this library through Syncthing and another implementation of the
same bridge. Its documented conflict detection remains a TODO
(`pocket/README.md:3`, `pocket/README.md:55`, `pocket/www/pocket-bridge.js:104`).
Keep compatibility in the decision map even though rebuilding Android is outside
this desktop effort.

## Existing defects and limitations to distinguish from parity

- JSON uses temporary-file rename, but chapter/auxiliary HTML uses direct overwrite
  (`main.js:76`, `main.js:149`, `main.js:172`). Save debounces 800 ms and also
  flushes on blur/unload and every 20 seconds; close-time IPC promises are not
  awaited (`app.js:2881`). The native design needs explicit durability semantics.
- Backups run at startup, once per UTC date, exclude `Backups`/`Exports`, and retain
  14 archive files. A continuously open session does not currently schedule a
  new daily backup (`main.js:637`, `main.js:1016`).
- Trash failure preserves the original book (`main.js:186`). Preserve that safety
  property; do not replace a failed trash operation with permanent deletion.
- Non-Gmail email invokes macOS `osascript` on every platform (`main.js:454`).
- Secrets silently fall back to plaintext when encrypted storage is unavailable
  (`main.js:277`, `main.js:294`), contrary to the README's encryption description.
- Body font choices include platform-specific names; only cover fonts are bundled
  (`main.js:744`, `app.js:3788`, `styles.css:106`). Font substitution needs explicit QA.
- Release publishing, app identity and error-report destinations still reference
  upstream `hughhowey/neo` (`package.json:16`, `main.js:916`, `main.js:1023`).
- No root automated test command is defined. CI packaging is not feature-parity
  verification (`package.json:8`, `.github/workflows/build.yml:109`).

## Largest decision risks

1. A native document model must preserve domain markers and coordinate typing
   undo with structural editing. Chromium DOM editing cannot simply be translated
   into a sequence of equivalent widget calls.
2. PDF pagination and canvas-generated cover art need native replacements because
   the user excludes a browser engine (`main.js:383`, `app.js:4196`).
3. Library conversion must establish backup, rollback, unknown-field preservation
   and Pocket coexistence rules before any real manuscript is written.
4. Full parity requires verifying uncommon workflows such as anthology export,
   anchored darlings restoration and email attachments, not only the writing view.
