#!/usr/bin/env python3

"""Run existing self-checking App testbenches independently of QoR assertions."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

TESTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "FairComparison"))
from run_pipeline_u250_comparisons import CASES as PIPELINE_CASES
from run_array_partition_u250_comparisons import CASES as PARTITION_CASES
from run_dataflow_u250_comparisons import CASES as DATAFLOW_CASES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--light-hls-top", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    executable = args.light_hls_top.resolve(strict=True)
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("Output directory must be absent or empty")
    output.mkdir(parents=True, exist_ok=True)
    results = []
    for folder, cases in (("backend_pipeline", PIPELINE_CASES),
                          ("backend_array_partition", PARTITION_CASES),
                          ("light_hls_dataflow", DATAFLOW_CASES)):
        app = TESTS.parent / "App" / folder
        for case in cases:
            directory = output / folder / case.name
            directory.mkdir(parents=True)
            result = {"case": f"{folder}/{case.name}", "valid": False}
            try:
                with (directory / "light_hls.log").open("w") as log:
                    subprocess.run([str(executable), str(app / case.source), case.top,
                                    "--board", "u250", "--freq", "100MHz",
                                    "--output", str(directory)], cwd=directory,
                                   stdout=log, stderr=subprocess.STDOUT,
                                   check=True, timeout=120)
                command = ["iverilog", "-g2012", "-Wall", "-s", case.testbench_top,
                           "-o", str(directory / "sim")]
                command += [f"-D{define}" for define in getattr(case, "common_defines", ())]
                command += [str(directory / "reports/rtl/top.v"), str(app / case.testbench)]
                with (directory / "iverilog.log").open("w") as log:
                    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                   check=True, timeout=60)
                simulation = subprocess.run(["vvp", str(directory / "sim")],
                                            capture_output=True, text=True,
                                            check=True, timeout=60)
                (directory / "simulation.log").write_text(simulation.stdout + simulation.stderr)
                if "TESTS PASSED" not in simulation.stdout:
                    raise ValueError("Testbench did not report success")
                result["cycles"] = [int(value) for value in re.findall(r"cycles=(\d+)", simulation.stdout)]
                result["valid"] = True
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                result["error"] = str(error)
                if isinstance(error, subprocess.CalledProcessError):
                    (directory / "failure.log").write_text((error.stdout or "") + (error.stderr or ""))
            results.append(result)
            print(result, flush=True)
    valid = all(result["valid"] for result in results)
    summary = {"valid": valid, "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
               "results": results}
    (output / "app_smoke.json").write_text(json.dumps(summary, indent=2) + "\n")
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
