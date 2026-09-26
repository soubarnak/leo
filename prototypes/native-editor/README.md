# Throwaway native editor decision probe

For [Validate native editing and choose the stack](https://github.com/soubarnak/leo/issues/5).
**First interaction round; not a stack decision, production editor, or compatibility guarantee.**

Run on the existing Void machine:

```sh
./prototypes/native-editor/run
```

Requires a C++17 compiler, CMake and Qt 6 Widgets/PrintSupport development files. No browser/webview is linked into the native demo. One window edits synthetic data; a second pane shows serialized HTML and JSON. Save/reopen and PDF write only to a process-specific `/tmp/leo-editor-PROTOTYPE-<pid>` folder. There is no real-library opener. Reset affects in-memory fixtures only. Do not use this on manuscript files.

## Try it

1. Type in the middle of the opening paragraph. Use Enter once, twice, then three times. Switch chapters using the selector.
2. Undo the chapter split, then the scene gesture. The scene undo rejoins the initial paragraph split, matching the legacy gesture. Try redo and typing after undo.
3. Select `bold`, Cut Darling, then Restore Darling. Undo both operations and inspect the record and formatting.
4. Try Bengali/other composition with your actual input method, cancellation, mouse selection, and keyboard movement across combining characters and emoji.
5. Save/reopen; inspect the scratch HTML/JSON. PDF produces an intentionally basic pagination probe, not a final export implementation.

Please report the action, expected behavior, and what happened. The purpose of this round is feedback on native input and structural gestures. The stack remains provisional.

## Human feedback, 2026-09-26

The maintainer tried the native demo and reported: “Typing, Enter gestures, undo, and Darlings felt right; I did not test an IME.” This validates the first interaction round only. It is not approval of untested library compatibility or a final stack choice.

## Evidence recorded 2026-09-26

- Void Linux, x86_64/glibc; installed Qt 6.11.2, native `wayland` platform. `niri msg --json windows` identified the running `leo-editor-probe` window, PID 13198, in workspace 1.
- [18 automated diagnostic checks](evidence/checks.txt) passed via `./prototypes/native-editor/run --probe`. These are small synthetic examples, not an exhaustive suite. They cover saved/reopened HTML and JSON, rejection of an unsupported image element, negative-control metadata loss through Qt's generic serializer, typing history, Enter gestures, section/sticky chapter ownership, Darling restoration and undo, synthetic IME preedit/cancel/commit/undo.
- [Window capture](evidence/window.png) comes from the probe's own QWidget; it contains only synthetic data. The final source includes the ownership check added after the first interactive window was launched; relaunch for the latest version.
- [Raw PDF evidence](evidence/sample.pdf): native Qt output, 11 A5 pages. The inspected prose page renders emphasis, Bengali and Arabic but the emoji sequence becomes a black square. Text extraction also fails to recover the complete Arabic sequence. This is an **unsatisfied export gate**, not a polished deliverable. Default Qt margins/page numbers are used; existing export parity has not been tested.
- `legacy-probe.py` extracts the baseline's actual `captureBody`, `syncGhosts`, and `reconcileMarks` functions into a synthetic Chromium DOM harness. The installed Vivaldi headless invocation timed out after 40 seconds. **No legacy-browser round trip passed.** No full legacy desktop or Pocket application was run. The script's assertions are proposed checks, not evidence that they executed.

The latest interactive process might differ from the PID above. The diagnostic log records its own separate process and scratch directory. Do not confuse a successful synthetic QInputMethodEvent with a real compositor/input-method test.

## What this prototype implements, and what it does not

An explicit fixture codec maps paragraphs, emphasis and span attributes to Qt format properties, and serializes them itself. Generic `QTextDocument::toHtml()` is only a negative control: it discards `data-sid` in this experiment. Unknown JSON fields and accepted paragraph/span attributes survive the exercised round trip. A strict XML-fragment reader accepts the bounded fixture markup, normalizing bare `<br>`; it rejects unsupported tags/styles instead of overwriting them. **This is not a general HTML5 legacy codec**: named HTML entities, malformed but browser-valid markup, arbitrary nesting/styles and opaque unknown content need an actual compatibility design.

History uses whole synthetic-book snapshots, including chapter order, section notes, stickies and Darlings, per key/input event. This is a cheap way to expose consistency in a tiny fixture, not a production history policy: typing grouping, large-book performance, selection restoration and every mutation path need further proof. The prototype's ownership updates are bounded to generated chapter splits; they are not a general identity reconciler. It does not reproduce the legacy application's known undo defects.

Remaining gates before resolution:

- Human feedback on the native window and real IME commit/cancel; keyboard, accessibility and scaling tests on niri.
- Editable drop caps. The source at `app.js`'s `selectionchange` handler hides the drop cap while the caret is in the first paragraph; that reduces the native layout question, but no decoration/layout implementation is proven here.
- Protected placeholder deletion, marker-safe rich clipboard, ghost promotion through all input paths, scene deletion/chapter joining, replace-all and multi-paragraph Darling restoration. This demo's plain-text paste intentionally does not establish clipboard parity. Its ghost handling is incomplete (keyboard text removes ghost class, but composition/paste coverage remains to implement).
- Full native-to-legacy-to-native round trips, including native-saved fixtures reopened in the old application and Pocket; unknown HTML preservation/refusal coverage.
- Export fidelity, font/emoji behavior, accessible text extraction, and comparison with the baseline PDF. The native rendering path exists; that is not enough to accept it.
- Durable multi-file saving, conflicts and recovery are deliberately owned by [Validate library persistence and recovery design](https://github.com/soubarnak/leo/issues/7), not claimed by this scratch save/reopen button.

These gaps distinguish incomplete probe code from a demonstrated Qt limitation. Do not infer that Qt must be rejected from behavior the probe has not implemented. Conversely, the passing examples do not justify selecting Qt yet.

## Supporting documentation

Current Qt docs were fetched with Context7 (resolve first, then two focused queries). Relevant contracts:

- [QTextCursor](https://doc.qt.io/qt-6/qtextcursor.html): inserted blocks inherit formats unless explicit formats are supplied; custom format attributes need deliberate ownership on split.
- [QTextFormat properties](https://doc.qt.io/qt-6/qtextformat.html): properties used for this bounded fixture mapping.
- [QInputMethodEvent](https://doc.qt.io/qt-6/qinputmethodevent.html): distinct preedit and commit operations.
- [QTextDocument printing](https://doc.qt.io/qt-6/qtextdocument.html#print): unpaginated documents get default margins and page numbers. This probe deliberately exposes that default, rather than claiming export parity.

## Additional fixture experiment commands

```sh
python3 prototypes/native-editor/legacy-probe.py /tmp/leo-editor-PROTOTYPE-PID/book-PROTOTYPE /tmp/leo-legacy-result
./prototypes/native-editor/run --roundtrip /tmp/leo-legacy-result/legacy-saved.html /tmp/leo-native-return.html
```

The first command is currently unverified because of the recorded browser timeout. The second only exercises the strict fixture codec, writes a new output file (refuses an existing destination), and cannot establish full-application interoperability by itself.
