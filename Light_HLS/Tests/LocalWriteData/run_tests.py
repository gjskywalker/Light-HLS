#!/usr/bin/env python3
"""Validate direct local payloads and conservative multi-writer fallback."""

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


CASES = (
    ("single_ordinary", 0, 4), ("single_pipeline", 0, 4),
    ("single_latency2", 0, 4), ("multiple_ordinary", 1, 0),
    ("multiple_dual", 1, 2), ("partial_banks", 2, 3),
    ("external_only", 3, 0), ("local_dataflow", 4, 1),
)


def run(command, directory, log):
    with (directory / log).open("w") as stream:
        subprocess.run(command, cwd=directory, stdout=stream,
                       stderr=subprocess.STDOUT, check=True, timeout=180)


def check_binding(directory, expected_ports, dataflow=False):
    total = 0
    for report_path in (directory / "reports").rglob("binding.json"):
        report = json.loads(report_path.read_text())
        memories = {m["name"]: m for m in report.get("memory_objects", [])}
        accesses = report.get("memory_access_bindings", [])
        writers = defaultdict(list)
        for access in accesses:
            if access["is_write"]:
                for bank in access["possible_banks"]:
                    writers[(access["memory"], bank, access["port"])].append(access)
        direct_count = 0
        for access in accesses:
            expected_banks = []
            if access["is_write"] and memories[access["memory"]]["storage"] == "local":
                expected_banks = [bank for bank in access["possible_banks"]
                                  if len(writers[(access["memory"], bank, access["port"])]) == 1]
            assert access["direct_write_data_banks"] == expected_banks, access
            direct_count += len(expected_banks)
        assert report["direct_local_write_port_count"] == direct_count, report_path
        total += direct_count
    # Child binding reports are not exported by the current DATAFLOW wrapper.
    assert total == (0 if dataflow else expected_ports), (directory, total, expected_ports)
    rtl = (directory / "reports/rtl/top.v").read_text()
    assignments = re.findall(r"^  assign (buffer_(?:\d+_)?d\d+) = .+;$", rtl, re.M)
    assert len(assignments) == expected_ports, assignments
    for signal in assignments:
        assert f"wire [31:0] {signal};" in rtl
        assert not re.search(rf"^\s+{signal} =", rtl, re.M), signal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    executable = args.executable.resolve(strict=True)
    output = (args.output or Path(tempfile.mkdtemp(prefix="light-hls-local-write."))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError(f"Refusing to overwrite: {output}")
    print(f"Preserving regression artifacts: {output}", flush=True)
    results = []
    for name, kind, ports in CASES:
        directory = output / name
        directory.mkdir()
        run([str(executable), str(root / f"{name}.cpp"), name,
             "--board", "u250", "--freq", "100MHz", "--output", str(directory)],
            directory, "light_hls.log")
        run(["opt", "-passes=verify", str(directory / "reports/.lighthls/top_output.bc"),
             "-disable-output"], directory, "verify.log")
        check_binding(directory, ports, kind == 4)
        run(["iverilog", "-g2012", "-Wall", "-s", "local_write_data_tb",
             f"-DDUT_NAME={name}", f"-DCASE_KIND={kind}",
             *(["-DDATAFLOW_PRIVATE"] if kind == 4 else []), "-o", str(directory / "sim"),
             str(directory / "reports/rtl/top.v"), str(root / "local_write_data_tb.v")],
            directory, "iverilog.log")
        run(["vvp", str(directory / "sim")], directory, "simulation.log")
        simulation = (directory / "simulation.log").read_text()
        assert "LOCAL WRITE DATA TESTS PASSED" in simulation, simulation
        results.append({"case": name, "valid": True, "direct_local_write_ports": ports,
                        "calls": 64, "outputs": 1024})
        print(f"PASS {name}: {simulation.strip()}", flush=True)
    sources = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
               for p in sorted(root.iterdir()) if p.suffix in (".cpp", ".v", ".py", ".sh")}
    summary = {"valid": True, "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
               "test_sources_sha256": sources,
               "calls": sum(r["calls"] for r in results), "outputs": sum(r["outputs"] for r in results),
               "results": results}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"ALL LOCAL WRITE DATA TESTS PASSED: {summary['calls']} calls, {summary['outputs']} outputs")


if __name__ == "__main__":
    main()
