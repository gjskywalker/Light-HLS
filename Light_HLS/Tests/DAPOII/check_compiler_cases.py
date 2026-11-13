#!/usr/bin/env python3
"""Bounded compiler/RTL regression without PPO or timeout-policy changes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, default=ROOT / "Dataset/Testbenches")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--designs", nargs="+")
    parser.add_argument("--verify", nargs="*", default=[])
    parser.add_argument("--estimate-only", action="store_true")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    fingerprint = hashlib.sha256(binary.read_bytes()).hexdigest()
    rows = []
    for source in sorted(args.dataset.resolve().glob("*.cc")):
        if args.designs and source.stem not in args.designs: continue
        directory = output / source.stem
        directory.mkdir()
        row = dict(name=source.stem, source=str(source), source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(), commands=[])
        rows.append(row)
        def run(label, command):
            command = [str(x) for x in command]
            with (directory / (label + ".log")).open("x") as log:
                try:
                    completed = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=300)
                    code = completed.returncode
                except subprocess.TimeoutExpired: code = "timeout"
            row["commands"].append(dict(label=label, command=command, returncode=code))
            (directory / "commands.json").write_text(json.dumps(row["commands"], indent=2)+"\n")
            return code == 0
        common = ["--board", "u250", "--freq", "100MHz"]
        row["prepared"] = run("prepare", [binary, "--prepare-source", source, "dut", *common, "--output", directory / "prepared"])
        if row["prepared"]:
            manifest = directory / "prepared/reports/dapo_manifest.json"
            ir = json.loads(manifest.read_text())["ir"]
            command = [binary, ir, "dut", "--manifest", manifest, *common, "--latency-model", "scheduled", "--output", directory / "generated"]
            if args.estimate_only: command += ["--estimate-only"]
            row["compiled"] = run("generate", command)
            failure = directory / "generated/reports/rtl/failure.json"
            if failure.exists(): row["errors"] = json.loads(failure.read_text()).get("errors", [])
            if row["compiled"] and source.stem in args.verify and not args.estimate_only:
                row["verified"] = run("verify", [sys.executable,
                    ROOT / "experiments/dapo_ii_cross_validation/verify_design.py", "--source", source,
                    "--reports", directory / "generated/reports", "--ir", ir,
                    "--output", directory / "verification", "--cases", "4"])
                verification = directory / "verification/verification.json"
                if verification.exists(): row["verification"] = json.loads(verification.read_text())
                if row["verified"]:
                    lowered = directory / "generated/reports/.lighthls/backend_lowered.bc"
                    row["lowered_ir_verified"] = run("verify_lowered", [sys.executable,
                        ROOT / "experiments/dapo_ii_cross_validation/verify_design.py", "--source", source,
                        "--reports", directory / "generated/reports", "--ir", lowered,
                        "--output", directory / "verification_lowered", "--cases", "4"])
                    row["verified"] &= row["lowered_ir_verified"]
                    estimate = json.loads((directory / "generated/reports/estimate/dapo_estimate.json").read_text())
                    envelope = estimate.get("scheduled_latency", {}).get("cycles", {})
                    row["cycle_envelope"] = envelope
                    if envelope:
                        row["cycles_in_envelope"] = all(envelope["minimum"] <= case["cycles"] <= envelope["maximum"]
                            for case in row["verification"]["cases"])
                        row["verified"] &= row["cycles_in_envelope"]
        (output / "results.json").write_text(json.dumps(dict(binary=str(binary), binary_sha256=fingerprint,
            binary_unchanged=hashlib.sha256(binary.read_bytes()).hexdigest()==fingerprint, results=rows), indent=2)+"\n")
        print(row["name"],row.get("compiled"),row.get("verified"),[(e["code"],e["location"]) for e in row.get("errors",[])],flush=True)
    return 0 if all(r.get("compiled") and r.get("verified", True) for r in rows) else 1


if __name__ == "__main__": raise SystemExit(main())
