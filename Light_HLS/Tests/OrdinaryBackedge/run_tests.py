#!/usr/bin/env python3
"""Check same-edge PHI sampling while preserving registered loop live-outs."""

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
    parser.add_argument("--baseline-executable", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    baseline = args.baseline_executable.resolve(strict=True) if args.baseline_executable else None
    root = Path(__file__).resolve().parent
    output = (args.output or Path(tempfile.mkdtemp(prefix="light-hls-backedge."))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError(f"Refusing to overwrite: {output}")
    print(f"Preserving regression artifacts: {output}", flush=True)
    names = ("nested", "dynamic_trip", "simultaneous", "load_dependent",
             "late_transition", "pipeline", "narrow", "multicycle", "old_phi_liveout")
    results = []
    for kind, name in enumerate(names):
        for latency in (1, 2):
            directory = output / f"{name}_latency{latency}"
            directory.mkdir()
            source = (root / "recurrence.cpp").read_text().replace(
                "#define CASE_KIND 0", f"#define CASE_KIND {kind}")
            if kind == 5:
                source = source.replace("// PIPELINE_DIRECTIVE", "#pragma HLS PIPELINE II=1")
            design = directory / "recurrence.cpp"
            design.write_text(source)
            configuration = directory / "interface.cfg"
            configuration.write_text(f"memory_interface variable=input ports=1 read_latency={latency}\n")
            run([str(executable), str(design), "recurrence", str(configuration),
                 "--board", "u250", "--freq", "100MHz", "--output", str(directory)],
                directory, "light_hls.log")
            run(["opt", "-passes=verify", str(directory / "reports/.lighthls/top_output.bc"),
                 "-disable-output"], directory, "verify.log")
            schedule = json.loads((directory / "reports/scheduling/schedule.json").read_text())
            bypasses = schedule["ordinary_backedge_bypasses"]
            assert len(bypasses) == schedule["ordinary_backedge_bypass_count"]
            if kind in (5, 7):
                assert not bypasses, bypasses
            elif kind != 4:
                assert bypasses, name
            rtl = (directory / "reports/rtl/top.v").read_text()
            operations = {op["name"]: (block, op) for block in schedule["blocks"]
                          for op in block["instructions"]}
            if kind == 4:
                assert all(operations[b["operation"]][1]["opcode"] != "xor" for b in bypasses)
            for bypass in bypasses:
                update, phi = bypass["operation"], bypass["phi"]
                block, op = operations[update]
                assert bypass["result_value_retained"]
                assert op["latency"] == 1 and op["start_cycle"] == bypass["sample_cycle"]
                assert op["start_cycle"] + 1 == block["state_count"]
                storage = bypass["storage_register"]
                assert re.search(rf"^  reg(?: \[[^\]]+\])? {re.escape(storage)};", rtl, re.M)
                if bypass["result_register_retained"]:
                    assert storage == update
                else:
                    assert storage == phi
                    assert not re.search(rf"^  reg(?: \[[^\]]+\])? {re.escape(update)};", rtl, re.M)
                assert re.search(rf"^\s+{re.escape(phi)} <= op_\w+_out;", rtl, re.M)
                assert re.search(rf"^\s+{re.escape(storage)} <= op_\w+_out;", rtl, re.M)
                if kind == 8:
                    assert bypass["result_register_retained"], "The old PHI must survive loop exit"
            run(["iverilog", "-g2012", "-Wall", "-s", "recurrence_tb", f"-DCASE_KIND={kind}",
                 f"-DREAD_LATENCY={latency}", "-o", str(directory / "sim"),
                 str(directory / "reports/rtl/top.v"), str(root / "recurrence_tb.v")],
                directory, "iverilog.log")
            run(["vvp", str(directory / "sim")], directory, "simulation.log")
            match = re.search(r"BACKEDGE TESTS PASSED: 64 calls, 1024 outputs, total_cycles=(\d+)",
                              (directory / "simulation.log").read_text())
            assert match, name
            result = {"case": name, "read_latency": latency, "valid": True,
                      "bypasses": len(bypasses), "calls": 64, "outputs": 1024,
                      "total_cycles": int(match[1])}
            if baseline and kind == 6:
                result["baseline_skipped_reason"] = "Prior compiler emits invalid cross-block integer IR"
            # The prior compiler emits invalid cross-block integer IR for narrow.
            if baseline and kind != 6:
                before = directory / "baseline"
                before.mkdir()
                run([str(baseline), str(design), "recurrence", str(configuration),
                     "--board", "u250", "--freq", "100MHz", "--output", str(before)],
                    before, "light_hls.log")
                run(["iverilog", "-g2012", "-s", "recurrence_tb", f"-DCASE_KIND={kind}",
                     f"-DREAD_LATENCY={latency}", "-o", str(before / "sim"),
                     str(before / "reports/rtl/top.v"), str(root / "recurrence_tb.v")],
                    before, "iverilog.log")
                run(["vvp", str(before / "sim")], before, "simulation.log")
                prior = re.search(r"BACKEDGE TESTS PASSED: 64 calls, 1024 outputs, total_cycles=(\d+)",
                                  (before / "simulation.log").read_text())
                assert prior, name
                result["baseline_total_cycles"] = int(prior[1])
                assert result["total_cycles"] <= int(prior[1]), result
                if kind == 0:
                    assert result["total_cycles"] < int(prior[1]), result
            results.append(result)
            print(f"PASS {name} latency={latency}: {len(bypasses)} bypasses", flush=True)
    summary = {"valid": True, "results": results, "calls": 64 * len(results),
               "outputs": 1024 * len(results),
               "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
               "test_sources_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                        for p in sorted(root.iterdir()) if p.is_file()}}
    if baseline:
        summary["baseline_executable_sha256"] = hashlib.sha256(baseline.read_bytes()).hexdigest()
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"ALL BACKEDGE TESTS PASSED: {summary['calls']} calls")


if __name__ == "__main__":
    main()
