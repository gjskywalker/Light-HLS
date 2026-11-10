#!/usr/bin/env python3
"""Check ordinary-loop bank proofs, port reservations, and generated RTL."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


CASES = (
    "cyclic_parallel", "block_parallel", "same_bank", "dynamic_offset",
    "dynamic_count", "unsigned_wrap", "signed_extension", "descending",
    "mixed_hazards", "nested_fallback",
)


def check_analysis(directory, name):
    binding = json.loads((directory / "reports/binding/binding.json").read_text())
    schedule = json.loads((directory / "reports/scheduling/schedule.json").read_text())
    accesses = [a for a in binding["memory_access_bindings"] if a["memory"] == "input"]
    assert accesses, name
    assert all(a["bank_analysis_source"] == "ordinary_loop" for a in accesses), name
    assert all(a["bank_analysis_reason"] for a in accesses), name
    if name in ("dynamic_offset", "dynamic_count", "nested_fallback"):
        assert all(not a["bank_mapping_exact"] and a["possible_banks"] == [0, 1, 2, 3]
                   for a in accesses), accesses
    elif name == "mixed_hazards":
        assert all(a["bank_mapping_exact"] == a["is_write"] for a in accesses), accesses
        assert all(a["possible_banks"] == [0, 1, 2, 3]
                   for a in accesses if not a["is_write"]), accesses
        assert sorted(a["possible_banks"] for a in accesses if a["is_write"]) == [[0], [1]]
    else:
        assert all(a["bank_mapping_exact"] and a["bank_analysis_iterations"] == 8
                   for a in accesses), accesses
        if name in ("cyclic_parallel", "block_parallel"):
            assert sorted(a["possible_banks"] for a in accesses) == [[0], [1], [2], [3]]
        elif name in ("unsigned_wrap", "signed_extension"):
            assert all(a["possible_banks"] == [0, 1, 2] for a in accesses), accesses
        else:
            assert all(a["possible_banks"] == [0] for a in accesses), accesses

    by_operation = {a["operation"]: a for a in accesses}
    for block in schedule["blocks"]:
        occupied = set()
        read_cycles = []
        for operation in block["instructions"]:
            access = by_operation.get(operation["name"])
            if access is None:
                continue
            if not access["is_write"]:
                read_cycles.append(operation["start_cycle"])
            for bank in access["possible_banks"]:
                slot = (bank, access["port"], operation["start_cycle"])
                assert slot not in occupied, (name, block["name"], slot)
                occupied.add(slot)
        if name in ("cyclic_parallel", "block_parallel") and read_cycles:
            assert len(read_cycles) == 4 and len(set(read_cycles)) == 1, read_cycles
        if name == "same_bank" and read_cycles:
            assert len(read_cycles) == 2 and len(set(read_cycles)) == 2, read_cycles


def run(command, cwd, log):
    with log.open("w") as stream:
        subprocess.run(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT,
                       check=True, timeout=120)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve()
    root = Path(__file__).resolve().parent
    output = args.output or Path(tempfile.mkdtemp(prefix="light-hls-ordinary-banks."))
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError(f"Refusing to overwrite test artifacts: {output}")
    print(f"Preserving regression artifacts: {output}", flush=True)
    results = []
    for kind, name in enumerate(CASES):
        for latency in (1, 2):
            directory = output / f"{name}_latency{latency}"
            directory.mkdir()
            command = [str(executable), str(root / f"{name}.cpp"), name]
            if latency == 2:
                command.append(str(root / "latency2.cfg"))
            command.extend(["--board", "u250", "--freq", "100MHz", "--output", str(directory)])
            run(command, directory, directory / "light_hls.log")
            run(["opt", "-passes=verify", str(directory / "reports/.lighthls/top_output.bc"),
                 "-disable-output"], directory, directory / "verify.log")
            check_analysis(directory, name)
            factor = 3 if kind in (5, 6) else 4
            run(["iverilog", "-g2012", "-Wall", "-s", "ordinary_bank_tb",
                 f"-DDUT_NAME={name}", f"-DCASE_KIND={kind}", f"-DBANK_FACTOR={factor}",
                 f"-DREAD_LATENCY={latency}", "-o", str(directory / "sim"),
                 str(directory / "reports/rtl/top.v"), str(root / "ordinary_bank_tb.v")],
                directory, directory / "iverilog.log")
            run(["vvp", str(directory / "sim")], directory, directory / "simulation.log")
            simulation = (directory / "simulation.log").read_text()
            assert "ORDINARY BANK TESTS PASSED" in simulation, simulation
            results.append({"case": name, "read_latency": latency, "valid": True,
                            "calls": 64, "outputs": 224 if kind == 4 else 512})
            print(f"PASS {name} latency={latency}: {simulation.strip()}", flush=True)
    summary = {"valid": True, "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
               "calls": sum(r["calls"] for r in results),
               "outputs": sum(r["outputs"] for r in results), "results": results}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"ALL ORDINARY BANK TESTS PASSED: {summary['calls']} calls, {summary['outputs']} outputs")


if __name__ == "__main__":
    main()
