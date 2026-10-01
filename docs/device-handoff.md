# Device handoff

A Library is a folder of plain files that can move between LEO on the desktop,
NEO, and NEO Pocket on a phone, usually through Syncthing. Handoff means
finishing work on one device and continuing on another without losing edits or
data that the second device does not understand.

## Rules

Close the app on the device you are leaving, then let synchronization finish
before you open the Library elsewhere. In Syncthing, wait until both devices
report **Up to Date**. LEO's **File → Prepare Device Handoff…** action saves
the open chapter and offers to close LEO so nothing is written after you start
waiting.

Edit a Library on one device at a time. Simultaneous editing of the same
Library on two devices is not supported, and LEO does not try to merge
conflicting edits. If LEO detects that another client changed the Library
during native work, it pauses saving, keeps your draft outside the shared
Library, and offers a separate Recovered library. The shared Library keeps the
external version until you explicitly switch.

## What is guaranteed

Opening a Library in another version of the app and saving a simple chapter
edit keeps the book's semantic graph: `stickies.json`, `darlings.json`,
protected sticky markers in chapter HTML, and chapter order. It also keeps data
the app does not recognize: unknown keys in JSON files and unknown supporting
files in the book folder.

## Automated evidence

The LEO and NEO round trip is covered by the
`leoNeoLeoRoundTripKeepsLatestEditsSemanticGraphAndUnknownData` test in
`tests/library_browser_test.cpp` (ctest name `leo-library-browser`). It copies a
fixture Library that includes unknown JSON keys and an unknown supporting file,
makes a LEO edit and runs the handoff action, then drives NEO's real
`main.js` IPC handlers headlessly through `tests/neo_handoff_roundtrip.cjs` to
read the Library and make a simple edit. LEO then reopens the Library and the
test checks that NEO's latest edit is visible and that the graph and unknown
data are intact. The cycle repeats with a second LEO edit that NEO reads back.
The test is skipped when `node` is not installed.

The harness reads NEO's own handlers and writes back what it read plus one
appended paragraph. It does not reproduce what NEO's renderer would save, and
no real synchronization is involved. Conflict handling is covered separately by
`externalEditPreservesDraftAndOffersExplicitRecoveredSwitch`.

### Syncthing and Pocket bridge evidence (opt-in)

`tests/handoff/syncthing_handoff.sh` runs two real Syncthing instances
(official `docker.io/syncthing/syncthing` image) in rootless podman containers
on a private network: A plays the desktop, B plays the phone. Discovery, relays
and NAT are off and the devices use static addresses. A synthetic Library
(two chapters, stickies, darlings, a protected sticky marker, unknown JSON keys
and an unknown `.bin` file) is seeded in A and the test then:

1. waits for Syncthing to deliver it to B and checks Pocket reads the same data
   as NEO;
2. makes a desktop edit with NEO's real `main.js` handlers, waits for sync;
3. makes a phone edit by running Pocket's real, unmodified
   `pocket/www/pocket-bridge.js` headlessly over the synced files
   (`tests/handoff/pocket_bridge_harness.cjs`), waits for sync back;
4. makes a second desktop edit, waits for sync, and confirms both devices read
   all three edits in order with identical chapter order, titles, stickies,
   darlings, protected marker, unknown keys, and a byte-identical (sha256)
   unknown file.

It is opt-in so default `ctest` stays fast and offline. Run it directly with
`bash tests/handoff/syncthing_handoff.sh`, or configure with
`-DLEO_RUN_SYNCTHING_TESTS=ON` and run `ctest -L syncthing`. It exits 77
(skipped) if podman, node, or the image is unavailable. Set
`LEO_SYNCTHING_IMAGE` to use a different image.

This proves real Syncthing synchronization and Pocket's bridge code against
synced files. It does **not** run the Android app, its WebView, Capacitor's
native filesystem plugin (a node `fs` mock stands in), Android permissions, or
a real phone, and it does not drive LEO's Qt window (LEO's side is covered by
the round-trip test above). The Android emulator test below covers the app.

### Android emulator evidence (opt-in)

`tests/handoff/android_pocket_handoff.sh` runs the same handoff through the real
Android stack, in an emulator, in rootless podman:

- the real NEO Pocket debug APK (built from this checkout) and the official
  Syncthing for Android app (`syncthing/syncthing-android` 1.28.1, checksum
  pinned) run in an Android 15 (API 35, `google_apis` x86_64) emulator with KVM;
- a desktop Syncthing (official image) in a second container shares the
  emulator's network namespace, so the phone dials it at `10.0.2.2:22000`
  (discovery, relays and NAT off);
- Pocket gets All files access (`appops MANAGE_EXTERNAL_STORAGE`), and the
  Syncthing Android app delivers a synthetic Library into
  `/storage/emulated/0/Documents/NEO Library`;
- desktop edits go through NEO's real `main.js` handlers; the phone edit is made
  in Pocket's real WebView with adb touch and key events (open the book, tap into
  the chapter, Ctrl+End, Enter, type, wait past the autosave, tap **Shelf**, close
  the app). Chrome DevTools is used only to locate elements and read state;
- after each step it waits for both sides to match file by file, then checks both
  desktop and the app on the device: all three edits in order, chapter order and
  titles, stickies, darlings, the protected sticky marker, unknown JSON keys and a
  byte-identical unknown `.bin` file. Screenshots, logcat and logs land in a
  gitignored evidence directory.

Run it with `LEO_ANDROID_CACHE=<big-dir> LEO_ANDROID_ACCEPT_LICENSES=1 bash
tests/handoff/android_pocket_handoff.sh` (first run downloads about 6 GB: Android
SDK, system image, Gradle; later runs take about six minutes), or configure with
`-DLEO_RUN_ANDROID_POCKET_TESTS=ON` and run `ctest -L android`. It exits 77
(skipped) without podman, `/dev/kvm`, node, or when the downloads are unavailable.
`/dev/kvm` access from rootless podman needs `crun`; if it is not installed the
script downloads the pinned static release into the cache.

Recorded run (2026-10-01, leo `c7179e3` plus the uncommitted changes that added
this test): passed on an emulated Android 15 (API 35), WebView 124.0.6367.219,
Syncthing for Android 1.28.1, desktop Syncthing v2.1.5, Pocket debug build.

What the real editor does to a Library (all asserted, none lost): Pocket adds
`authors` and `hintShown` to `library.json` and `chapterNotes`, `wordCount` and
`dailyCounts` to `book.json`; it rewrites the chapter HTML it saves, so an inner
`<span>` inside an unknown `<div>` is unwrapped and `inputmode="none"` is added to
the sticky marker (the unknown element, its attribute and text, and the marker
itself survive). The headless tests above do not exercise these because they never
run the editor.

## Manual release gate

Handoff through the real NEO Pocket app is now automated on an emulator, but an
emulator is not a physical phone. The check on a real phone stays a manual release
gate, following the "Device handoff check" section of
[`pocket/README.md`](../pocket/README.md), until it has been done once on hardware.
Still not proven: vendor Android builds and their storage or battery restrictions,
the current Syncthing for Android 1.28.1 (December 2024, the latest release of that app) on a real
network, relays or discovery, a signed release APK (the test uses the debug build),
and LEO's own **Prepare Device Handoff…** window with a real Syncthing.
