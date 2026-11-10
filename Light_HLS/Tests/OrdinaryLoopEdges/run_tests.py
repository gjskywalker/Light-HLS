#!/usr/bin/env python3
"""Check outer latch feedback and edge-local memory-response PHI sampling."""

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


def simulate(directory, root, kind, latency):
    run(["iverilog", "-g2012", "-Wall", "-s", "outer_edges_tb", f"-DCASE_KIND={kind}",
         f"-DREAD_LATENCY={latency}", "-o", str(directory / "sim"),
         str(directory / "reports/rtl/top.v"), str(root / "outer_edges_tb.v")],
        directory, "iverilog.log")
    run(["vvp", str(directory / "sim")], directory, "simulation.log")
    match = re.search(r"OUTER EDGE TESTS PASSED: 64 calls, outputs=(\d+), total_cycles=(\d+)",
                      (directory / "simulation.log").read_text())
    assert match, directory
    return int(match[1]), int(match[2])


def check_structure(directory, kind, latency):
    schedule = json.loads((directory / "reports/scheduling/schedule.json").read_text())
    rtl = (directory / "reports/rtl/top.v").read_text()
    operations = {op["name"]: (block, op) for block in schedule["blocks"]
                  for op in block["instructions"]}
    outer_wires = [item for item in schedule["ordinary_feedback_wires"]
                   if item["sample_edge"] == "ordinary_loop_latch"]
    bypasses = schedule["ordinary_read_phi_bypasses"]
    assert len(bypasses) == schedule["ordinary_read_phi_bypass_count"]
    if kind == 0:
        assert outer_wires and bypasses
    if kind == 3:
        assert not outer_wires, "Dynamic steps must not become constant-step feedback wires"
    if kind == 5:
        assert len(bypasses) == 2, "Both eligible successor edges must be represented"
    if kind == 7:
        assert not bypasses, "Pipeline initialization uses its separate scheduling path"
    if kind == 8:
        assert len(bypasses) == 1, "Only the last of two port responses may bypass"
        assert sum(op["opcode"] == "load" for _, op in operations.values()) == 2
    if kind == 9:
        assert not bypasses, "A branch using the captured load delays PHI sampling"
    for wire in outer_wires:
        update, phi = wire["operation"], wire["phi"]
        assert operations[update][1]["latency"] == 0
        assert re.search(rf"^  wire(?: \[[^\]]+\])? {re.escape(update)} =", rtl, re.M)
        assert re.search(rf"^  reg(?: \[[^\]]+\])? {re.escape(phi)};", rtl, re.M)
        assert f"{phi} <= {update};" in rtl
        assert not re.search(rf"^\s+{re.escape(update)} <=", rtl, re.M)
    for bypass in bypasses:
        load, phi = bypass["load"], bypass["phi"]
        block, operation = operations[load]
        assert bypass["load_register_retained"]
        assert operation["latency"] == latency + 1
        assert bypass["sample_cycle"] == operation["start_cycle"] + latency
        assert bypass["sample_cycle"] + 1 == block["state_count"]
        assert re.search(rf"^  reg(?: \[[^\]]+\])? {re.escape(load)};", rtl, re.M)
        state = block["first_state"] + bypass["sample_cycle"]
        body = re.search(rf"^\s+\d+'d{state}: begin\n(.*?)(?=^\s+\d+'d\d+: begin|^\s+default:)",
                         rtl.rsplit("case (state)", 1)[-1], re.M | re.S)
        assert body, (state, bypass)
        for value in (load, phi):
            assert f"{value} <= input_r_q0;" in body[1], (value, body[1])
    return len(outer_wires), len(bypasses)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--baseline-executable", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    baseline = args.baseline_executable.resolve(strict=True) if args.baseline_executable else None
    root = Path(__file__).resolve().parent
    output = (args.output or Path(tempfile.mkdtemp(prefix="light-hls-outer-edges."))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError(f"Refusing to overwrite: {output}")
    names = ("nested", "zero_dynamic_outer", "descending", "dynamic_step",
             "load_liveout", "two_successors", "division_liveout", "inner_pipeline",
             "two_port_responses", "load_branch")
    results = []
    for kind, name in enumerate(names):
        for latency in (1, 2):
            directory = output / f"{name}_latency{latency}"
            directory.mkdir()
            source = (root / "outer_edges.cpp").read_text().replace(
                "#define CASE_KIND 0", f"#define CASE_KIND {kind}")
            if kind == 7:
                source = source.replace("// PIPELINE_DIRECTIVE", "#pragma HLS PIPELINE II=1")
            design = directory / "outer_edges.cpp"
            design.write_text(source)
            configuration = directory / "interface.cfg"
            configuration.write_text(f"memory_interface variable=input ports=1 read_latency={latency}\n")
            run([str(executable), str(design), "outer_edges", str(configuration),
                 "--board", "u250", "--freq", "100MHz", "--output", str(directory)],
                directory, "light_hls.log")
            run(["opt", "-passes=verify", str(directory / "reports/.lighthls/top_output.bc"),
                 "-disable-output"], directory, "verify.log")
            outer_wires, read_bypasses = check_structure(directory, kind, latency)
            outputs, cycles = simulate(directory, root, kind, latency)
            result = {"case": name, "read_latency": latency, "valid": True,
                      "outer_feedback_wires": outer_wires, "read_phi_bypasses": read_bypasses,
                      "calls": 64, "outputs": outputs, "total_cycles": cycles}
            if baseline:
                before = directory / "baseline"
                before.mkdir()
                run([str(baseline), str(design), "outer_edges", str(configuration),
                     "--board", "u250", "--freq", "100MHz", "--output", str(before)],
                    before, "light_hls.log")
                before_outputs, before_cycles = simulate(before, root, kind, latency)
                assert outputs == before_outputs and cycles <= before_cycles, result
                if kind == 0:
                    assert cycles < before_cycles
                result["baseline_total_cycles"] = before_cycles
            results.append(result)
            print(f"PASS {name} latency={latency}: {outer_wires} outer wires, "
                  f"{read_bypasses} read bypasses, {cycles} cycles", flush=True)
    summary = {"valid": True, "results": results, "calls": 64 * len(results),
               "outputs": sum(item["outputs"] for item in results),
               "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
               "test_sources_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                        for p in sorted(root.iterdir()) if p.is_file()}}
    if baseline:
        summary["baseline_executable_sha256"] = hashlib.sha256(baseline.read_bytes()).hexdigest()
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"ALL OUTER EDGE TESTS PASSED: {summary['calls']} calls")


if __name__ == "__main__":
    main()
