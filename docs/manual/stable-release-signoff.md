# Stable-release maintainer sign-off (issue #39)

A stable release of `leo-writer` requires a person to record each judgement
below on the exact release package. Automated tests and scripted sessions
are listed as supporting evidence, but they do not replace the sign-off.
While any row is **Pending**, the build stays labelled **preview**
(`0.2.0+preview`, `-DLEO_PREVIEW_BUILD=ON`) and `docs/parity-matrix.md`
must keep `Release label: preview`.

Session template: record the date, tester, package version and build
commit, machine (`lsb_release -a`), desktop (session, X11 or Wayland), Qt
version, screen reader and version, and the result for each row.

| ID | Judgement | What the maintainer verifies | Supporting evidence already recorded | Sign-off |
| --- | --- | --- | --- | --- |
| S01 | Writing feel | A real writing session (typing, selection, undo, autosave, chapter navigation, zoom, typewriter scrolling, fullscreen) feels correct and loses no prose on a physical keyboard and display. | Automated: `leo-library-browser`, `leo-chapter-structure`, `leo-library-persistence`. Scripted keyboard session (xdotool, no human judgement): `docs/debian-stable-validation.md`. | Pending |
| S02 | Keyboard-only operation | Every core action and help is reachable without a mouse, focus is always visible, and shortcuts match Help → Writing Shortcuts. | Scripted xdotool keyboard session recorded in `docs/debian-stable-validation.md`; focus/shortcut regressions covered by `leo-library-browser`. The scripted run is not a human sign-off. | Pending |
| S03 | Screen-reader operation | Orca announces library, editor, dialogs and checked menu states correctly in both themes, and the fixes after 59ad74e are heard by a person. | Scripted Orca pass recorded in `docs/debian-stable-validation.md`; automated accessible-name/state tests in `leo-library-browser`. The doc states the post-59ad74e fixes have not been re-run under Orca. | Pending |
| S04 | Visual | Paper and night themes, drop caps, fonts, zoom levels, 100/150/200% scaling, and exported PDF pages are legible and correctly laid out in a real viewer; approved NEO pagination differences are accepted. | `QT_SCALE_FACTOR=2` scripted check in `docs/debian-stable-validation.md`; NEO PDF baseline compared numerically in `leo-manuscript-export`; rendered pages have not been inspected in a viewer (`docs/manuscript-pdf-export.md`). | Pending |
| S05 | Desktop integration | On physical Debian stable XFCE/X11: package installs, launcher appears with icon, `leo-writer` runs beside an existing `leo` command, single-instance behaves, removal preserves user data. | APT lifecycle drill scripted in `tests/debian_package_lifecycle.sh` inside a clean container; keyboard-only XFCE (xfwm4 on Xvfb) session in `docs/debian-stable-validation.md`. Both are headless container sessions, not a physical desktop. | Pending |
| G01 | Real-phone Device handoff | Handoff through the real NEO Pocket app on physical hardware via Syncthing, per `pocket/README.md`. | Emulator run recorded in `docs/device-handoff.md` (2026-10-01). An emulator is not a phone; the doc keeps this a manual release gate. | Pending |

## Rules

- Do not change a row to anything other than Pending without recording a
  dated session that names the package version and build commit verified.
- Stable release is blocked while any row is Pending, while any
  `docs/parity-matrix.md` row is not `pass`, or while an unresolved data
  loss or missing feature is known.
- Wayland and niri remain unclaimed until a separate session is recorded
  there; do not infer them from XFCE/X11 evidence.
