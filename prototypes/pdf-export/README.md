# Throwaway PDF export decision probe

For [Choose native PDF export renderer and fidelity contract](https://github.com/soubarnak/leo/issues/10). This folder compares one synthetic book through NEO 0.7.9's actual `buildHtml` and `renderPDF` functions, Qt `QTextDocument::print()`, and a native Pango/Cairo layout. It contains no manuscript data or production exporter.

## Run

On Debian with Electron 43 installed for this checkout, a C++ compiler, Qt 6 development packages, Pango/Cairo development packages, Poppler tools and ImageMagick:

```sh
LEO_ELECTRON=/path/to/electron LEO_FONTCONFIG_FILE=/path/to/fonts.conf ./prototypes/pdf-export/run
```

`LEO_ELECTRON` defaults to this checkout's `node_modules/.bin/electron`; omit `LEO_FONTCONFIG_FILE` when suitable Noto fonts are installed. The captured run used Debian testing + XFCE/X11, Electron 43, Qt 6.10.2, Pango 1.58.0, Cairo 1.18.4. Noto core and color emoji fonts came from the scratch fontconfig tree described in the [earlier export probe](../native-editor/evidence/round5-debian/export-probe.txt); no host package was installed. The runner writes synthetic outputs only under `evidence/` and compiles to a disposable `/tmp` directory.

`baseline.js` executes the two real NEO 0.7.9 function bodies extracted from `app.js` and `main.js`. It supplies a synthetic book and cover. Only `app.getLocaleCountryCode()` is replaced to force both paper choices (`IN` for A4, `US` for Letter). The cover is a synthetic SVG supplied directly to `buildHtml`; this does not validate NEO's cover generation. `qt-print.cpp` prints that HTML with Qt's rich text engine and one-inch margins. `pango-print.cpp` lays out the same structured fixture, including the cover, title, chapter starts, emphasis, alignment and scene break. Its hardcoded art and layout rules are throwaway code.

## Review

Left side of each preview is NEO baseline. Right side is Pango/Cairo. Open full PDFs to inspect page flow and text selection:

- [Cover preview](evidence/compare-cover.png), [title preview](evidence/compare-title.png), [chapter preview](evidence/compare-chapter.png).
- [Baseline A4](evidence/baseline-a4.pdf), [Pango/Cairo A4](evidence/pango-a4.pdf), [Qt A4](evidence/qt-a4.pdf).
- [Baseline Letter](evidence/baseline-letter.pdf), [Pango/Cairo Letter](evidence/pango-letter.pdf), [Qt Letter](evidence/qt-letter.pdf).

[Evidence and limits](evidence/observations.md) records page starts, glyph behavior, margins and failure behavior. These samples raise fidelity enough for a maintainer decision about the renderer contract. They do not certify production PDF export.
