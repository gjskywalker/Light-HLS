#!/usr/bin/env python3
"""Exercise the generated PIPO bank primitive with concurrent/paused requests."""

import argparse
import hashlib
import json
import os
from pathlib import Path
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
    source = root.parents[1] / "App/light_hls_dataflow/vector_array_dataflow.cc"
    commands = [
        ("light_hls.log", [str(executable), str(source), "vector_array_dataflow", "--board", "u250",
                           "--freq", "100MHz", "--output", str(output)]),
        ("iverilog.log", ["iverilog", "-g2012", "-Wall", "-s", "pipo_banks_tb", "-o", str(output / "sim"),
                          str(output / "reports/rtl/top.v"), str(root / "banks_tb.v")]),
        ("simulation.log", ["vvp", str(output / "sim")]),
    ]
    for log, command in commands:
        with (output / log).open("w") as stream:
            subprocess.run(command, cwd=output, stdout=stream, stderr=subprocess.STDOUT,
                           env=environment, check=True, timeout=180)
    assert "PIPO BANK TESTS PASSED" in (output / "simulation.log").read_text()
    summary = {"valid": True, "request_hold_cycles": 8192, "depths": [1, 3, 16, 64],
               "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
               "test_sources_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                        for p in sorted(root.iterdir()) if p.is_file()}}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("PIPO BANK TESTS PASSED: 8192 request/hold cycles", flush=True)


if __name__ == "__main__":
    main()
