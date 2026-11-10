#!/usr/bin/env python3
"""Check cost-monotone FU affinity, invariant inputs, and captured arguments."""
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
    root = Path(__file__).resolve().parent
    executable = args.executable.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error(f"Refusing to overwrite {output}")
    environment = dict(os.environ)
    environment["HI_HLS_LIB_PATH"] = str(root.parents[1] / "HLS_Data_Lib")
    results = []
    for frequency in (100, 150, 200, 250, 333):
        directory = output / str(frequency)
        directory.mkdir()
        commands = [
            ("light_hls.log", [executable, root / "affinity.cpp", "binding_affinity", "--board", "u250",
                               "--freq", f"{frequency}MHz", "--output", directory]),
            ("iverilog.log", ["iverilog", "-g2012", "-Wall", "-s", "affinity_tb", "-o", directory / "sim",
                              directory / "reports/rtl/top.v", root / "affinity_tb.v"]),
            ("simulation.log", ["vvp", directory / "sim"]),
        ]
        for log, command in commands:
            with (directory / log).open("w") as stream:
                subprocess.run([str(item) for item in command], cwd=directory, env=environment,
                               stdout=stream, stderr=subprocess.STDOUT, timeout=180, check=True)
        binding = json.loads((directory / "reports/binding/binding.json").read_text())
        assert binding["selected_mux_bit_cost"] <= binding["ordinal_mux_bit_cost"]
        assert "BINDING COST TESTS PASSED" in (directory / "simulation.log").read_text()
        results.append({"frequency_mhz": frequency, "valid": True, "calls": 256,
                        "ordinal_cost": binding["ordinal_mux_bit_cost"],
                        "selected_cost": binding["selected_mux_bit_cost"],
                        "rebound_groups": binding["affinity_rebound_groups"],
                        "invariant_operands": sum(item["invariant_operand_count"] for item in binding["operator_instances"])})
        print(f"PASS binding at {frequency} MHz: {results[-1]}", flush=True)
    assert any(item["rebound_groups"] for item in results), "Affinity fixture did not exercise rebinding"
    assert any(item["invariant_operands"] for item in results), "Invariant inputs were not exercised"
    unsupported = output / "unsupported_clock"
    unsupported.mkdir()
    with (unsupported / "light_hls.log").open("w") as stream:
        failed = subprocess.run([str(executable), str(root / "affinity.cpp"), "binding_affinity",
            "--board", "u250", "--freq", "500MHz", "--output", str(unsupported)],
            cwd=unsupported, env=environment, stdout=stream, stderr=subprocess.STDOUT, timeout=180)
    assert failed.returncode == 1, failed.returncode
    assert "HLS_LIBRARY_CLOCK_UNSUPPORTED" in (unsupported / "light_hls.log").read_text()
    assert not (unsupported / "reports/rtl/top.v").exists()
    (output / "summary.json").write_text(json.dumps({"valid": True, "results": results,
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest()}, indent=2) + "\n")


if __name__ == "__main__":
    main()
