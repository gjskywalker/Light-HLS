#!/usr/bin/env python3
"""Regenerate and verify affine pipeline expression grouping from source and IR."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]


def run(directory, label, command):
    command = [str(item) for item in command]
    with (directory / (label + ".log")).open("x") as log:
        result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=300)
    (directory / (label + ".command.json")).write_text(json.dumps(
        dict(command=command, returncode=result.returncode), indent=2) + "\n")
    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--frozen-manifest", type=Path)
    parser.add_argument("--frozen-source", type=Path)
    args = parser.parse_args()
    binary, output = args.binary.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    fingerprint = hashlib.sha256(binary.read_bytes()).hexdigest()
    sources = sorted((ROOT / "Light_HLS/App/backend_expression").glob("*.cpp"))
    sources += [ROOT / "Light_HLS/App/backend_pipeline/backend_pipeline.cpp",
                ROOT / "Light_HLS/App/backend_pipeline/backend_pipeline_recurrence.cpp",
                ROOT / "Light_HLS/App/backend_pipeline/backend_pipeline_mixed_memory.cpp"]
    # Legacy smoke kernels use their filename as top; normalize only the top name
    # in separate generated copies so the shared typed verifier can call dut.
    records = []
    if bool(args.frozen_manifest) != bool(args.frozen_source):
        parser.error("Provide both frozen arguments or neither")
    jobs = [(source.stem, source, None) for source in sources]
    if args.frozen_manifest:
        jobs.insert(0, ("random67_frozen", args.frozen_source.resolve(), args.frozen_manifest.resolve()))
    for name, source, frozen in jobs:
        directory = output / name
        directory.mkdir()
        row = dict(name=name, source=str(source), status="failed")
        records.append(row)
        if not frozen and source.stem.startswith("backend_pipeline"):
            target = directory / "source.cpp"
            target.write_text(source.read_text().replace(source.stem + "(", "dut("))
            source = target
        common = ["--board", "u250", "--freq", "100MHz"]
        manifest = frozen or directory / "prepared/reports/dapo_manifest.json"
        prepared = frozen is not None or run(directory, "prepare", [binary, "--prepare-source", source,
            "dut", *common, "--output", directory / "prepared"])
        if prepared:
            ir = json.loads(manifest.read_text())["ir"]
            generated = directory / "generated"
            row["generated"] = run(directory, "generate", [binary, ir, "dut", "--manifest", manifest,
                *common, "--latency-model", "scheduled", "--output", generated])
            if row["generated"]:
                verified = run(directory, "verify", [sys.executable,
                    ROOT / "experiments/dapo_ii_cross_validation/verify_design.py", "--source", source,
                    "--ir", ir, "--reports", generated / "reports", "--output", directory / "verification",
                    "--cases", "4"])
                evidence = json.loads((directory / "verification/verification.json").read_text())
                estimate = json.loads((generated / "reports/estimate/dapo_estimate.json").read_text())
                row.update(verification_status=evidence["status"],
                    native_ir_equivalence=evidence["native_ir_equivalence"],
                    measured_cycles=[case.get("cycles") for case in evidence["cases"]],
                    estimated_cycles=estimate["scheduled_latency"]["cycles"])
                bounds = row["estimated_cycles"]
                if verified and bounds["constant"] and all(c == bounds["maximum"] for c in row["measured_cycles"]):
                    row["status"] = "passed"
        report = dict(binary=str(binary), binary_sha256=fingerprint, results=records,
            binary_unchanged=hashlib.sha256(binary.read_bytes()).hexdigest() == fingerprint)
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print(name, row["status"], row.get("measured_cycles"), flush=True)
    return 0 if all(row["status"] == "passed" for row in records) and report["binary_unchanged"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
