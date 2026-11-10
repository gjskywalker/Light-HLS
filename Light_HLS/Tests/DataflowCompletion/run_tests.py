#!/usr/bin/env python3
"""Require completion of producer effects after its last FIFO token."""
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

    def run(command, name):
        with (output / (name + ".log")).open("w") as stream:
            subprocess.run([str(item) for item in command], cwd=output, env=environment,
                           stdout=stream, stderr=subprocess.STDOUT, timeout=180, check=True)

    run([executable, root / "late_effects.cpp", "late_effects", "--board", "u250",
         "--freq", "100MHz", "--output", output], "light_hls")
    graph = json.loads((output / "reports/scheduling/dataflow_graph.json").read_text())
    assert set(graph["completion_tasks"]) == {"produce_tail_0", "consume_tail_1"}, graph
    assert graph["retirement_cycles"] == 0, graph
    for stalled in (False, True):
        name = "stalled" if stalled else "natural"
        run(["iverilog", "-g2012", "-Wall", "-s", "late_effects_tb",
             *(["-DINJECT_STALLS"] if stalled else []), "-o", output / name,
             output / "reports/rtl/top.v", root / "late_effects_tb.v"], name + "_compile")
        run(["vvp", output / name], name)
        assert "DATAFLOW COMPLETION TESTS PASSED" in (output / (name + ".log")).read_text()
    (output / "summary.json").write_text(json.dumps({"valid": True, "calls": 128,
        "committed_writes": 10240, "completion_tasks": graph["completion_tasks"],
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest()}, indent=2) + "\n")
    print("PASS: DATAFLOW trailing effects, natural and injected stalls", flush=True)


if __name__ == "__main__":
    main()
