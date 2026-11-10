#!/usr/bin/env python3
"""Compare unpaced transaction streams on unchanged Light-HLS and Vitis RTL."""
import argparse
import json
from pathlib import Path
import re
import statistics

from run_u250_fair_comparison import executable_path, run_command, run_simulation, sha256_file


def summarize(lines):
    events = [dict((key, int(value)) for key, value in re.findall(r"(\w+)=(\d+)", line))
              for line in lines if line.startswith("SUSTAINED trial=")]
    if len(events) != 32 or [e.get("trial") for e in events] != list(range(32)):
        raise ValueError("Missing or unordered sustained transaction events")
    fields = ("acknowledged", "last_commit", "done")
    intervals = {}
    for field in fields:
        values = [right[field] - left[field] for left, right in zip(events, events[1:])]
        if any(value <= 0 for value in values):
            raise ValueError("Nonmonotone sustained transaction events")
        intervals[field] = values
    # Discard the first four transaction boundaries, but retain all raw samples.
    steady = intervals["last_commit"][4:]
    return {"events": events, "intervals": intervals, "warmup_intervals": 4,
            "steady_commit_interval_cycles": statistics.fmean(steady),
            "steady_commit_interval_min": min(steady), "steady_commit_interval_max": max(steady),
            "steady_outputs_per_cycle": 256 / statistics.fmean(steady)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--comparison", type=Path)
    inputs.add_argument("--rtl-root", type=Path, help="Generated comparison tree; physical measurement may still be running")
    parser.add_argument("--source", type=Path, help="Required with --rtl-root")
    parser.add_argument("--light-rtl", type=Path, help="Explicit repaired Light-HLS RTL; cached comparison supplies Vitis only")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    comparison = args.comparison.resolve(strict=True) if args.comparison else None
    report = json.loads(comparison.read_text()) if comparison else None
    if not comparison and not args.source:
        parser.error("--rtl-root requires --source")
    source = Path(report["source"]) if report else args.source.resolve(strict=True)
    if source.name not in ("dsp_map.cpp", "dsp_dataflow.cpp") or (report and sha256_file(source) != report["source_sha256"]):
        raise ValueError("This traffic contract supports the immutable DSP pressure fixtures only")
    root = comparison.parent if comparison else args.rtl_root.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("Refusing to overwrite throughput results")
    fixture = Path(__file__).resolve().parents[1] / "QoRPressure"
    result = {"valid": False, "comparison_sha256": sha256_file(comparison) if comparison else None,
              "rtl_root": str(root),
              "light_rtl_override": str(args.light_rtl.resolve(strict=True)) if args.light_rtl else None,
              "source_sha256": sha256_file(source), "testbench_sha256": sha256_file(fixture / "sustained_tb.v"),
              "contract": "32 held-start offers, changing coefficient, immutable input RAM, one-cycle reads, no output backpressure",
              "note": "Ready is an acknowledgement, not necessarily the first execution edge. Throughput uses final commits.",
              "results": {}}
    try:
        run_command(["clang++", "-std=c++17", "-Wno-unknown-pragmas", "-I", str(fixture / "software"),
                     str(source), str(fixture / "sustained_reference.cpp"), "-o", str(output / "reference")],
                    output, output / "reference_build.log", 120)
        run_command([str(output / "reference")], output, output / "reference.log", 120)
        reference = [line for line in (output / "reference.log").read_text().splitlines()
                     if line.startswith("RESULT ")]
        for generator in ("light_hls", "vitis_hls"):
            rtl = ([root / "light_hls/reports/rtl/top.v"] if generator == "light_hls" else
                   sorted((root / "vitis_hls/vitis_project/solution/syn/verilog").glob("*.v")))
            if generator == "light_hls" and args.light_rtl:
                rtl = [args.light_rtl.resolve(strict=True)]
            hashes = {str(path): sha256_file(path) for path in rtl}
            directory = output / generator
            directory.mkdir()
            defines = ["PRESSURE_MEMORY_DIRECTIONS"] if generator == "vitis_hls" and source.name == "dsp_dataflow.cpp" else []
            simulation = run_simulation(executable_path("iverilog", "Icarus"), rtl, fixture / "sustained_tb.v",
                "sustained_tb", directory, 900, defines, "xsim")
            if simulation["trace"] != reference:
                raise ValueError(f"{generator} does not match the C reference")
            if hashes != {str(path): sha256_file(path) for path in rtl}:
                raise ValueError("RTL changed during simulation")
            result["results"][generator] = {"rtl_sha256": hashes, "reference_outputs": len(reference),
                **summarize((directory / "simulation.log").read_text().splitlines())}
        result["valid"] = True
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        (output / "sustained_throughput.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value["steady_commit_interval_cycles"] for key, value in result["results"].items()}))


if __name__ == "__main__":
    main()
