#!/usr/bin/env python3
"""Validate the stable-release parity matrix (GitHub issue #39).

The matrix in docs/parity-matrix.md is the traceability record for the
NEO feature inventory: every row must carry a fixture (path + sha256),
an action, semantic and visible expectations, automated evidence, a build
commit, a Debian version and a status.  A row may be ``pass`` only when:

* its automated evidence (ctest tests or a recorded script drill) passed in
  a recorded run that ran on Debian, at the matrix's build commit;
* the build commit exists in git and the build-relevant paths of the tree
  still match that commit (so the evidence describes this tree);
* the Debian version column matches the recorded run's Debian version;
* the fixture paths still hash to the recorded sha256 values.

While any row is not ``pass`` the release label must stay ``preview`` and
Wayland/niri support must remain unclaimed.

Exit codes: 0 valid, 1 findings, 2 usage/read error.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass

STATUS_PASS = "pass"
STATUSES = (STATUS_PASS, "pending-evidence", "pending-manual", "pending-signoff")
EVIDENCE_PREFIXES = ("ctest:", "script:", "manual:", "pending:")
ROW_COLUMNS = (
    "ID",
    "Feature row",
    "Fixture",
    "Action",
    "Semantic expectation",
    "Visible expectation",
    "Automated evidence",
    "Build commit",
    "Debian version",
    "Status",
    "Notes",
)
FIELD_NAMES = dict(zip(ROW_COLUMNS, ROW_COLUMNS))
REQUIRED_HEADER_KEYS = ("Release label", "Wayland/niri")
DEFAULT_BUILD_PATHS = (
    "CMakeLists.txt",
    "src",
    "tests",
    "debian",
    "resources",
    "scripts",
    ".github/workflows/debian-package.yml",
)
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
FULL_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
HEADER_RE = re.compile(r"^([A-Za-z][A-Za-z0-9 /]*):\s+(.+)$")


class MatrixError(Exception):
    """A structural problem in the matrix, inventory or evidence files."""


@dataclass
class Row:
    id: str
    feature: str
    fixtures: str
    action: str
    semantic: str
    visible: str
    evidence: str
    build_commit: str
    debian_version: str
    status: str
    notes: str


def parse_matrix(text: str):
    lines = text.splitlines()
    table_start = None
    for index, line in enumerate(lines):
        if line.startswith("| ID |"):
            table_start = index
            break
    if table_start is None:
        raise MatrixError("matrix has no '| ID |' table header")

    header = {}
    for line in lines[:table_start]:
        if line.startswith("#"):
            continue
        match = HEADER_RE.match(line.strip())
        if match:
            header[match.group(1).strip()] = match.group(2).strip()
    for key in REQUIRED_HEADER_KEYS:
        if key not in header:
            raise MatrixError("matrix header is missing '%s:'" % key)

    rows = []
    for line in lines[table_start:]:
        stripped = line.strip()
        if not stripped.startswith("|"):
            if rows:
                break
            continue
        cells = [cell.strip() for cell in stripped.strip("|").split("|")]
        if cells and all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells):
            continue
        if cells[0] == "ID":
            continue
        if len(cells) != len(ROW_COLUMNS):
            raise MatrixError(
                "matrix row has %d columns, expected %d: %s"
                % (len(cells), len(ROW_COLUMNS), stripped[:80])
            )
        rows.append(Row(*cells))
    if not rows:
        raise MatrixError("matrix contains no data rows")
    return header, rows


def fixture_entries(cell: str):
    entries = []
    for part in cell.split(", "):
        if "=" not in part:
            raise MatrixError("fixture entry is not 'path=sha256': %s" % part)
        path, digest = part.rsplit("=", 1)
        if not SHA256_RE.match(digest):
            raise MatrixError("fixture digest is not sha256: %s" % part)
        entries.append((path, digest))
    return entries


def compute_fixture_hash(repo_root: str, relpath: str) -> str:
    path = os.path.join(repo_root, relpath)
    if not os.path.exists(path):
        raise MatrixError("fixture path is missing: %s" % relpath)
    if os.path.isfile(path):
        with open(path, "rb") as handle:
            return hashlib.sha256(handle.read()).hexdigest()
    digest = hashlib.sha256()
    for base, dirnames, filenames in os.walk(path):
        dirnames.sort()
        for name in sorted(filenames):
            full = os.path.join(base, name)
            rel = os.path.relpath(full, path)
            with open(full, "rb") as handle:
                file_hash = hashlib.sha256(handle.read()).hexdigest()
            digest.update(("%s  %s\n" % (file_hash, rel)).encode("utf-8"))
    return digest.hexdigest()


def evidence_segments(cell: str):
    segments = []
    for segment in cell.split("; "):
        matched = False
        for prefix in EVIDENCE_PREFIXES:
            if segment.startswith(prefix):
                rest = segment[len(prefix):].strip()
                if not rest:
                    raise MatrixError("empty evidence segment: %s" % segment)
                segments.append((prefix, rest))
                matched = True
                break
        if not matched:
            raise MatrixError(
                "evidence must start with %s: %s"
                % ("/".join(EVIDENCE_PREFIXES), segment)
            )
    return segments


def check_row(row: Row, runs, git, repo_root: str):
    errors = []
    prefix = "row %s:" % row.id

    values = {
        "ID": row.id,
        "Feature row": row.feature,
        "Fixture": row.fixtures,
        "Action": row.action,
        "Semantic expectation": row.semantic,
        "Visible expectation": row.visible,
        "Automated evidence": row.evidence,
        "Build commit": row.build_commit,
        "Debian version": row.debian_version,
        "Status": row.status,
        "Notes": row.notes,
    }
    for column, value in values.items():
        if not value.strip():
            errors.append("%s missing %s" % (prefix, FIELD_NAMES[column]))
    if errors:
        return errors

    if row.status not in STATUSES:
        errors.append("%s unknown status '%s'" % (prefix, row.status))

    try:
        for relpath, expected in fixture_entries(row.fixtures):
            try:
                actual = compute_fixture_hash(repo_root, relpath)
            except MatrixError:
                errors.append("%s fixture is missing: %s" % (prefix, relpath))
                continue
            if actual != expected:
                errors.append(
                    "%s fixture sha256 mismatch for %s (recorded %s, tree %s)"
                    % (prefix, relpath, expected[:12], actual[:12])
                )
    except MatrixError as exc:
        errors.append("%s %s" % (prefix, exc))

    try:
        segments = evidence_segments(row.evidence)
    except MatrixError as exc:
        errors.append("%s %s" % (prefix, exc))
        return errors

    if row.status != STATUS_PASS:
        return errors

    automated = [seg for seg in segments if seg[0] in ("ctest:", "script:")]
    if not automated:
        errors.append(
            "%s pass status requires automated evidence (ctest:/script:), got '%s'"
            % (prefix, row.evidence)
        )
        return errors

    if not FULL_SHA_RE.match(row.build_commit):
        errors.append(
            "%s pass status requires a full 40-character build commit, got '%s'"
            % (prefix, row.build_commit)
        )
        return errors
    if not git.commit_exists(row.build_commit):
        errors.append("%s unknown commit %s" % (prefix, row.build_commit))
    elif not git.build_paths_clean(row.build_commit):
        errors.append(
            "%s build paths in the working tree differ from build commit %s"
            % (prefix, row.build_commit)
        )

    for kind, rest in automated:
        if kind == "ctest:":
            names = rest.split()
            matching = [
                run
                for run in runs
                if "tests" in run
                and run.get("commit") == row.build_commit
                and run.get("debian_version")
            ]
            chosen = None
            for run in matching:
                if all(run["tests"].get(name) == "pass" for name in names):
                    chosen = run
                    break
            if not matching:
                errors.append(
                    "%s no passing Debian evidence for build commit %s"
                    % (prefix, row.build_commit)
                )
                continue
            if chosen is None:
                errors.append(
                    "%s tests %s did not pass in the Debian evidence run at %s"
                    % (prefix, " ".join(names), row.build_commit)
                )
                continue
            if chosen["debian_version"] != row.debian_version:
                errors.append(
                    "%s Debian version '%s' does not match recorded run '%s'"
                    % (prefix, row.debian_version, chosen["debian_version"])
                )
        else:
            script_name = rest
            chosen = None
            for run in runs:
                if (
                    run.get("kind") == "script"
                    and run.get("name") == script_name
                    and run.get("result") == "pass"
                    and run.get("commit") == row.build_commit
                    and run.get("debian_version")
                ):
                    chosen = run
                    break
            if chosen is None:
                errors.append(
                    "%s no passing script evidence for %s at build commit %s"
                    % (prefix, script_name, row.build_commit)
                )
                continue
            if chosen["debian_version"] != row.debian_version:
                errors.append(
                    "%s Debian version '%s' does not match recorded script run '%s'"
                    % (prefix, row.debian_version, chosen["debian_version"])
                )
    return errors


def check_release_label(header, rows):
    errors = []
    label = header.get("Release label", "").strip()
    if label not in ("preview", "stable"):
        errors.append("release label must be 'preview' or 'stable', got '%s'" % label)
    elif label == "stable":
        pending = [row.id for row in rows if row.status != STATUS_PASS]
        if pending:
            errors.append(
                "release label 'stable' is forbidden while rows are not pass: %s"
                % ", ".join(pending)
            )
    claim = header.get("Wayland/niri", "").strip().lower()
    if claim != "not claimed":
        errors.append(
            "Wayland/niri support must remain unclaimed, found '%s'"
            % header.get("Wayland/niri", "")
        )
    return errors


def inventory_coverage(matrix_text: str, inventory_text: str):
    errors = []
    rows = []
    for line in matrix_text.splitlines():
        stripped = line.strip()
        if stripped.startswith("|") and not stripped.startswith("| ID |"):
            cells = [cell.strip() for cell in stripped.strip("|").split("|")]
            if cells and all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells):
                continue
            if len(cells) == len(ROW_COLUMNS):
                rows.append(cells)
    areas = []
    for line in inventory_text.splitlines():
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        cells = [cell.strip() for cell in stripped.strip("|").split("|")]
        if not cells or cells[0] in ("Area", "") or re.fullmatch(r":?-{3,}:?", cells[0]):
            continue
        areas.append(cells[0])
    if not areas:
        raise MatrixError("inventory table has no feature areas")
    for area in areas:
        found = False
        for cells in rows:
            haystack = "%s %s" % (cells[1], cells[10] if len(cells) > 10 else "")
            if area.lower() in haystack.lower():
                found = True
                break
        if not found:
            errors.append("inventory area '%s' has no matrix row" % area)
    return errors


def load_evidence(evidence_dir: str):
    if not os.path.isdir(evidence_dir):
        return []
    runs = []
    for name in sorted(os.listdir(evidence_dir)):
        if not name.endswith(".json"):
            continue
        path = os.path.join(evidence_dir, name)
        try:
            with open(path, encoding="utf-8") as handle:
                run = json.load(handle)
        except (OSError, ValueError) as exc:
            raise MatrixError("evidence file %s is unreadable: %s" % (name, exc))
        run.setdefault("file", name)
        if "commit" not in run or "debian_version" not in run:
            raise MatrixError("evidence file %s lacks commit/debian_version" % name)
        kind = run.get("kind")
        if kind == "script":
            if "name" not in run or "result" not in run:
                raise MatrixError("evidence file %s lacks script name/result" % name)
        elif "tests" in run:
            if not isinstance(run["tests"], dict):
                raise MatrixError("evidence file %s has a non-map tests entry" % name)
        else:
            raise MatrixError("evidence file %s has unknown kind" % name)
        runs.append(run)
    return runs


class RealGit:
    def __init__(self, repo_root: str, build_paths=None):
        self.repo_root = repo_root
        self.build_paths = list(build_paths or DEFAULT_BUILD_PATHS)

    def _git(self, *args):
        return subprocess.run(
            ["git", "-C", self.repo_root] + list(args),
            capture_output=True,
            text=True,
            check=False,
        )

    def commit_exists(self, sha: str) -> bool:
        return self._git("rev-parse", "--verify", "--quiet", "%s^{commit}" % sha).returncode == 0

    def build_paths_clean(self, sha: str) -> bool:
        diff = self._git("diff", "--quiet", sha, "--", *self.build_paths)
        if diff.returncode != 0:
            return False
        status = self._git("status", "--porcelain", "--", *self.build_paths)
        return status.returncode == 0 and not status.stdout.strip()


def main(argv=None, git_probe=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=".")
    parser.add_argument("--matrix", default=None)
    parser.add_argument("--inventory", default=None)
    parser.add_argument("--evidence-dir", default=None)
    parser.add_argument("--build-paths", nargs="+", default=None)
    args = parser.parse_args(argv)

    root = os.path.abspath(args.repo_root)
    matrix_path = args.matrix or os.path.join(root, "docs", "parity-matrix.md")
    inventory_path = args.inventory or os.path.join(
        root, "docs", "research", "void-native-parity.md"
    )
    evidence_dir = args.evidence_dir or os.path.join(root, "docs", "evidence")
    build_paths = args.build_paths or list(DEFAULT_BUILD_PATHS)

    try:
        with open(matrix_path, encoding="utf-8") as handle:
            matrix_text = handle.read()
        with open(inventory_path, encoding="utf-8") as handle:
            inventory_text = handle.read()
        header, rows = parse_matrix(matrix_text)
        runs = load_evidence(evidence_dir)
    except (OSError, MatrixError) as exc:
        print("parity-matrix: %s" % exc, file=sys.stderr)
        return 2

    git = git_probe or RealGit(root, build_paths)

    errors = []
    errors.extend(inventory_coverage(matrix_text, inventory_text))
    errors.extend(check_release_label(header, rows))
    for row in rows:
        errors.extend(check_row(row, runs, git, root))

    if errors:
        for error in errors:
            print("parity-matrix: %s" % error, file=sys.stderr)
        print("parity-matrix: %d finding(s)" % len(errors), file=sys.stderr)
        return 1
    print(
        "parity-matrix: %d rows checked, %d recorded run(s), all findings clean"
        % (len(rows), len(runs))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
