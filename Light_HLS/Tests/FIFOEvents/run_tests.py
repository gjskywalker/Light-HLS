#!/usr/bin/env python3
"""Check FIFO event preservation, ordering, backpressure and negative boundaries."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def run(command, directory, log, success=True):
    environment = dict(os.environ)
    environment["HI_HLS_LIB_PATH"] = str(Path(__file__).resolve().parents[2] / "HLS_Data_Lib")
    with (directory / log).open("w") as stream:
        result = subprocess.run(command, cwd=directory, stdout=stream,
                                stderr=subprocess.STDOUT, timeout=180, env=environment)
    if success and result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {directory / log}")
    return result.returncode


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    executable = args.executable.resolve(strict=True)
    output = (args.output or Path(tempfile.mkdtemp(prefix="light-hls-fifo-events."))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError(f"Refusing to overwrite: {output}")
    print(f"Preserving regression artifacts: {output}", flush=True)
    template = (root / "events.cpp").read_text()
    historical = root.parent / "LocalWriteData/known_failures/private_stream_buffer.cpp"
    variants = [
        ("ordinary_depth1", template, "fifo_events", 8, {"tokens": 64}),
        ("ordinary_depth4", template.replace("depth=1", "depth=4"),
         "fifo_events", 8, {"tokens": 64}),
        ("ordinary_depth3", template.replace("depth=1", "depth=3"),
         "fifo_events", 8, {"tokens": 64}),
        ("pipeline_depth1", template.replace("// PIPELINE_DIRECTIVE", "#pragma HLS PIPELINE II=1"),
         "fifo_events", 8, {"tokens": 64}),
        ("unroll2_depth1", template.replace("// PIPELINE_DIRECTIVE", "#pragma HLS UNROLL factor=2"),
         "fifo_events", 8, {"tokens": 64}),
        ("private_buffer", historical.read_text(), "local_dataflow", 4,
         {"loaded": 32, "computed": 16}),
    ]
    results = []
    for name, source, top, minimum_events, rates in variants:
        directory = output / name
        directory.mkdir()
        design = directory / "design.cpp"
        design.write_text(source)
        run([str(executable), str(design), top, "--board", "u250", "--freq", "100MHz",
             "--output", str(directory)], directory, "light_hls.log")
        intermediates = directory / "reports/.lighthls"
        stages = {}
        for stage in ("top.bc", "top_output_PM0.bc", "top_output_PM1.bc",
                      "top_output_PM2.bc", "top_output_PM4.bc", "top_output.bc"):
            path = intermediates / stage
            run(["opt", "-passes=verify", str(path), "-disable-output"],
                directory, stage + ".verify.log")
            ir = subprocess.check_output(["llvm-dis", str(path), "-o", "-"], text=True)
            count = len(re.findall(r"\b(?:load|store) volatile\b", ir))
            assert count >= minimum_events, (name, stage, count)
            stages[stage] = count
        graph = json.loads((directory / "reports/scheduling/dataflow_graph.json").read_text())
        assert graph["valid"] and graph["rates_balanced"], name
        assert {channel["name"] for channel in graph["channels"]} == set(rates), name
        for channel in graph["channels"]:
            assert channel["rate_known"], channel
            assert channel["production_rate"] == channel["consumption_rate"] == rates[channel["name"]], channel
            expected_pop_capture = name != "pipeline_depth1" and channel["name"] != "loaded"
            assert channel["consumer_read_on_request"] == expected_pop_capture, channel
            rtl = (directory / "reports/rtl/top.v").read_text()
            direct = re.search(rf"assign \w+_q0 = channel_{channel['name']}_0_dout;", rtl)
            assert bool(direct) == expected_pop_capture, channel
        run(["iverilog", "-g2012", "-Wall", "-s", "events_tb", f"-DDUT_NAME={top}",
             "-DFIFO_INTERNAL_MONITOR",
             *(["-DPRIVATE_BUFFER"] if top == "local_dataflow" else []),
             "-o", str(directory / "sim"), str(directory / "reports/rtl/top.v"),
             str(root / "events_tb.v")], directory, "iverilog.log")
        run(["vvp", str(directory / "sim")], directory, "simulation.log")
        assert "FIFO EVENT TESTS PASSED" in (directory / "simulation.log").read_text(), name
        run(["iverilog", "-g2012", "-Wall", "-s", "fifo_stall_driver", f"-DDUT_NAME={top}",
             "-DFIFO_INTERNAL_MONITOR",
             *(["-DPRIVATE_BUFFER"] if top == "local_dataflow" else []),
             "-o", str(directory / "stall_sim"), str(directory / "reports/rtl/top.v"),
             str(root / "events_tb.v"), str(root / "stall_driver.v")], directory, "stall_iverilog.log")
        run(["vvp", str(directory / "stall_sim")], directory, "stall_simulation.log")
        assert "FIFO EVENT TESTS PASSED" in (directory / "stall_simulation.log").read_text(), name
        results.append({"case": name, "valid": True, "source_sha256": digest(design),
                        "calls": 64, "outputs": 1024, "channel_rates": rates,
                        "injected_stall_calls": 64, "injected_stall_outputs": 1024,
                        "volatile_event_counts": stages})
        print(f"PASS {name}: 64 calls, 1024 outputs, rates={rates}", flush=True)

    negatives = {
        "volatile_memory": "void unsupported(volatile unsigned input[16], unsigned output[16]) { output[0] = input[0]; }\n",
        "volatile_memory_in_fifo_task": template.replace("input[i] * seed", "*(volatile unsigned *)(input+i) * seed"),
    }
    for name, source in negatives.items():
        directory = output / name
        directory.mkdir()
        design = directory / "design.cpp"
        design.write_text(source)
        code = run([str(executable), str(design),
                    "unsupported" if name == "volatile_memory" else "fifo_events",
                    "--board", "u250", "--freq", "100MHz", "--output", str(directory)],
                   directory, "light_hls.log", success=False)
        log = (directory / "light_hls.log").read_text()
        assert code > 0 and "BACKEND_UNSUPPORTED_MEMORY_OPERATION" in log, (name, code, log[-2000:])
        assert not (directory / "reports/rtl/top.v").exists(), name
        print(f"PASS negative {name}: explicit volatile-memory rejection", flush=True)
    stub = root.parent / "Light_HLS_Top/include/hls_stream.h"
    summary = {"valid": True, "executable_sha256": digest(executable),
               "stream_header_sha256": digest(stub),
               "test_sources_sha256": {p.name: digest(p) for p in sorted(root.iterdir())
                                        if p.is_file()},
               "historical_reproducer_sha256": digest(historical),
               "calls": 64 * len(results), "outputs": 1024 * len(results),
               "injected_stall_calls": 64 * len(results), "injected_stall_outputs": 1024 * len(results),
               "negative_cases": list(negatives), "results": results}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"ALL FIFO EVENT TESTS PASSED: {summary['calls']} calls, {summary['outputs']} outputs")


if __name__ == "__main__":
    main()
