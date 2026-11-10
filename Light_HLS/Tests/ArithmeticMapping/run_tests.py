#!/usr/bin/env python3
"""Check overflow, high-bit operands and shared narrow/wide multiplication."""
import argparse
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "QoRPressure"))
from run_tests import run

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("Refusing to overwrite arithmetic results")
    for frequency in (100, 250):
        directory = output / str(frequency)
        directory.mkdir()
        run([args.executable.resolve(), root / "mixed_width.cpp", "arithmetic_mapping", "--board", "u250",
             "--freq", f"{frequency}MHz", "--output", directory], directory, "compile.log")
        run(["iverilog", "-g2012", "-s", "testbench", "-o", directory / "sim",
             directory / "reports/rtl/top.v", root / "testbench.v"], directory, "iverilog.log")
        run(["vvp", directory / "sim"], directory, "simulation.log")
        assert "ARITHMETIC TESTS PASSED" in (directory / "simulation.log").read_text()
    (output / "summary.json").write_text(json.dumps({"valid": True, "calls": 2048,
        "frequencies_mhz": [100, 250]}, indent=2) + "\n")

if __name__ == "__main__":
    main()
