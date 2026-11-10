#!/usr/bin/env python3
"""Check held-PHI feedback wires against memory edges and independent outputs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def run(command, directory, log):
    environment = dict(os.environ)
    environment["HI_HLS_LIB_PATH"] = str(Path(__file__).resolve().parents[2] / "HLS_Data_Lib")
    with (directory / log).open("w") as stream:
        subprocess.run(command, cwd=directory, stdout=stream, stderr=subprocess.STDOUT,
                       env=environment, check=True, timeout=180)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    root = Path(__file__).resolve().parent
    output = (args.output or Path(tempfile.mkdtemp(prefix="light-hls-feedback."))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError(f"Refusing to overwrite: {output}")
    print(f"Preserving regression artifacts: {output}", flush=True)
    names = ("wrap", "descending", "dynamic_count", "dynamic_step",
             "early_exit", "pipeline", "conditional_step", "signed_return")
    cases = [(name, kind, latency, "100MHz") for kind, name in enumerate(names)
             for latency in (1, 2)]
    # Three nanoseconds is the tightest characterized U250 library period.
    cases.append(("constrained_clock", 0, 1, "333.3333333333333MHz"))
    results = []
    for name, kind, latency, frequency in cases:
        directory = output / f"{name}_latency{latency}"
        directory.mkdir()
        design = directory / "feedback.cpp"
        source = (root / "feedback.cpp").read_text().replace(
            "#define CASE_KIND 0", f"#define CASE_KIND {kind}")
        # Directive discovery precedes preprocessing; remove inactive pragmas.
        if kind != 5:
            source = source.replace("#pragma HLS PIPELINE II=1", "")
        design.write_text(source)
        configuration = directory / "interface.cfg"
        configuration.write_text(f"memory_interface variable=input ports=1 read_latency={latency}\n")
        run([str(executable), str(design), "feedback", str(configuration),
             "--board", "u250", "--freq", frequency, "--output", str(directory)],
            directory, "light_hls.log")
        run(["opt", "-passes=verify", str(directory / "reports/.lighthls/top_output.bc"),
             "-disable-output"], directory, "verify.log")
        schedule = json.loads((directory / "reports/scheduling/schedule.json").read_text())
        wires = schedule["ordinary_feedback_wires"]
        assert schedule["ordinary_feedback_wire_count"] == len(wires), name
        if name == "pipeline":
            assert not wires, (name, wires)
        elif name != "early_exit":
            assert wires, name
        rtl = (directory / "reports/rtl/top.v").read_text()
        operations = {op["name"]: op for block in schedule["blocks"] for op in block["instructions"]}
        for wire in wires:
            update, phi = wire["operation"], wire["phi"]
            assert wire["sample_edge"] == "ordinary_self_backedge", wire
            assert operations[update]["latency"] == 0, wire
            assert re.search(rf"^  wire(?: \[[^\]]+\])? {re.escape(update)} =", rtl, re.M), wire
            assert re.search(rf"^  reg(?: \[[^\]]+\])? {re.escape(phi)};", rtl, re.M), wire
            assert not re.search(rf"^\s+{re.escape(update)} <=", rtl, re.M), wire
            assert f"{phi} <= {update};" in rtl, wire
        run(["iverilog", "-g2012", "-Wall", "-s", "feedback_tb", f"-DCASE_KIND={kind}",
             f"-DREAD_LATENCY={latency}", "-o", str(directory / "sim"),
             str(directory / "reports/rtl/top.v"), str(root / "feedback_tb.v")],
            directory, "iverilog.log")
        run(["vvp", str(directory / "sim")], directory, "simulation.log")
        assert "ORDINARY FEEDBACK TESTS PASSED" in (directory / "simulation.log").read_text()
        result = {"case": name, "read_latency": latency, "frequency": frequency,
                  "feedback_wires": len(wires), "calls": 64, "valid": True}
        results.append(result)
        print(f"PASS {name} latency={latency}: {len(wires)} feedback wires, 64 calls", flush=True)
    summary = {"valid": True, "results": results, "calls": 64 * len(results),
               "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
               "test_sources_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                        for p in sorted(root.iterdir()) if p.is_file()}}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"ALL ORDINARY FEEDBACK TESTS PASSED: {summary['calls']} calls and returns")


if __name__ == "__main__":
    main()
