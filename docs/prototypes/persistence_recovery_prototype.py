#!/usr/bin/env python3
"""DISPOSABLE filesystem proof for LEO persistence; synthetic fixtures only.

Run: python3 docs/prototypes/persistence_recovery_prototype.py
This is a design experiment, not production storage code or a power-loss test.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
import zipfile


def digest(data: bytes | None) -> str | None:
    return None if data is None else hashlib.sha256(data).hexdigest()


def read(path: Path) -> bytes | None:
    try:
        return path.read_bytes()
    except FileNotFoundError:
        return None


def sync_dir(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def replace_durable(path: Path, data: bytes, fault: bool = False) -> None:
    """Same-directory stage, fsync, rename, directory fsync; no direct-write fallback."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.leo-staging-{os.getpid()}")
    try:
        with temporary.open("xb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        if fault:
            raise OSError("injected failure before rename")
        os.replace(temporary, path)
        sync_dir(path.parent)
    finally:
        temporary.unlink(missing_ok=True)


def checked(path: Path) -> bytes:
    data = read(path)
    if data is None:
        raise FileNotFoundError(f"missing input: {path.name}")
    if path.name in {"library.json", "book.json", "darlings.json", "stickies.json"}:
        try:
            json.loads(data)
        except (UnicodeError, ValueError) as error:
            raise ValueError(f"corrupt input: {path.name}") from error
    return data


class Store:
    def __init__(self, library: Path, recovery: Path):
        self.library = library
        self.recovery = recovery
        self.pending = recovery / "pending"
        self.completed = recovery / "completed"
        self.conflicts = recovery / "conflicts"
        self.snapshots = recovery / "snapshots"
        for folder in (self.pending, self.completed, self.conflicts, self.snapshots):
            folder.mkdir(parents=True, exist_ok=True)

    def retire(self, operation: Path) -> None:
        """Remove live gate atomically; later cleanup of completed data is harmless."""
        destination = self.completed / operation.name
        os.replace(operation, destination)
        sync_dir(self.pending)
        sync_dir(self.completed)
        shutil.rmtree(destination)
        sync_dir(self.completed)

    def preserve_conflict(self, name: str, local: bytes, external: bytes | None) -> Path:
        folder = self.conflicts / name
        folder.mkdir(parents=True, exist_ok=True)
        replace_durable(folder / "local", local)
        if external is not None:
            replace_durable(folder / "external", external)
        return folder

    def recover_conflict_to_library(self, relative: str, label: str) -> Path:
        """Build an inspectable local library; never replace the shared library."""
        if list(self.pending.iterdir()):
            raise RuntimeError("finish pending saves before conflict recovery")
        local = checked(self.conflicts / relative.replace("/", "__") / "local")
        source = self.inventory()
        if {name: digest(data) for name, data in self.inventory().items()} != {
                name: digest(data) for name, data in source.items()}:
            raise RuntimeError("shared library changed during conflict recovery")
        recovered = self.recovery / f"recovered-conflict-{label}"
        if recovered.exists():
            raise FileExistsError(recovered)
        recovered.mkdir()
        try:
            for name, data in source.items():
                replace_durable(recovered / name, local if name == relative else data)
        except Exception:
            shutil.rmtree(recovered)
            raise
        return recovered

    def save(self, changes: dict[str, bytes], expected: dict[str, str | None],
             fault: str | None = None, legacy_race=None, late_legacy_race=None) -> str:
        """Journal before applying; recover rolls forward only if inputs still match."""
        if list(self.pending.iterdir()):
            raise RuntimeError("recover pending operation before new edits")
        for relative, local in changes.items():
            current = read(self.library / relative)
            if digest(current) != expected[relative]:
                self.preserve_conflict(relative.replace("/", "__"), local, current)
                return "conflict: both copies retained; book paused"

        operation = self.pending / "operation"
        operation.mkdir()
        entries = []
        for index, (relative, local) in enumerate(changes.items()):
            original = read(self.library / relative)
            if original is not None:
                replace_durable(operation / f"{index}.before", original)
            replace_durable(operation / f"{index}.after", local)
            entries.append({"path": relative, "before": digest(original),
                            "after": digest(local)})
        replace_durable(operation / "manifest.json", json.dumps(entries).encode())
        sync_dir(self.pending)
        if fault == "after_journal":
            raise OSError("injected interruption after journal")

        for index, entry in enumerate(entries):
            path = self.library / entry["path"]
            if legacy_race and index == 0:
                legacy_race(path)
            current = read(path)
            if digest(current) != entry["before"]:
                self.preserve_conflict(entry["path"].replace("/", "__"),
                                       (operation / f"{index}.after").read_bytes(), current)
                return "conflict: both copies retained; book paused"
            if late_legacy_race and index == 0:
                late_legacy_race(path)  # The unavoidable legacy compare/rename race.
            replace_durable(path, (operation / f"{index}.after").read_bytes(),
                            fault=fault == "before_replace")
            if fault == f"after_replace_{index + 1}":
                raise OSError("injected interruption after replacement")
        replace_durable(operation / "committed", b"yes")
        self.retire(operation)
        return "saved"

    def recover(self) -> str:
        operation = self.pending / "operation"
        if not operation.exists():
            return "nothing pending"
        entries = json.loads(checked(operation / "manifest.json"))
        for entry in entries:
            current = digest(read(self.library / entry["path"]))
            if current not in (entry["before"], entry["after"]):
                return "recovery blocked: external change; inspect pending journal"
        for index, entry in enumerate(entries):
            path = self.library / entry["path"]
            if digest(read(path)) == entry["before"]:
                replace_durable(path, checked(operation / f"{index}.after"))
        self.retire(operation)
        return "recovered and saved"

    def inventory(self) -> dict[str, bytes]:
        items = {}
        for path in sorted(self.library.rglob("*")):
            if path.is_symlink():
                raise ValueError("symlink requires explicit policy")
            if not path.is_file():
                continue
            relative = path.relative_to(self.library).as_posix()
            if relative.split("/")[0] in {"Backups", "Exports"}:
                continue
            items[relative] = checked(path)
        return items

    def snapshot(self, day: str, optional_drive: Path | None = None,
                 safety: bool = False, fault: str | None = None) -> str:
        if list(self.pending.iterdir()):
            raise RuntimeError("finish pending saves before snapshot")
        target = self.snapshots / (f"safety-{day}.zip" if safety else f"daily-{day}.zip")
        if target.exists():
            return "daily snapshot already completed" if not safety else "safety snapshot already exists"
        source = self.inventory()
        fingerprint = digest(json.dumps({name: digest(data) for name, data in source.items()},
                                        sort_keys=True).encode())
        latest = sorted(self.snapshots.glob("daily-*.zip"))
        if not safety and latest:
            with zipfile.ZipFile(latest[-1]) as archive:
                previous = json.loads(archive.read("control/manifest.json"))["fingerprint"]
            if previous == fingerprint:
                return "unchanged: no new daily snapshot"
        staging = target.with_suffix(".partial")
        manifest = {"fingerprint": fingerprint,
                    "files": {name: digest(data) for name, data in source.items()}}
        try:
            with zipfile.ZipFile(staging, "w", zipfile.ZIP_DEFLATED) as archive:
                for name, data in source.items():
                    archive.writestr(f"payload/{name}", data)
                archive.writestr("control/manifest.json", json.dumps(manifest))
            if fault == "partial_archive":
                raise OSError("injected incomplete archive")
            if {name: digest(data) for name, data in self.inventory().items()} != manifest["files"]:
                raise RuntimeError("library changed during snapshot")
            with zipfile.ZipFile(staging) as archive:
                for name, expected in manifest["files"].items():
                    if digest(archive.read(f"payload/{name}")) != expected:
                        raise ValueError("snapshot verification failed")
            with staging.open("rb") as handle:
                os.fsync(handle.fileno())
            os.replace(staging, target)
            sync_dir(self.snapshots)
        finally:
            staging.unlink(missing_ok=True)
        if not safety:
            daily = sorted(self.snapshots.glob("daily-*.zip"))
            for old in daily[:-14]:
                old.unlink()
            sync_dir(self.snapshots)
        if optional_drive is not None:
            try:
                optional_drive.mkdir(parents=True, exist_ok=True)
                shutil.copy2(target, optional_drive / target.name)
            except OSError:
                return "local snapshot saved; optional drive failed visibly"
        return "snapshot saved"

    def tick(self, utc_day: str) -> str:
        """The application calls this at startup and hourly while open."""
        return self.snapshot(utc_day)

    def restore(self, archive_path: Path, label: str, fault: str | None = None) -> Path:
        safety_result = self.snapshot(f"restore-{label}", safety=True, fault=fault)
        if safety_result != "snapshot saved":
            raise RuntimeError(safety_result)
        restored = self.recovery / f"recovered-{label}"
        if restored.exists():
            raise FileExistsError(restored)
        content = {}
        with zipfile.ZipFile(archive_path) as archive:
            manifest = json.loads(archive.read("control/manifest.json"))
            for name, expected in manifest["files"].items():
                path = Path(name)
                if path.is_absolute() or ".." in path.parts:
                    raise ValueError("unsafe archive path")
                data = archive.read(f"payload/{name}")
                if digest(data) != expected:
                    raise ValueError("corrupt snapshot")
                if path.name in {"library.json", "book.json", "darlings.json", "stickies.json"}:
                    json.loads(data)
                content[path] = data
        restored.mkdir()
        try:
            for path, data in content.items():
                replace_durable(restored / path, data)
        except Exception:
            shutil.rmtree(restored)
            raise
        return restored


def scenario(label: str, passed: bool) -> None:
    if not passed:
        raise AssertionError(label)
    print(f"PASS {label}")


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="leo-persistence-prototype-") as scratch:
        root = Path(scratch)
        library = root / "NEO Library"
        library.mkdir()
        replace_durable(library / "library.json", b'{"shelves":[],"unknownSetting":true}')
        book = library / "book-synthetic"
        (book / "chapters").mkdir(parents=True)
        chapter = book / "chapters" / "chapter-1.html"
        metadata = book / "book.json"
        replace_durable(chapter, b"<p>Original</p>")
        replace_durable(metadata, b'{"title":"Original","unknown":"keep"}')
        replace_durable(book / "extra.bin", b"unknown asset")
        replace_durable(library / "manifest.json", b"opaque unknown file, not JSON")
        store = Store(library, root / "local-recovery")

        try:
            replace_durable(chapter, b"<p>Partial</p>", fault=True)
        except OSError:
            pass
        scenario("failed replacement keeps old chapter", checked(chapter) == b"<p>Original</p>")
        for fault in ("after_journal", "after_replace_1", "after_replace_2"):
            old_chapter, old_meta = checked(chapter), checked(metadata)
            new_chapter = f"<p>{fault}</p>".encode()
            new_meta = json.dumps({"title": fault, "unknown": "keep"}).encode()
            try:
                store.save({"book-synthetic/chapters/chapter-1.html": new_chapter,
                            "book-synthetic/book.json": new_meta},
                           {"book-synthetic/chapters/chapter-1.html": digest(old_chapter),
                            "book-synthetic/book.json": digest(old_meta)}, fault=fault)
            except OSError:
                pass
            scenario(f"{fault} rolls forward on reopen",
                     store.recover() == "recovered and saved" and
                     checked(chapter) == new_chapter and checked(metadata) == new_meta)

        old_chapter = checked(chapter)
        try:
            store.save({"book-synthetic/chapters/chapter-1.html": b"<p>Local pending</p>"},
                       {"book-synthetic/chapters/chapter-1.html": digest(old_chapter)},
                       fault="after_journal")
        except OSError:
            pass
        replace_durable(chapter, b"<p>Sync arrived during interruption</p>")
        scenario("interrupted sync blocks replay and retains local journal",
                 store.recover().startswith("recovery blocked") and
                 checked(chapter) == b"<p>Sync arrived during interruption</p>" and
                 (store.pending / "operation" / "0.after").read_bytes() == b"<p>Local pending</p>")
        shutil.rmtree(store.pending / "operation")  # Reset disposable scenario only.

        local = b"<p>Unsaved local edit</p>"
        expected = digest(checked(chapter))
        replace_durable(chapter, b"<p>Pocket edit</p>")
        outcome = store.save({"book-synthetic/chapters/chapter-1.html": local},
                             {"book-synthetic/chapters/chapter-1.html": expected})
        scenario("observed external change preserves both versions",
                 outcome.startswith("conflict") and checked(chapter) == b"<p>Pocket edit</p>" and
                 (store.conflicts / "book-synthetic__chapters__chapter-1.html" / "local").read_bytes() == local)
        conflict_library = store.recover_conflict_to_library(
            "book-synthetic/chapters/chapter-1.html", "local-choice")
        scenario("local choice creates separate library without changing shared chapter",
                 checked(chapter) == b"<p>Pocket edit</p>" and
                 checked(conflict_library / "book-synthetic" / "chapters" / "chapter-1.html") == local and
                 checked(conflict_library / "book-synthetic" / "book.json") == checked(metadata))
        late_result = store.save(
            {"book-synthetic/chapters/chapter-1.html": b"<p>Native replacement</p>"},
            {"book-synthetic/chapters/chapter-1.html": digest(checked(chapter))},
            late_legacy_race=lambda path: replace_durable(path, b"<p>Uncooperative late edit</p>"))
        scenario("late legacy race can overwrite an unseen edit",
                 late_result == "saved" and checked(chapter) == b"<p>Native replacement</p>")
        try:
            checked(book / "chapters" / "missing.html")
        except FileNotFoundError:
            scenario("missing input reported, never converted to empty", True)
        replace_durable(metadata, b"{broken")
        try:
            checked(metadata)
        except ValueError:
            scenario("corrupt JSON reported, never converted to empty", True)
        replace_durable(metadata, b'{"title":"Restored","unknown":"keep"}')

        (library / "Exports").mkdir()
        replace_durable(library / "Exports" / "generated.pdf", b"not a library source")
        (library / "Backups").mkdir()
        replace_durable(library / "Backups" / "legacy.zip", b"existing legacy backup")

        scenario("first daily snapshot includes unknown asset",
                 store.tick("2026-09-01") == "snapshot saved" and
                 "book-synthetic/extra.bin" in store.inventory() and
                 "manifest.json" in store.inventory() and
                 "Exports/generated.pdf" not in store.inventory() and
                 "Backups/legacy.zip" not in store.inventory())
        scenario("unchanged day does not duplicate snapshot",
                 store.tick("2026-09-02") == "unchanged: no new daily snapshot")
        replace_durable(chapter, b"<p>Next day</p>")
        try:
            store.snapshot("2026-09-02", fault="partial_archive")
        except OSError:
            pass
        scenario("failed snapshot retains prior valid one",
                 len(list(store.snapshots.glob("daily-*.zip"))) == 1)
        unavailable_drive = root / "unavailable-drive"
        unavailable_drive.write_bytes(b"not a directory")
        scenario("optional drive failure does not block local snapshot",
                 store.snapshot("2026-09-02", optional_drive=unavailable_drive) ==
                 "local snapshot saved; optional drive failed visibly")
        scenario("same day does not replace completed snapshot",
                 store.tick("2026-09-02") == "daily snapshot already completed")
        for day in range(3, 17):
            replace_durable(chapter, f"<p>day {day}</p>".encode())
            store.tick(f"2026-09-{day:02d}")
        scenario("long-running hourly tick retains fourteen completed daily snapshots",
                 len(list(store.snapshots.glob("daily-*.zip"))) == 14)
        before = checked(chapter)
        old_archive = sorted(store.snapshots.glob("daily-*.zip"))[0]
        try:
            store.restore(old_archive, "blocked", fault="partial_archive")
        except OSError:
            pass
        scenario("failed safety snapshot aborts restoration",
                 not (store.recovery / "recovered-blocked").exists() and
                 checked(chapter) == before)
        recovered = store.restore(old_archive, "review")
        scenario("restore produces separate inspectable library and safety snapshot",
                 checked(chapter) == before and
                 checked(recovered / "book-synthetic" / "chapters" / "chapter-1.html") != before and
                 checked(recovered / "manifest.json") == b"opaque unknown file, not JSON" and
                 len(list(store.snapshots.glob("safety-*.zip"))) == 1)
        print("LIMIT: A legacy writer may change a file between final hash check and rename.")
        print("LIMIT: Syncthing may expose a mixed multi-file state before operation completes.")
        print("LIMIT: Simulated interruption does not prove power-loss or filesystem durability.")


if __name__ == "__main__":
    main()
