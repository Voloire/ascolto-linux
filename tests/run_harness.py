#!/usr/bin/env python3
"""Run isolated scenarios and retain machine-readable evidence, including failures."""
import json
import os
import sys
import time
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

root = Path(__file__).resolve().parents[1]
run = root / "build/test-runs" / time.strftime("%Y%m%d-%H%M%S")
run.mkdir(parents=True, exist_ok=False)
os.environ["ASCOLTO_TEST_RUN"] = str(run)


class Result(unittest.TextTestResult):
    def startTest(self, test):
        self.started = time.monotonic()
        super().startTest(test)

    def stopTest(self, test):
        durations[test.id()] = time.monotonic() - self.started
        super().stopTest(test)


durations = {}
suite = unittest.TestSuite()
for name in ("test_providers", "test_e2e"):
    suite.addTests(unittest.defaultTestLoader.loadTestsFromName(name))
result = unittest.TextTestRunner(verbosity=2, resultclass=Result).run(suite)
failures = {test.id(): detail for test, detail in result.failures + result.errors}
summary = {"tests": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
           "skipped": len(result.skipped), "durations": durations, "failure_details": failures,
           "isolation": "mock audio, fixture transcription, mock provider HTTP and local fixture CLI; no credentials"}
(run / "results.json").write_text(json.dumps(summary, indent=2))
xml = ET.Element("testsuite", name="Ascolto autonomous harness", tests=str(result.testsRun),
                 failures=str(len(result.failures)), errors=str(len(result.errors)))
for name, seconds in durations.items():
    case = ET.SubElement(xml, "testcase", name=name, time=f"{seconds:.3f}")
    if name in failures:
        ET.SubElement(case, "failure").text = failures[name]
ET.ElementTree(xml).write(run / "junit.xml", encoding="utf-8", xml_declaration=True)
(run / "README.md").write_text("# Autonomous test run\n\n" + f"{result.testsRun} tests; {len(failures)} failures/errors.\n\n"
                              "Each end-to-end directory retains the daemon log, command/response trace, event stream, SQLite database and session artifacts.\n")
print(f"\nEvidence: {run}")
sys.exit(0 if result.wasSuccessful() else 1)
