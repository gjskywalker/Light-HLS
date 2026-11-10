#!/usr/bin/env python3
"""Verify start-edge constant initialization and fused void completion."""

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
    environment = dict(os.environ)
    environment["HI_HLS_LIB_PATH"] = str(root.parents[1] / "HLS_Data_Lib")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError(f"Refusing to overwrite: {output}")
    results = []
    for kind, name in enumerate(("constant_phi", "argument_phi", "zero_trip", "early_exit",
                                 "signed_constant_shift", "dynamic_shift")):
        directory = output / name
        directory.mkdir()
        source = directory / "edges.cpp"
        source.write_text((root / "edges.cpp").read_text().replace("#define CASE_KIND 0", f"#define CASE_KIND {kind}"))
        commands = [
            ("light_hls.log", [str(executable), str(source), "transaction_edges", "--board", "u250",
                               "--freq", "100MHz", "--output", str(directory)]),
            ("verify.log", ["opt", "-passes=verify", str(directory / "reports/.lighthls/top_output.bc"), "-disable-output"]),
            ("iverilog.log", ["iverilog", "-g2012", "-Wall", "-s", "transaction_edges_tb", f"-DCASE_KIND={kind}",
                              "-o", str(directory / "sim"), str(directory / "reports/rtl/top.v"), str(root / "edges_tb.v")]),
            ("simulation.log", ["vvp", str(directory / "sim")]),
            ("held_build.log", ["iverilog", "-g2012", "-Wall", "-s", "held_tb", f"-DCASE_KIND={kind}",
                                "-o", str(directory / "held_sim"), str(directory / "reports/rtl/top.v"), str(root / "held_tb.v")]),
            ("held_simulation.log", ["vvp", str(directory / "held_sim")]),
        ]
        for log, command in commands:
            with (directory / log).open("w") as stream:
                subprocess.run(command, cwd=directory, stdout=stream, stderr=subprocess.STDOUT,
                               env=environment, check=True, timeout=180)
        match = re.search(r"TRANSACTION EDGE TESTS PASSED: 64 calls, total_cycles=(\d+)",
                          (directory / "simulation.log").read_text())
        assert match, directory
        assert "HELD TRANSACTION TESTS PASSED" in (directory / "held_simulation.log").read_text(), directory
        schedule = json.loads((directory / "reports/scheduling/schedule.json").read_text())
        assert schedule["ordinary_entry_fused"] == (kind in (0, 1, 4, 5)), (name, schedule)
        assert schedule["ordinary_fused_void_return_count"] == 1, name
        results.append({"case": name, "valid": True, "calls": 192, "held_and_gapped_calls": 128,
                        "reset_recovery": True, "pulse_total_cycles": int(match[1])})
        print(f"PASS {name}: {match[1]} cycles", flush=True)
    summary = {"valid": True, "results": results, "calls": 192 * len(results),
               "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
               "test_sources_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                        for p in sorted(root.iterdir()) if p.is_file()}}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
