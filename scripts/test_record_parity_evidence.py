#!/usr/bin/env python3
"""Unit tests for record_parity_evidence.py (#39)."""

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import record_parity_evidence as rpe  # noqa: E402

JUNIT = """<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="Linux-c++" tests="3" failures="1" disabled="0" skipped="1">
  <testcase name="leo-pass" classname="leo-pass" time="0.1" status="run"/>
  <testcase name="leo-fail" classname="leo-fail" time="0.2" status="run">
    <failure message="boom">details</failure>
  </testcase>
  <testcase name="leo-skip" classname="leo-skip" time="0.0" status="run">
    <skipped/>
  </testcase>
</testsuite>
"""

DEBIAN_OS = 'PRETTY_NAME="Debian GNU/Linux 13 (trixie)"\nID=debian\n'
VOID_OS = 'NAME="Void"\nID="void"\nPRETTY_NAME="Void Linux"\n'


class ParseJUnitTest(unittest.TestCase):
    def test_statuses(self):
        self.assertEqual(
            rpe.parse_junit(JUNIT),
            {"leo-pass": "pass", "leo-fail": "fail", "leo-skip": "skip"},
        )

    def test_empty_suite_rejected(self):
        with self.assertRaises(rpe.RecorderError):
            rpe.parse_junit("<testsuite name='x' tests='0'></testsuite>")


class OsReleaseTest(unittest.TestCase):
    def test_debian_detected(self):
        with tempfile.NamedTemporaryFile("w", suffix="-release", delete=False) as handle:
            handle.write(DEBIAN_OS)
            path = handle.name
        self.addCleanup(os.unlink, path)
        pretty, ident = rpe.read_os_release(path)
        self.assertEqual(pretty, "Debian GNU/Linux 13 (trixie)")
        self.assertEqual(rpe.debian_version(pretty, ident), pretty)

    def test_non_debian_is_none(self):
        with tempfile.NamedTemporaryFile("w", suffix="-release", delete=False) as handle:
            handle.write(VOID_OS)
            path = handle.name
        self.addCleanup(os.unlink, path)
        pretty, ident = rpe.read_os_release(path)
        self.assertEqual(rpe.debian_version(pretty, ident), None)


class RecordTest(unittest.TestCase):
    def test_ctest_record_fields(self):
        record = rpe.build_ctest_record(
            tests=rpe.parse_junit(JUNIT),
            command="ctest -j",
            commit="a" * 40,
            tree_clean=True,
            os_pretty="Debian GNU/Linux 13 (trixie)",
            os_id="debian",
            recorded_at="2026-10-12T00:00:00Z",
        )
        self.assertEqual(record["kind"], "ctest-run")
        self.assertEqual(record["commit"], "a" * 40)
        self.assertEqual(record["debian_version"], "Debian GNU/Linux 13 (trixie)")
        self.assertEqual(record["tests"]["leo-fail"], "fail")

    def test_script_record_fields(self):
        record = rpe.build_script_record(
            name="tests/debian_package_lifecycle.sh",
            result="pass",
            commit="b" * 40,
            os_pretty="Void Linux",
            os_id="void",
            log="all good\n",
            recorded_at="2026-10-12T00:00:00Z",
        )
        self.assertEqual(record["kind"], "script")
        self.assertEqual(record["debian_version"], None)
        self.assertEqual(record["result"], "pass")


class ImportMainTest(unittest.TestCase):
    def test_import_mode_writes_evidence(self):
        with tempfile.TemporaryDirectory() as root:
            junit = os.path.join(root, "junit.xml")
            osr = os.path.join(root, "os-release")
            out = os.path.join(root, "evidence.json")
            with open(junit, "w", encoding="utf-8") as handle:
                handle.write(JUNIT)
            with open(osr, "w", encoding="utf-8") as handle:
                handle.write(DEBIAN_OS)
            code = rpe.main(
                [
                    "--output", out,
                    "--junit", junit,
                    "--os-release", osr,
                    "--commit", "a" * 40,
                    "--command", "ctest --test-dir obj -j",
                ]
            )
            self.assertEqual(code, 0)
            with open(out, encoding="utf-8") as handle:
                record = json.load(handle)
            self.assertEqual(record["kind"], "ctest-run")
            self.assertEqual(record["debian_version"], "Debian GNU/Linux 13 (trixie)")
            self.assertEqual(record["tests"]["leo-pass"], "pass")
            self.assertEqual(record["commit"], "a" * 40)

    def test_script_mode_writes_evidence(self):
        with tempfile.TemporaryDirectory() as root:
            log = os.path.join(root, "drill.log")
            osr = os.path.join(root, "os-release")
            out = os.path.join(root, "evidence.json")
            with open(log, "w", encoding="utf-8") as handle:
                handle.write("sentinels preserved\n")
            with open(osr, "w", encoding="utf-8") as handle:
                handle.write(DEBIAN_OS)
            code = rpe.main(
                [
                    "--output", out,
                    "--kind", "script",
                    "--name", "tests/debian_package_lifecycle.sh",
                    "--result", "pass",
                    "--log", log,
                    "--os-release", osr,
                    "--commit", "a" * 40,
                ]
            )
            self.assertEqual(code, 0)
            with open(out, encoding="utf-8") as handle:
                record = json.load(handle)
            self.assertEqual(record["name"], "tests/debian_package_lifecycle.sh")
            self.assertIn("sentinels preserved", record["log_excerpt"])

    def test_bad_junit_fails(self):
        with tempfile.NamedTemporaryFile("w", suffix=".xml", delete=False) as handle:
            handle.write("<not-xml")
            path = handle.name
        self.addCleanup(os.unlink, path)
        with tempfile.TemporaryDirectory() as root:
            code = rpe.main(["--output", os.path.join(root, "x.json"), "--junit", path])
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
