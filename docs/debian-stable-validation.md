# Debian stable validation record

Host: rootless podman container on Debian 13.7 (trixie) amd64, Qt 6.8.2, GCC 14.2, Pango 1.56, Xvfb with the xfwm4 window manager from XFCE, X11. The `leo-writer` 0.2.0 `.deb` was built with `dpkg-buildpackage` in that image and installed with `dpkg -i`. This is a headless X11 session, not a physical XFCE desktop.

## Package and tests

- `dpkg-buildpackage -us -uc -b` builds the package and runs all 14 test targets in parallel (`ctest -j`). Two real defects surfaced and were fixed: tests that needed an offscreen display, and tests sharing one application state directory.
- A clean `debian:trixie` container with the `.deb` installed and its recommended fonts passes all PDF export tests, including the NEO baseline comparison on A4 and Letter. Without the recommended Noto fonts, Bengali and emoji fixtures fail with a clear font error.

## Keyboard-only writing session (XFCE/X11)

Driven by xdotool key events only, with no mouse:

| Step | Result |
| --- | --- |
| End, Return in the Library tree | Chapter opens. |
| Type a sentence | Text appears and saves. Before the fix it was dropped because focus stayed on the page's first button. |
| Ctrl+A, Ctrl+C, Ctrl+End, Return, Ctrl+V | Selection copied and pasted as a second paragraph. |
| Ctrl+B, type, Ctrl+B | Bold run saved as `<b>`. |
| Ctrl+plus twice, Ctrl+0 | Zoom changes and resets. |
| Ctrl+Return, Escape | Fullscreen on (`_NET_WM_STATE_FULLSCREEN`), then off. |
| Alt+V, F1, Ctrl+F, each closed with Escape | View menu, writing shortcuts and Find dialogs open and close. Typing afterwards lands in the chapter (before the fix it did not). |

## Accessibility and scaling

- AT-SPI (`pyatspi`) lists the Library tree as "Library shelves, books, and chapters"; the chapter editor as "Chapter text or read-only source" and focused after a keyboard open; and named buttons such as "Return to Library", "Previous", "Next", "Save" and "Goals sprints…". Only internal list and cell wrappers are unnamed.
- With `QT_SCALE_FACTOR=2` the editor, headings and menu bar stay legible. The window opens at 1920×1400 and is not clamped to the 1280×800 screen.
- With the body typeface Georgia missing, the editor falls back and shows a notice. The first run rendered the fallback in a sans face beside a serif drop cap, because Qt's serif style hint does not reach fontconfig. The fallback now picks the first installed of Georgia, Liberation Serif, DejaVu Serif and Noto Serif, and only then the style hint (`unavailableSavedPreferencesUseFallbackWithoutLibraryWrites`).

## Writing-view persistence

`writingViewPreferencesPersistAcrossReopen` drives the View menu actions, then reopens the Library in a fresh window. Paper/night theme, brighter controls, pinned writing controls, typewriter scrolling, zoom, writing text size, body typeface and drop-cap style are written to `library.json` under NEO's keys and restored on reopen; turning them back off persists too.

Fullscreen and help are session state and are not persisted, matching NEO, which also toggles fullscreen without saving it. Hover panels are not saved either; only the "Pin writing controls" choice is (`chromePinned`, a LEO-only key).

## Scripted Orca pass (XFCE/X11, podman)

Orca 48.1, AT-SPI 2.56.2, Qt 6.8.2, xfwm4 4.20 on Xvfb 1280×800, `leo-writer` 0.2.0 built from 59ad74e. Keys sent with xdotool; speech read from Orca's debug log. This is a scripted run, not a person listening. Georgia and Liberation Serif were absent.

- Passes: the Library tree is announced as "Library shelves, books, and chapters tree"; Return on a chapter focuses and announces "Chapter text or read-only source text"; typing echoes; buttons are announced by name; View menu items announce checked state; F1 reads the writing shortcuts; Escape from help or Find returns focus to the editor.
- Font substitution: the status bar read "Saved typeface 'Georgia' is unavailable; using 'DejaVu Serif'." and the body rendered in a serif beside the serif drop cap. Orca never speaks that notice, because the chapter-open message replaces it.
- Scaling: at 2× the window grew to 1624×776 and the page clipped; the Library tree elided book and chapter names to "F…" and "…". 1.5× fits but the Library column is still narrow.
- Defects found, not yet fixed: leaf chapters announced as "expanded"; generic editor name without the chapter title; silent Tab stops on unnamed spacers; submenus (Page theme, Body typeface, Drop-cap style) not announced as submenus; "Zoom in" followed by a garbled shortcut; Find results tree has no accessible name; onboarding path field spoken only as "text"; window not clamped to the screen at 2×.

## Not covered

No human listening session (only the scripted Orca run above), writing-feel judgement, real XFCE panel session, or Wayland run. Those remain maintainer sign-offs.
