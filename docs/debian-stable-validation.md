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

## Not covered

No human screen-reader (Orca) session, writing-feel judgement, real XFCE panel session, or Wayland run. Those remain maintainer sign-offs.
