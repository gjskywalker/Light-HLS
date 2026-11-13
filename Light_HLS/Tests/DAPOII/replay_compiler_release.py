#!/usr/bin/env python3
"""Replay accepted baseline/pragma IR with a fingerprinted release binary."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary, work, output = args.binary.resolve(), args.work.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    initial_hash = digest(binary)
    groups = ["baseline80_release", "pipeline_sweep", "unroll_sweep", "cyclic_sweep", "block_sweep"]
    jobs = [(group, row["name"]) for group in groups
            for row in json.loads((work / group / "results.json").read_text())["results"]]

    def run(job):
        group, name = job
        manifest = work / group / name / "prepared/reports/dapo_manifest.json"
        data = json.loads(manifest.read_text())
        target = output / group / name
        target.mkdir(parents=True)
        command = [str(binary), data["ir"], "dut", "--manifest", str(manifest), "--board", "u250",
                   "--freq", "100MHz", "--latency-model", "scheduled", "--estimate-only", "--output", str(target)]
        with (target / "driver.log").open("x") as stream:
            try:
                code = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, timeout=300).returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
        estimate = target / "reports/estimate/dapo_estimate.json"
        report = json.loads(estimate.read_text()) if estimate.exists() else {}
        return dict(group=group, name=name, command=command, returncode=code,
                    manifest_sha256=digest(manifest), ir_sha256=digest(Path(data["ir"])),
                    passed=code == 0 and report.get("valid") is True and report.get("scheduled_latency", {}).get("valid") is True)

    with ThreadPoolExecutor(max_workers=4) as executor:
        rows = list(executor.map(run, jobs))
    result = dict(binary=str(binary), binary_sha256=initial_hash, binary_unchanged=digest(binary) == initial_hash,
                  total=len(rows), passed=sum(row["passed"] for row in rows), results=rows,
                  scope="Scheduling, binding and estimator acceptance only; RTL simulation is recorded separately.")
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items() if key != "results"}), flush=True)
    return 0 if result["binary_unchanged"] and result["passed"] == result["total"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
