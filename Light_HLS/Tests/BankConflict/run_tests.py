#!/usr/bin/env python3
"""Check request-edge proof against exhaustive timelines and latency-two RTL."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "QoRPressure"))
from run_tests import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("Refusing to overwrite bank conflict results")
    common = root.parents[1] / "Implementations/HI_BackendCommon"
    flags = shlex.split(subprocess.check_output(["llvm-config", "--cxxflags"], text=True))
    libraries = shlex.split(subprocess.check_output(
        ["llvm-config", "--ldflags", "--libs", "support", "--system-libs"], text=True))
    # Override release NDEBUG: these assertions are the independent oracle.
    run(["clang++", *flags, "-std=c++17", "-UNDEBUG", "-I", common,
         common / "HI_BankConflict.cc", root / "conflicts.cpp", *libraries, "-o", output / "proof"], output, "build.log")
    run([output / "proof"], output, "proof.log")
    run([args.executable.resolve(), root / "latency2.cpp", "qor_pressure", "--board", "u250",
         "--freq", "250MHz", "--output", output], output, "compile.log")
    run(["iverilog", "-g2012", "-s", "pressure_tb", "-DCASE_KIND=2", "-o", output / "sim",
         output / "reports/rtl/top.v", root.parent / "QoRPressure/pressure_tb.v"], output, "iverilog.log")
    run(["vvp", output / "sim"], output, "simulation.log")
    schedule = json.loads((output / "reports/scheduling/pipeline_schedule.json").read_text())
    assert all(loop["achieved_ii"] == 1 for loop in schedule["pipelines"])
    assert (output / "simulation.log").read_text().count("TESTS PASSED") == 8
    (output / "summary.json").write_text(json.dumps({"valid": True, "random_proofs": 20000,
        "latency_two_calls": 8, "frequency_mhz": 250, "intervals": [1, 1]}, indent=2) + "\n")


if __name__ == "__main__":
    main()
