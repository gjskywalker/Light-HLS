#!/usr/bin/env python3
"""Archive paired measurements and prove their candidate RTL matches a release.

The manifest contains cases with case/before/after fields. Report paths remain
original measurements: compiler fingerprints and reward eligibility are never
rewritten. An older measurement may be reused only after an RTL identity check.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from verify_archived_rtl import canonicalize_zero_assignments


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--light-hls-top", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--baseline-archive", type=Path,
                        help="Use a preserved compact baseline instead of per-case before reports")
    parser.add_argument("--identity-only", action="store_true",
                        help="Prove release RTL coverage without producing a before/after archive")
    parser.add_argument("--include-timing-failures", action="store_true",
                        help="Identity-only: retain functionally valid measurements even when timing fails")
    args = parser.parse_args()
    if args.include_timing_failures and not args.identity_only:
        parser.error("--include-timing-failures requires --identity-only; it never grants reward eligibility")
    executable = args.light_hls_top.resolve(strict=True)
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        raise ValueError(f"Refusing to overwrite {output}")
    output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(args.manifest.read_text())
    cases = manifest["cases"]
    names = [case["case"] for case in cases]
    if not cases or len(names) != len(set(names)):
        raise ValueError("Case names must be nonempty and unique")
    if any(not name or Path(name).is_absolute() or ".." in Path(name).parts for name in names):
        raise ValueError("Case names must be relative paths without parent traversal")
    root = Path(__file__).resolve().parent
    environment = dict(os.environ)
    environment["HI_HLS_LIB_PATH"] = str(root.parents[1] / "HLS_Data_Lib")
    identities = []
    for item in cases:
        path = Path(item["after"]).resolve(strict=True)
        report = json.loads(path.read_text())
        if report.get("valid") is not True or (report.get("reward_eligible") is not True and
                not (args.include_timing_failures and report.get("functional_equivalence", {}).get("valid") is True)):
            raise ValueError(f"Ineligible measurement: {path}")
        if report.get("config_sha256") is not None:
            raise ValueError("Explicit configuration reconstruction is not supported")
        source = Path(report["source"]).resolve(strict=True)
        if digest(source) != report["source_sha256"]:
            raise ValueError(f"Changed source: {source}")
        directory = output / "regenerated" / item["case"]
        directory.mkdir(parents=True)
        frequency = 1000.0 / report["clock_period_ns"]
        with (directory / "light_hls.log").open("w") as log:
            subprocess.run([str(executable), str(source), report["top_function"], "--board", "u250",
                            "--freq", f"{frequency:.17g}MHz", "--output", str(directory)],
                           cwd=directory, env=environment, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=180)
        measured = path.parent / "light_hls/reports/rtl/top.v"
        regenerated = directory / "reports/rtl/top.v"
        identical = digest(measured) == digest(regenerated)
        canonical = canonicalize_zero_assignments(measured.read_text()) == canonicalize_zero_assignments(regenerated.read_text())
        identities.append({"case": item["case"], "comparison_id": report["comparison_id"],
                           "measurement_reward_eligible": report["reward_eligible"],
                           "measured_executable_sha256": report["light_hls_executable_sha256"],
                           "rtl_identical": identical, "rtl_matches_modulo_zero_assignment_order": canonical,
                           "measured_rtl_sha256": digest(measured), "regenerated_rtl_sha256": digest(regenerated)})
        print(f"{item['case']}: {'IDENTICAL' if identical else 'ZERO ORDER ONLY' if canonical else 'MISMATCH'}", flush=True)
    valid = all(item["rtl_matches_modulo_zero_assignment_order"] for item in identities)
    coverage = {"schema_version": "1.0", "stage": "release_measured_rtl_coverage", "valid": valid,
                "case_count": len(identities), "light_hls_executable_sha256": digest(executable),
                "measurements_rewritten": False, "manifest_sha256": digest(args.manifest), "cases": identities}
    (output / "release_rtl_coverage.json").write_text(json.dumps(coverage, indent=2) + "\n")
    if not valid:
        return 1
    if args.identity_only:
        return 0
    for side in ("before", "after"):
        if side == "before" and args.baseline_archive:
            (output / side).mkdir()
            shutil.copyfile(args.baseline_archive.resolve(strict=True), output / side / "u250_baselines.json")
            continue
        command = [sys.executable, str(root / "archive_u250_baselines.py"), "--output", str(output / side)]
        for item in cases:
            command += ["--input", f"{item['case']}={item[side]}"]
        subprocess.run(command, check=True)
    subprocess.run([sys.executable, str(root / "compare_u250_baselines.py"),
                    "--baseline", str(output / "before/u250_baselines.json"),
                    "--candidate", str(output / "after/u250_baselines.json"),
                    "--output", str(output / "qor_delta.json")], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
