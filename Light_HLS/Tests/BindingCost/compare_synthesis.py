#!/usr/bin/env python3
"""Measure binding changes with identical U250 OOC synthesis and RTL tests."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "FairComparison"))
from run_u250_fair_comparison import (
    affinity_prefix, parse_metrics, run_command, write_common_vivado_tcl,
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True, type=Path)
    parser.add_argument("--after", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("Output must be absent or empty")
    vivado = shutil.which("vivado")
    if not vivado:
        parser.error("Vivado is required")
    report = {"valid": False, "part": "xcu250-figd2104-2L-e", "frequency_mhz": 250,
              "source_sha256": digest(root / "affinity.cpp"),
              "testbench_sha256": digest(root / "affinity_tb.v"), "results": {},
              "notes": ["Same native interface, testbench, and OOC synthesis flow.",
                        "Selector bit cost is a proxy, not a LUT estimate.",
                        "Post-synthesis timing is not post-route timing."]}
    for label in ("before", "after"):
        executable = getattr(args, label).resolve(strict=True)
        directory = output / label
        directory.mkdir()
        run_command([str(executable), str(root / "affinity.cpp"), "binding_affinity",
                     "--board", "u250", "--freq", "250MHz", "--output", str(directory)],
                    directory, directory / "light_hls.log", 600)
        rtl = directory / "reports/rtl/top.v"
        run_command(["iverilog", "-g2012", "-Wall", "-s", "affinity_tb", "-o",
                     str(directory / "sim"), str(rtl), str(root / "affinity_tb.v")],
                    directory, directory / "iverilog.log", 60)
        run_command(["vvp", str(directory / "sim")], directory, directory / "simulation.log", 60)
        assert "BINDING COST TESTS PASSED: 256 calls" in (directory / "simulation.log").read_text()
        tcl = directory / "synthesize.tcl"
        write_common_vivado_tcl(tcl, [rtl], "binding_affinity", 4, directory)
        elapsed = run_command(affinity_prefix() + [vivado, "-mode", "batch", "-nolog",
                              "-nojournal", "-source", str(tcl)], directory,
                              directory / "vivado.log", 1800)
        report["results"][label] = {
            "executable_sha256": digest(executable), "rtl_sha256": digest(rtl),
            "functional_calls": 256, "qor": parse_metrics(directory / "metrics.txt", 4, elapsed),
        }
        (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"PASS {label}", flush=True)
    assert report["results"]["before"]["qor"]["interface_signature"] == report["results"]["after"]["qor"]["interface_signature"]
    report["valid"] = True
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
