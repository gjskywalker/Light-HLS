#!/usr/bin/env python3
"""Validate integer DSP/BRAM pressure kernels and optionally compare U250 RTL."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


CASES = {"dsp_map": 0, "bram_permute": 1, "bram_banked": 2, "dsp_dataflow": 3}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, directory, log):
    environment = dict(os.environ)
    environment["HI_HLS_LIB_PATH"] = str(Path(__file__).resolve().parents[2] / "HLS_Data_Lib")
    with (directory / log).open("w") as stream:
        subprocess.run([str(item) for item in command], cwd=directory, env=environment,
                       stdout=stream, stderr=subprocess.STDOUT, check=True, timeout=1200)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--freq-mhz", type=float, action="append")
    parser.add_argument("--compare", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    executable = args.executable.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error(f"Refusing to overwrite {output}")
    results = []
    expected_results = len(args.freq_mhz or [100]) * len(args.case or CASES)
    for frequency in args.freq_mhz or [100]:
        if frequency <= 0:
            parser.error("Frequency must be positive")
        for name in args.case or CASES:
            directory = output / f"{name}_{frequency:g}MHz"
            directory.mkdir()
            source = root / f"{name}.cpp"
            run(["clang++", "-std=c++17", "-Wno-unknown-pragmas", "-I", root / "software",
                 source, root / "reference.cpp", "-o", directory / "reference"], directory, "reference_build.log")
            run([directory / "reference"], directory, "reference.log")
            reference = (directory / "reference.log").read_text().splitlines()
            if args.compare:
                command = [sys.executable, root.parent / "FairComparison/run_u250_fair_comparison.py",
                           "--light-hls-top", executable, "--source", source, "--top", "qor_pressure",
                           "--testbench", root / "pressure_tb.v", "--testbench-top", "pressure_tb",
                           "--testbench-define", f"CASE_KIND={CASES[name]}",
                           "--vitis-testbench-define", "PRESSURE_HOLD_START",
                           "--clock-period-ns", str(1000 / frequency), "--output", directory / "comparison"]
                if name == "dsp_dataflow":
                    command += ["--vitis-testbench-define", "PRESSURE_MEMORY_DIRECTIONS",
                                "--rtl-simulator", "xsim"]
                if name.startswith("bram_"):
                    command += ["--vitis-testbench-define", "PRESSURE_ALLOW_PREFETCH"]
                run(command, directory, "comparison.log")
                report = json.loads((directory / "comparison/u250_fair_comparison.json").read_text())
                assert report["valid"] and report["functional_equivalence"]["valid"], directory
                for generator in ("light_hls", "vitis_hls"):
                    assert report["functional_equivalence"][generator]["trace"] == reference, generator
                    resources = report["results"][generator]["post_rtl_synthesis"]["resources"]
                    if name.startswith("dsp_"):
                        assert resources["dsp"] > 0, (generator, name, resources)
                    else:
                        assert resources["bram_tiles"] > 0, (generator, name, resources)
                results.append({"case": name, "frequency_mhz": frequency, "valid": True,
                                "comparison": str(directory / "comparison/u250_fair_comparison.json"),
                                "reward_eligible": report["reward_eligible"],
                                "cycles": {key: report["functional_equivalence"][key]["mean_transaction_cycles"]
                                           for key in ("light_hls", "vitis_hls")}})
            else:
                run([executable, source, "qor_pressure", "--board", "u250", "--freq", f"{frequency:g}MHz",
                     "--output", directory], directory, "light_hls.log")
                run(["opt", "-passes=verify", directory / "reports/.lighthls/top_output.bc", "-disable-output"],
                    directory, "verify.log")
                run(["iverilog", "-g2012", "-Wall", "-s", "pressure_tb", f"-DCASE_KIND={CASES[name]}",
                     "-o", directory / "sim", directory / "reports/rtl/top.v", root / "pressure_tb.v"],
                    directory, "iverilog.log")
                run(["vvp", directory / "sim"], directory, "simulation.log")
                lines = (directory / "simulation.log").read_text().splitlines()
                assert [line for line in lines if line.startswith("RESULT ")] == reference, directory
                events = [{key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)}
                          for line in lines if line.startswith("EVENT ")]
                assert len(events) == 8 and all(item["done"] >= item["last_commit"] for item in events), events
                binding = json.loads((directory / "reports/binding/binding.json").read_text())
                assert binding["selected_mux_bit_cost"] <= binding["ordinal_mux_bit_cost"], binding
                if name.startswith("bram_"):
                    tile = next(item for item in binding["memory_objects"] if item["name"] == "tile")
                    assert tile["depth"] == 256 and tile["depth_exact"], tile
                    if name == "bram_permute":
                        assert tile["inferred_minimum_depth"] == 256, tile
                    else:
                        assert tile["banks"] == 2 and tile["bank_depth"] == 128, tile
                    assert tile["implementation"] == "bram", tile
                if name == "dsp_dataflow":
                    for stall in (False, True):
                        label = "queued_stalled" if stall else "queued"
                        run(["iverilog", "-g2012", "-Wall", "-s", "queued_tb",
                             *(["-DINJECT_STALLS"] if stall else []), "-o", directory / label,
                             directory / "reports/rtl/top.v", root / "queued_tb.v"],
                            directory, label + "_iverilog.log")
                        run(["vvp", directory / label], directory, label + ".log")
                        assert "QUEUED PRESSURE TESTS PASSED" in (directory / (label + ".log")).read_text()
                if name in ("dsp_map", "dsp_dataflow"):
                    run(["clang++", "-std=c++17", "-Wno-unknown-pragmas", "-I", root / "software",
                         source, root / "sustained_reference.cpp", "-o", directory / "sustained_reference"],
                        directory, "sustained_reference_build.log")
                    run([directory / "sustained_reference"], directory, "sustained_reference.log")
                    run(["iverilog", "-g2012", "-Wall", "-s", "sustained_tb", "-o", directory / "sustained_sim",
                         directory / "reports/rtl/top.v", root / "sustained_tb.v"], directory, "sustained_build.log")
                    run(["vvp", directory / "sustained_sim"], directory, "sustained.log")
                    sustained = (directory / "sustained.log").read_text()
                    assert "SUSTAINED TESTS PASSED" in sustained
                    assert [line for line in sustained.splitlines() if line.startswith("RESULT ")] == (
                        directory / "sustained_reference.log").read_text().splitlines()
                    run(["iverilog", "-g2012", "-Wall", "-s", "sustained_tb", "-DRESET_ABORT",
                         "-o", directory / "reset_sim", directory / "reports/rtl/top.v", root / "sustained_tb.v"],
                        directory, "reset_build.log")
                    run(["vvp", directory / "reset_sim"], directory, "reset.log")
                    reset = (directory / "reset.log").read_text()
                    assert "SUSTAINED TESTS PASSED" in reset
                    assert [line for line in reset.splitlines() if line.startswith("RESULT ")] == (
                        directory / "sustained_reference.log").read_text().splitlines()
                results.append({"case": name, "frequency_mhz": frequency, "valid": True,
                                "calls": 8, "outputs": len(reference), "events": events,
                                "sustained_calls": 32 if name in ("dsp_map", "dsp_dataflow") else 0,
                                "reset_recovery_calls": 32 if name in ("dsp_map", "dsp_dataflow") else 0,
                                "source_sha256": digest(source), "rtl_sha256": digest(directory / "reports/rtl/top.v")})
            (output / "summary.json").write_text(json.dumps({"valid": len(results) == expected_results,
                "expected_results": expected_results, "results": results,
                "executable_sha256": digest(executable)}, indent=2) + "\n")
            print(f"PASS {name} at {frequency:g} MHz", flush=True)


if __name__ == "__main__":
    main()
