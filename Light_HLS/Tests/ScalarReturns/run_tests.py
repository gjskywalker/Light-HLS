#!/usr/bin/env python3
"""Stress old-PHI live-outs, timed entry/return fusion, and argument acceptance."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    root = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError(f"Refusing to overwrite: {output}")
    environment = dict(os.environ)
    environment["HI_HLS_LIB_PATH"] = str(root.parents[1] / "HLS_Data_Lib")
    results = []
    for kind, name in enumerate(("exit_expression", "dynamic_zero_trip", "old_and_new_liveouts")):
        for frequency in ("100MHz", "333.3333333333333MHz"):
            directory = output / f"{name}_{frequency}"
            directory.mkdir()
            source = directory / "returns.cpp"
            source.write_text((root / "returns.cpp").read_text().replace("#define CASE_KIND 0", f"#define CASE_KIND {kind}"))
            commands = [
                ("light_hls.log", [str(executable), str(source), "scalar_returns", "--board", "u250",
                                   "--freq", frequency, "--output", str(directory)]),
                ("verify.log", ["opt", "-passes=verify", str(directory / "reports/.lighthls/top_output.bc"), "-disable-output"]),
                ("iverilog.log", ["iverilog", "-g2012", "-Wall", "-s", "scalar_returns_tb", f"-DCASE_KIND={kind}",
                                  "-o", str(directory / "sim"), str(directory / "reports/rtl/top.v"), str(root / "returns_tb.v")]),
                ("simulation.log", ["vvp", str(directory / "sim")]),
            ]
            for log, command in commands:
                with (directory / log).open("w") as stream:
                    subprocess.run(command, cwd=directory, stdout=stream, stderr=subprocess.STDOUT,
                                   env=environment, check=True, timeout=180)
            match = re.search(r"SCALAR RETURN TESTS PASSED: 64 calls, total_cycles=(\d+)",
                              (directory / "simulation.log").read_text())
            assert match, directory
            results.append({"case": name, "frequency": frequency, "calls": 64,
                            "total_cycles": int(match[1]), "valid": True})
            print(f"PASS {name} {frequency}: 64 calls, {match[1]} cycles", flush=True)
    summary = {"valid": True, "results": results, "calls": 64 * len(results),
               "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
               "test_sources_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                        for p in sorted(root.iterdir()) if p.is_file()}}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
