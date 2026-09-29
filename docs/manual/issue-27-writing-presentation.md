# Issue #27: Debian writing presentation pass

The native View menu provides paper and night themes, brighter and pinned
controls, body typeface and drop-cap choices, writing text size, zoom,
typewriter scrolling, and fullscreen. Help → Writing Shortcuts lists the main
keyboard commands. Library presentation choices are saved in `library.json`.

## Recorded verification

- Automated Qt browser test and full CTest suite passed on 2026-09-30 using
  the offscreen Qt platform.
- A Debian stable XFCE/X11 manual pass is pending. The development session ran
  on Wayland/niri; XFCE and a screen reader were unavailable. The automated
  results are not evidence of XFCE/X11 or screen-reader behavior.

## Manual pass on Debian stable XFCE/X11

Open a disposable Library with at least two chapters and record the date,
desktop, Qt version, screen reader, and results below.

| Check | Result |
| --- | --- |
| Tab and Shift+Tab reach the writing view controls; focus is visible and spoken | Pending |
| Select prose by keyboard; Ctrl+C, Ctrl+X, Ctrl+V preserve expected text | Pending |
| Ctrl+B, Ctrl+I, Ctrl+F and menu shortcuts work while writing | Pending |
| Ctrl+Enter enters fullscreen and Escape exits it | Pending |
| Both themes, brighter controls, hover and pinned controls remain legible | Pending |
| Font substitution, drop caps, text size and zoom remain readable after reopen | Pending |
| Typewriter scrolling keeps the caret near the viewport center | Pending |
| Screen reader announces editor, chapter and control focus in both themes | Pending |
| Scaling at 100%, 150%, and 200% does not clip controls or text | Pending |

Do not infer Wayland/niri behavior from this X11 pass; record a separate pass
there when available.
