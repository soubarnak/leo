#!/usr/bin/env python3
"""Unit tests for check_parity_matrix.py.

TDD seam for the stable-release parity verification tooling (#39):
run with `python3 scripts/test_check_parity_matrix.py`.
"""

import hashlib
import os
import sys
import tempfile
import textwrap
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_parity_matrix as cpm  # noqa: E402


INVENTORY = textwrap.dedent(
    """\
    ## Feature inventory and acceptance evidence to collect

    | Area | Existing behavior | Source | Required proof for the rebuild |
    | --- | --- | --- | --- |
    | Library | Bookshelves | a | proof |
    | Export | Formats | b | proof |
    """
)

MATRIX = textwrap.dedent(
    """\
    # Stable-release parity matrix

    Release label: preview
    Platform claim: Debian stable amd64 XFCE/X11
    Wayland/niri: not claimed
    Default Debian version: Debian GNU/Linux 13 (trixie)

    | ID | Feature row | Fixture | Action | Semantic expectation | Visible expectation | Automated evidence | Build commit | Debian version | Status | Notes |
    | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
    | F01 | Library organization | {fixture} | Open and save | graph unchanged | shelf order identical | ctest: leo-library-organization | {commit} | Debian GNU/Linux 13 (trixie) | pass | none |
    | F02 | Export formats | {fixture} | Export a book | scaffolding omitted | file appears | pending: leo-manuscript-export | - | pending run | pending-evidence | none |
    | S01 | Writing feel sign-off | {fixture} | Human writing session | prose unchanged | maintainer notes recorded | manual: docs/manual/stable-release-signoff.md | - | pending session | pending-signoff | none |
    """
)


class FakeGit:
    def __init__(self, commits=None, dirty=None):
        self.commits = set(commits or [])
        self.dirty = set(dirty or [])

    def commit_exists(self, sha):
        return sha in self.commits

    def build_paths_clean(self, sha):
        return sha not in self.dirty


def write_tree(root, files):
    for rel, content in files.items():
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(content)


class ParseMatrixTest(unittest.TestCase):
    def test_parse_matrix_valid(self):
        header, rows = cpm.parse_matrix(MATRIX.format(fixture="a.txt=x", commit="a" * 40))
        self.assertEqual(header["Release label"], "preview")
        self.assertEqual(header["Wayland/niri"], "not claimed")
        self.assertEqual(len(rows), 3)
        self.assertEqual(rows[0].id, "F01")
        self.assertEqual(rows[0].status, "pass")
        self.assertEqual(rows[2].status, "pending-signoff")

    def test_parse_matrix_missing_column(self):
        broken = textwrap.dedent(
            """\
            | ID | Feature row | Fixture | Action | Semantic expectation | Visible expectation | Automated evidence | Build commit | Debian version | Status |
            | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
            | F01 | a | b | c | d | e | f | g | h | pass |
            """
        )
        with self.assertRaises(cpm.MatrixError):
            cpm.parse_matrix(broken)

    def test_parse_matrix_requires_table(self):
        with self.assertRaises(cpm.MatrixError):
            cpm.parse_matrix("# nothing here\n")


class FixtureHashTest(unittest.TestCase):
    def test_file_and_directory_hashes(self):
        with tempfile.TemporaryDirectory() as root:
            write_tree(root, {"f/a.txt": "alpha", "f/b.txt": "beta"})
            file_hash = cpm.compute_fixture_hash(root, "f/a.txt")
            self.assertEqual(file_hash, hashlib.sha256(b"alpha").hexdigest())
            first = cpm.compute_fixture_hash(root, "f")
            self.assertEqual(first, cpm.compute_fixture_hash(root, "f"))
            with open(os.path.join(root, "f/b.txt"), "w", encoding="utf-8") as handle:
                handle.write("changed")
            self.assertNotEqual(first, cpm.compute_fixture_hash(root, "f"))

    def test_missing_path_raises(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaises(cpm.MatrixError):
                cpm.compute_fixture_hash(root, "nope.txt")


class RowChecksTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mktemp()
        os.makedirs(self.root)
        write_tree(self.root, {"a.txt": "alpha", "docs/manual/stable-release-signoff.md": "sign"})
        self.fixture = "a.txt=" + hashlib.sha256(b"alpha").hexdigest()
        self.header = {"Release label": "preview", "Wayland/niri": "not claimed"}
        self.run = {
            "commit": "a" * 40,
            "debian_version": "Debian GNU/Linux 13 (trixie)",
            "tests": {"leo-library-organization": "pass"},
        }
        self.addCleanup(self._cleanup)

    def _cleanup(self):
        import shutil

        shutil.rmtree(self.root, ignore_errors=True)

    def row(self, **overrides):
        values = dict(
            id="F01",
            feature="Library organization",
            fixtures=self.fixture,
            action="Open and save",
            semantic="graph unchanged",
            visible="shelf order identical",
            evidence="ctest: leo-library-organization",
            build_commit="a" * 40,
            debian_version="Debian GNU/Linux 13 (trixie)",
            status="pass",
            notes="none",
        )
        values.update(overrides)
        return cpm.Row(**values)

    def check(self, row, runs=None, git=None):
        return cpm.check_row(row, runs if runs is not None else [self.run], git or FakeGit(commits={"a" * 40}), self.root)

    def test_pass_row_ok(self):
        self.assertEqual(self.check(self.row()), [])

    def test_missing_field_rejected(self):
        errors = self.check(self.row(visible=""))
        self.assertTrue(any("Visible expectation" in error for error in errors))

    def test_invalid_status_rejected(self):
        errors = self.check(self.row(status="verified"))
        self.assertTrue(errors)

    def test_fixture_hash_mismatch_rejected(self):
        bad = self.row(fixtures="a.txt=" + "0" * 64)
        errors = self.check(bad)
        self.assertTrue(any("sha256" in error for error in errors))

    def test_missing_fixture_rejected(self):
        bad = self.row(fixtures="gone.txt=" + "0" * 64)
        errors = self.check(bad)
        self.assertTrue(any("missing" in error for error in errors))

    def test_pass_requires_debian_evidence(self):
        void_run = dict(self.run, debian_version=None)
        errors = self.check(self.row(), runs=[void_run])
        self.assertTrue(any("Debian evidence" in error for error in errors))

    def test_pass_requires_test_passing_in_run(self):
        failed_run = {"commit": "a" * 40, "debian_version": self.run["debian_version"], "tests": {"leo-library-organization": "fail"}}
        errors = self.check(self.row(), runs=[failed_run])
        self.assertTrue(any("did not pass" in error for error in errors))

    def test_pass_requires_matching_commit(self):
        errors = self.check(self.row(build_commit="b" * 40), git=FakeGit(commits={"b" * 40}))
        self.assertTrue(any("evidence" in error for error in errors))

    def test_pass_requires_clean_build_paths(self):
        errors = self.check(self.row(), git=FakeGit(commits={"a" * 40}, dirty={"a" * 40}))
        self.assertTrue(any("differ" in error for error in errors))

    def test_pass_requires_existing_commit(self):
        errors = self.check(self.row(), git=FakeGit(commits=set()))
        self.assertTrue(any("unknown commit" in error for error in errors))

    def test_pass_evidence_must_be_automated(self):
        errors = self.check(self.row(evidence="manual: docs/manual/x.md"))
        self.assertTrue(any("automated evidence" in error for error in errors))

    def test_pending_row_ok_with_manual_evidence(self):
        row = self.row(
            status="pending-signoff",
            evidence="manual: docs/manual/stable-release-signoff.md",
            build_commit="-",
            debian_version="pending session",
        )
        self.assertEqual(self.check(row), [])

    def test_pending_row_with_unknown_status_segment_rejected(self):
        row = self.row(status="pending-evidence", evidence="nonsense", build_commit="-")
        errors = self.check(row)
        self.assertTrue(errors)

    def test_script_evidence_matched(self):
        record = {
            "kind": "script",
            "name": "tests/debian_package_lifecycle.sh",
            "result": "pass",
            "commit": "a" * 40,
            "debian_version": self.run["debian_version"],
        }
        row = self.row(evidence="script: tests/debian_package_lifecycle.sh")
        self.assertEqual(self.check(row, runs=[record]), [])

    def test_script_evidence_unmatched_rejected(self):
        row = self.row(evidence="script: tests/debian_package_lifecycle.sh")
        errors = self.check(row, runs=[self.run])
        self.assertTrue(any("script evidence" in error for error in errors))


class ReleaseLabelTest(unittest.TestCase):
    def row(self, status):
        return cpm.Row(
            id="F01", feature="Library", fixtures="f=x", action="a", semantic="s", visible="v",
            evidence="ctest: t", build_commit="-", debian_version="d", status=status, notes="n",
        )

    def test_pending_rows_forbid_stable_label(self):
        errors = cpm.check_release_label({"Release label": "stable", "Wayland/niri": "not claimed"}, [self.row("pending-manual")])
        self.assertTrue(errors)

    def test_all_pass_allows_stable_label(self):
        errors = cpm.check_release_label({"Release label": "stable", "Wayland/niri": "not claimed"}, [self.row("pass")])
        self.assertEqual(errors, [])

    def test_all_pass_keeps_preview_allowed(self):
        errors = cpm.check_release_label({"Release label": "preview", "Wayland/niri": "not claimed"}, [self.row("pass")])
        self.assertEqual(errors, [])

    def test_wayland_claim_rejected(self):
        errors = cpm.check_release_label({"Release label": "preview", "Wayland/niri": "supported"}, [self.row("pass")])
        self.assertTrue(any("Wayland" in error for error in errors))

    def test_unknown_label_rejected(self):
        errors = cpm.check_release_label({"Release label": "beta", "Wayland/niri": "not claimed"}, [self.row("pass")])
        self.assertTrue(errors)


class InventoryCoverageTest(unittest.TestCase):
    def test_all_areas_covered(self):
        matrix = "| ID | Feature row | Fixture | Action | Semantic expectation | Visible expectation | Automated evidence | Build commit | Debian version | Status | Notes |\n| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n| F01 | Library organization | x | a | s | v | e | c | d | pass | n |\n| F02 | Export formats | x | a | s | v | e | c | d | pass | n |\n"
        self.assertEqual(cpm.inventory_coverage(matrix, INVENTORY), [])

    def test_missing_area_reported(self):
        matrix = "| ID | Feature row | Fixture | Action | Semantic expectation | Visible expectation | Automated evidence | Build commit | Debian version | Status | Notes |\n| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n| F01 | Library organization | x | a | s | v | e | c | d | pass | n |\n"
        errors = cpm.inventory_coverage(matrix, INVENTORY)
        self.assertTrue(any("Export" in error for error in errors))


class MainTest(unittest.TestCase):
    def test_main_end_to_end(self):
        commit = "a" * 40
        with tempfile.TemporaryDirectory() as root:
            write_tree(
                root,
                {
                    "a.txt": "alpha",
                    "docs/manual/stable-release-signoff.md": "sign",
                    "docs/research/void-native-parity.md": INVENTORY,
                    "docs/parity-matrix.md": MATRIX.format(fixture="a.txt=" + hashlib.sha256(b"alpha").hexdigest(), commit=commit),
                    "docs/evidence/run.json": (
                        '{"kind": "ctest-run", "commit": "%s", "debian_version": "Debian GNU/Linux 13 (trixie)",'
                        ' "tests": {"leo-library-organization": "pass"}}' % commit
                    ),
                },
            )

            code = cpm.main(
                [
                    "--repo-root", root,
                    "--matrix", os.path.join(root, "docs/parity-matrix.md"),
                    "--inventory", os.path.join(root, "docs/research/void-native-parity.md"),
                    "--evidence-dir", os.path.join(root, "docs/evidence"),
                    "--build-paths", "a.txt",
                ],
                git_probe=FakeGit(commits={commit}),
            )
            self.assertEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
