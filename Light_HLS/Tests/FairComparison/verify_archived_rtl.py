#!/usr/bin/env python3

"""Regenerate RTL and compare it with measured comparison inputs.

This audit does not rewrite measurement fingerprints or grant reward eligibility
to a different compiler executable. Reports with an explicit configuration are
rejected because older comparison reports do not retain its source path.
The only permitted textual difference is the order of adjacent independent
nonblocking assignments of literal zero; raw byte hashes are retained as well.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import subprocess
from pathlib import Path


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def canonicalize_zero_assignments(text: str) -> str:
    """Sort only adjacent literal-zero NBAs with unique scalar destinations."""
    lines = text.splitlines(keepends=True)
    pattern = re.compile(r"^\s+([A-Za-z_][A-Za-z0-9_$]*) <= [1-9][0-9]*'d0;\s*$")
    result = []
    index = 0
    while index < len(lines):
        end = index
        destinations = []
        while end < len(lines):
            match = pattern.fullmatch(lines[end])
            if not match:
                break
            destinations.append(match.group(1))
            end += 1
        if end == index:
            result.append(lines[index])
            index += 1
            continue
        group = lines[index:end]
        if len(set(destinations)) == len(destinations):
            group = [line for _, line in sorted(zip(destinations, group))]
        result.extend(group)
        index = end
    return "".join(result)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--light-hls-top", required=True, type=Path)
    parser.add_argument("--comparison-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--expected-cases", required=True, type=int)
    args = parser.parse_args()
    if args.expected_cases <= 0:
        parser.error("Expected case count must be positive")
    executable = args.light_hls_top.resolve(strict=True)
    root = args.comparison_root.resolve(strict=True)
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("Output directory must be absent or empty")
    reports = sorted(path for path in root.rglob("u250_fair_comparison.json")
                     if not any(".failed." in part for part in path.relative_to(root).parts))
    if len(reports) != args.expected_cases:
        parser.error(f"Expected {args.expected_cases} comparisons, found {len(reports)}")
    output.mkdir(parents=True, exist_ok=True)
    results = []
    compiler_hash = digest(executable)
    for report_path in reports:
        report = json.loads(report_path.read_text(encoding="utf-8"))
        relative = report_path.parent.relative_to(root)
        case = {"case": str(relative), "comparison_id": report["comparison_id"],
                "measured_executable_sha256": report["light_hls_executable_sha256"],
                "rtl_identical": False, "rtl_matches_modulo_zero_assignment_order": False}
        try:
            if report.get("valid") is not True or report.get("reward_eligible") is not True:
                raise ValueError("Comparison is not a completed, eligible measurement")
            if report.get("config_sha256") is not None:
                raise ValueError("Cannot reconstruct an explicit configuration from this report")
            source = Path(report["source"]).resolve(strict=True)
            if digest(source) != report["source_sha256"]:
                raise ValueError("Source fingerprint has changed")
            case_directory = output / relative
            case_directory.mkdir(parents=True, exist_ok=True)
            period = float(report["clock_period_ns"])
            if not math.isfinite(period) or period <= 0:
                raise ValueError("Invalid clock period in comparison report")
            frequency = 1000.0 / period
            with (case_directory / "light_hls.log").open("w", encoding="utf-8") as log:
                subprocess.run(
                    [str(executable), str(source), report["top_function"],
                     "--board", report["board"], "--freq", f"{frequency:.12g}MHz",
                     "--output", str(case_directory)],
                    cwd=case_directory, stdout=log, stderr=subprocess.STDOUT,
                    check=True, timeout=600)
            measured = report_path.parent / "light_hls/reports/rtl/top.v"
            regenerated = case_directory / "reports/rtl/top.v"
            case["measured_rtl_sha256"] = digest(measured)
            case["regenerated_rtl_sha256"] = digest(regenerated)
            case["rtl_identical"] = case["measured_rtl_sha256"] == case["regenerated_rtl_sha256"]
            case["rtl_matches_modulo_zero_assignment_order"] = (
                canonicalize_zero_assignments(measured.read_text(encoding="utf-8")) ==
                canonicalize_zero_assignments(regenerated.read_text(encoding="utf-8")))
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            case["error"] = str(error)
        results.append(case)
        status = ("IDENTICAL" if case["rtl_identical"] else
                  "ZERO-ASSIGNMENT ORDER ONLY" if case["rtl_matches_modulo_zero_assignment_order"] else "FAIL")
        print(f"[{relative}] {status}", flush=True)
    valid = all(case["rtl_matches_modulo_zero_assignment_order"] for case in results)
    summary = {"schema_version": "1.0", "stage": "measured_rtl_identity_audit",
               "valid": valid, "light_hls_executable_sha256": compiler_hash,
               "measurements_rewritten": False, "cases": results}
    (output / "rtl_identity_audit.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
