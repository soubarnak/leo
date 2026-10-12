#!/usr/bin/env python3
"""Record automated evidence for the stable-release parity matrix (#39).

Three modes, all writing one JSON record per run into docs/evidence/:

* default: run ctest in a build directory (excluding the parity checker
  itself) with --output-junit and record every test's outcome;
* --junit: import a JUnit file produced elsewhere (for example inside the
  Debian packaging container, where dh_auto_test / ctest ran);
* --kind script: record the outcome of a scripted drill such as the Debian
  package lifecycle check.

Each record carries the git commit it ran at, whether the tree was clean,
the host operating system, and the Debian version when the host is Debian —
the parity checker only trusts ``pass`` rows backed by a Debian record at
the matrix's build commit.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

EXCLUDE_PATTERN = "^leo-parity-matrix$"


class RecorderError(Exception):
    """The run could not be recorded."""


def parse_junit(xml_text: str):
    try:
        root = ET.fromstring(xml_text)
    except ET.ParseError as exc:
        raise RecorderError("JUnit file is not valid XML: %s" % exc)
    tests = {}
    for case in root.iter("testcase"):
        name = case.get("name")
        if not name:
            continue
        if case.find("failure") is not None or case.find("error") is not None:
            tests[name] = "fail"
        elif case.find("skipped") is not None:
            tests[name] = "skip"
        else:
            tests[name] = "pass"
    if not tests:
        raise RecorderError("JUnit file contains no test cases")
    return tests


def read_os_release(path: str):
    pretty = None
    ident = None
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line.startswith("PRETTY_NAME="):
                pretty = line.split("=", 1)[1].strip().strip('"')
            elif line.startswith("ID="):
                ident = line.split("=", 1)[1].strip().strip('"')
    if pretty is None and ident is None:
        raise RecorderError("os-release lacks PRETTY_NAME and ID: %s" % path)
    return pretty, ident


def debian_version(pretty, ident):
    if ident == "debian":
        return pretty
    return None


def git_state(repo_root: str):
    sha = subprocess.run(
        ["git", "-C", repo_root, "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    status = subprocess.run(
        ["git", "-C", repo_root, "status", "--porcelain"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    return sha, not status.strip()


def _timestamp():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _base_record(commit, tree_clean, os_pretty, os_id, recorded_at):
    return {
        "recorded_at": recorded_at or _timestamp(),
        "commit": commit,
        "tree_clean": tree_clean,
        "os_pretty": os_pretty,
        "os_id": os_id,
        "debian_version": debian_version(os_pretty, os_id),
    }


def build_ctest_record(tests, command, commit, tree_clean, os_pretty, os_id, recorded_at=None):
    record = _base_record(commit, tree_clean, os_pretty, os_id, recorded_at)
    record["kind"] = "ctest-run"
    record["command"] = command
    record["tests"] = tests
    return record


def build_script_record(name, result, commit, os_pretty, os_id, log, recorded_at=None, tree_clean=True):
    record = _base_record(commit, tree_clean, os_pretty, os_id, recorded_at)
    record["kind"] = "script"
    record["name"] = name
    record["result"] = result
    record["log_excerpt"] = "\n".join(log.splitlines()[-40:])
    return record


def run_ctest(build_dir: str, command: str):
    with tempfile.NamedTemporaryFile(suffix=".xml", delete=False) as handle:
        junit_path = handle.name
    full_command = command + " --output-junit " + junit_path
    proc = subprocess.run(full_command, shell=True, check=False)
    try:
        with open(junit_path, encoding="utf-8") as handle:
            xml_text = handle.read()
    finally:
        os.unlink(junit_path)
    if not xml_text.strip():
        raise RecorderError("ctest produced no JUnit output (exit %d)" % proc.returncode)
    return xml_text, proc.returncode, full_command


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--repo-root", default=".")
    parser.add_argument("--os-release", default="/etc/os-release")
    parser.add_argument("--commit", default=None)
    parser.add_argument("--command", default=None)
    # default (ctest run) mode
    parser.add_argument("--build-dir", default=None)
    parser.add_argument("--ctest-args", default=None)
    # import mode
    parser.add_argument("--junit", default=None)
    # script mode
    parser.add_argument("--kind", choices=["script"], default=None)
    parser.add_argument("--name", default=None)
    parser.add_argument("--result", choices=["pass", "fail"], default=None)
    parser.add_argument("--log", default=None)
    args = parser.parse_args(argv)

    try:
        os_pretty, os_id = read_os_release(args.os_release)
        if args.commit:
            commit = args.commit
            tree_clean = None
        else:
            commit, tree_clean = git_state(args.repo_root)

        if args.kind == "script":
            if not (args.name and args.result and args.log):
                raise RecorderError("--kind script needs --name, --result and --log")
            with open(args.log, encoding="utf-8", errors="replace") as handle:
                log_text = handle.read()
            record = build_script_record(
                name=args.name,
                result=args.result,
                commit=commit,
                os_pretty=os_pretty,
                os_id=os_id,
                log=log_text,
                tree_clean=bool(tree_clean),
            )
            exit_code = 0 if args.result == "pass" else 1
        else:
            if args.junit:
                with open(args.junit, encoding="utf-8") as handle:
                    xml_text = handle.read()
                command = args.command or ("import junit %s" % args.junit)
                exit_code = 0
            elif args.build_dir:
                command = args.ctest_args or (
                    "ctest --test-dir %s -j --output-on-failure -E '%s'"
                    % (args.build_dir, EXCLUDE_PATTERN)
                )
                xml_text, ctest_code, command = run_ctest(args.build_dir, command)
                exit_code = ctest_code
            else:
                raise RecorderError("choose a mode: --build-dir, --junit or --kind script")
            tests = parse_junit(xml_text)
            record = build_ctest_record(
                tests=tests,
                command=command,
                commit=commit,
                tree_clean=bool(tree_clean),
                os_pretty=os_pretty,
                os_id=os_id,
            )
            if exit_code != 0:
                exit_code = 1

        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        with open(args.output, "w", encoding="utf-8") as handle:
            json.dump(record, handle, indent=2, sort_keys=True)
            handle.write("\n")
        failed = sorted(
            name for name, value in record.get("tests", {}).items() if value == "fail"
        )
        summary = "%d test(s)" % len(record.get("tests", {}))
        if record.get("kind") == "script":
            summary = "script result %s" % record["result"]
        print(
            "recorded %s at %s on %s (Debian: %s): %s"
            % (
                record["kind"],
                record["commit"][:12],
                record["os_pretty"],
                record["debian_version"] or "no",
                summary,
            )
        )
        if failed:
            print("failed tests: %s" % " ".join(failed), file=sys.stderr)
        return exit_code
    except (RecorderError, OSError, subprocess.CalledProcessError, ValueError) as exc:
        print("record-parity-evidence: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
