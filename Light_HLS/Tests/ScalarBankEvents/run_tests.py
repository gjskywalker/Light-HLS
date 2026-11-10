#!/usr/bin/env python3
"""Check scalar-bank fanout, pre-write sampling, and explicit latency fallback."""

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
    results = []
    for kind, name in enumerate(("ordinary_rw", "pipeline_rw", "pipeline_fanout")):
        for latency in (0, 1, 2):
            directory = output / f"{name}_latency{latency}"
            directory.mkdir()
            source = (root / "events.cpp").read_text().replace("#define CASE_KIND 0", f"#define CASE_KIND {kind}")
            if kind == 0:
                source = source.replace("#pragma HLS PIPELINE II=1", "")
            (directory / "events.cpp").write_text(source)
            tb = (root / "events_tb.v").read_text()
            (directory / "events_tb.v").write_text(tb)
            config = directory / "interface.cfg"
            config.write_text(f"memory_interface variable=data ports=1 read_latency={latency}\n" if latency else "")
            commands = [
                ("light_hls.log", [str(executable), str(directory / "events.cpp"), "scalar_bank_events", str(config),
                                   "--board", "u250", "--freq", "100MHz", "--output", str(directory)]),
                ("verify.log", ["opt", "-passes=verify", str(directory / "reports/.lighthls/top_output.bc"), "-disable-output"]),
                ("iverilog.log", ["iverilog", "-g2012", "-Wall", "-s", "scalar_bank_events_tb", f"-DCASE_KIND={kind}"] +
                 (["-DREAD_ONLY"] if kind == 2 else []) + ["-o", str(directory / "sim"),
                  str(directory / "reports/rtl/top.v"), str(directory / "events_tb.v")]),
                ("simulation.log", ["vvp", str(directory / "sim")]),
            ]
            for log, command in commands:
                with (directory / log).open("w") as stream:
                    subprocess.run(command, cwd=directory, stdout=stream, stderr=subprocess.STDOUT,
                                   env=environment, check=True, timeout=180)
            assert "SCALAR BANK EVENTS PASSED" in (directory / "simulation.log").read_text()
            binding = json.loads((directory / "reports/binding/binding.json").read_text())
            memory = next(item for item in binding["memory_objects"] if item["name"] == "data")
            assert memory["read_latency"] == latency, memory
            assert memory["combinational_scalar_reads"] == (latency == 0), memory
            if kind == 2 and latency == 0:
                pipeline = json.loads((directory / "reports/scheduling/pipeline_schedule.json").read_text())["pipelines"][0]
                assert pipeline["memory_mii"] == 1 and pipeline["achieved_ii"] == 1, pipeline
            results.append({"case": name, "read_latency": latency, "calls": 64, "valid": True})
            print(f"PASS {name} latency={latency}: 64 calls", flush=True)
    summary = {"valid": True, "results": results, "calls": 64 * len(results),
               "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
               "test_sources_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                        for p in sorted(root.iterdir()) if p.is_file()}}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
