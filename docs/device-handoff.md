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
the round-trip test above). It is not final proof of the Pocket handoff.

## Manual release gate

The handoff through the real NEO Pocket Android app on a phone is not automated;
the opt-in podman test above covers only Syncthing and the bridge code. It stays
a manual check that must pass before release, following the steps in the
"Device handoff check" section of [`pocket/README.md`](../pocket/README.md).
