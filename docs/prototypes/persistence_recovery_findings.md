# LEO persistence and recovery: disposable proof

Question: Can the native application keep the existing `NEO Library` readable by NEO and Pocket while making its own saves recoverable? This proof uses only a temporary synthetic library. It does not implement production storage.

## Run

```sh
python3 docs/prototypes/persistence_recovery_prototype.py
```

Open `docs/prototypes/persistence_recovery_walkthrough.html` in a browser to review the human conflict and restore choices. Both assets are disposable.

## Candidate design

1. Keep the library's plain files and paths. Put operation journals, conflict copies, daily snapshots, and restore staging outside the synced library. The persistence service owns all native writes; editor and bookshelf code submit changed files and the hashes read at open. Idle sessions do not rewrite unchanged files.
2. Before each save, re-read every target and compare its bytes with the recorded baseline hash. When one differs, preserve the local candidate outside the library, leave the external file in place, pause the affected book, and ask the writer to choose or reconcile. Check again before each replacement. Compare content hashes, not only timestamps.
3. Prepare a durable operation journal containing the old and new bytes for all changed files. Then write each new file to a unique temporary file in its target directory, `fsync` that file, rename it over the target, and `fsync` the directory. Never enable direct-write fallback. Report save success only after all replacements and the durable completion marker succeed. On failure, keep the editor dirty and the journal recoverable. On reopen, roll forward only if each file still matches its old or intended new hash; otherwise pause and preserve the journal for manual conflict handling.
4. Block native writes while creating a snapshot; await pending saves. Copy source files, including unknown files and assets, except `Backups` and `Exports`. Verify source hashes did not change during copying, verify archive hashes, then publish the completed archive. A changed source aborts the snapshot. Call the due check at startup and hourly in long sessions. Keep at most fourteen completed daily archives; retain restoration safety archives independently. Optional drive failure is visible but does not block a healthy local save or local snapshot.
5. Before restoring, make and verify a safety snapshot of the current library. Validate the selected archive and extract it to a *separate* recovered library. Let the writer inspect it. Switch only after explicit selection; never overwrite the current synced library as a restore side effect.

This uses [Qt `QSaveFile`](https://doc.qt.io/qt-6/qsavefile.html) only if its direct-write fallback remains disabled, with explicit file and parent-directory durability checks around commit. Linux [rename](https://man7.org/linux/man-pages/man2/rename.2.html) gives atomic replacement of one path; [fsync](https://man7.org/linux/man-pages/man2/fsync.2.html) documents the separate directory sync needed for a durable directory entry. The journal is needed because rename does not atomically replace several files.

## Fixture observations

The command prints each checked scenario. Its synthetic `NEO Library` contains `library.json`, book metadata, chapter HTML, unknown asset, generated export, and legacy backup. It demonstrates failed single-file replacement, interruptions after journal preparation and after each replacement, safe replay, blocked replay when synchronization changes a file, observed conflict preservation, an unseen late legacy write that the native replacement overwrites, distinct missing and corrupt input, changed-only daily archives, failed archive handling, optional drive failure, hourly due checks across sixteen simulated UTC days, fourteen-archive retention, failed safety snapshot, and separate restoration. The proof cleans its temporary directory after running.

## Limits and implementation gates

- This is process-level fault injection. It does not prove power-loss durability, real filesystem behavior, Qt behavior, or actual Syncthing/Pocket handoff. Exercise those in implementation acceptance on Debian stable and real supported filesystems.
- The prototype's serial operation and fixed temporary names are deliberately simplified. Production needs unique names, serialized per-library writes, path and symlink defenses, quota handling, verified directory creation, and explicit deletion/move semantics.
- A legacy writer can change a file after the last comparison and before native rename. No cooperative cross-client lock or compare-and-swap exists in the old clients. Even with later detection, the overwrite may already have happened. Keep pre-save copies and snapshots; do not promise prevention of this race. Unsupported simultaneous editing remains outside the contract.
- A multi-file operation exposes intermediate states to NEO, Pocket, and Syncthing. The private journal repairs native reopening, but cannot make several synced files appear atomically elsewhere. Keep edits on one device at a time and wait for synchronization after closing the book. [Syncthing conflict copies](https://docs.syncthing.net/users/syncing) are useful evidence, not a semantic merge or a transaction.
- The snapshot prototype verifies unchanged content before publishing. A noncooperative writer can still race that final check, so production should freeze native writes and surface external changes; the supported handoff workflow remains essential.
- The HTML walkthrough is a decision aid, not a UI design. Human review must settle whether “keep shared,” “choose local,” and “inspect then switch” match the intended recovery experience.
