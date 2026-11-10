#!/usr/bin/env python3
"""Validate DAPO-II integer benchmark RTL against the unchanged source program."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SPECS = {
    "substring": {
        "source": "Dataset/HLSDesigns/substring/substring_opt.cc",
        "arguments": [(f"str_{i}", 4096) for i in range(8)],
        "writable": [],
        "coverage": "Exact _opt source, including its unusual first loop; bounded values, boundary-spanning matches and random data.",
    },
    "getTanh": {
        "source": "Dataset/HLSDesigns/getTanh/getTanh_opt.cc",
        "arguments": [("A", 1000), ("addr_in", 1000), ("addr_out", 1000),
                      ("atanh", 12), ("sinh", 5), ("cosh", 5)],
        "writable": ["A"],
        "coverage": "Exact _opt shift expression, nonnegative bounded initial angles, bounded lookup tables, nonzero divisors; negative initial angles and arbitrary lookup tables are not covered.",
    },
    "bnn": {
        "source": "Dataset/HLSDesigns/BNNKernel/bnn_opt.cc",
        "arguments": [("addr_in", 10000), ("alpha", 0), ("w", 10000),
                      ("addr_out", 10000), ("data", 10000), ("in", 10000),
                      ("mean", 10000)],
        "writable": ["data"],
        "coverage": "Bounded signed arithmetic, independent argument allocations, indirect updates with identity, random and strongly colliding addresses.",
    },
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def tool_path(value: str) -> str:
    resolved = shutil.which(value)
    if not resolved:
        raise RuntimeError(f"Required executable not found: {value}")
    # Driver names such as clang++ select language-specific link libraries.
    return str(Path(resolved).absolute())


class Commands:
    """Retain every command, output and exit status, including failed commands."""

    def __init__(self, root: Path, timeout: int):
        self.root = root
        self.timeout = timeout
        self.records: list[dict] = []

    def run(self, name: str, argv: list[str], timeout: int | None = None) -> str:
        log_path = self.root / f"{name}.log"
        record = {"name": name, "argv": argv, "cwd": str(self.root),
                  "timeout_seconds": timeout or self.timeout, "log": str(log_path)}
        self.records.append(record)
        write_json(self.root / "commands.json", self.records)
        print(f"[{name}] {' '.join(argv)}", flush=True)
        started = time.monotonic()
        with log_path.open("w", encoding="utf-8") as log_file:
            process = subprocess.Popen(argv, cwd=self.root, stdout=log_file,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            record.update({"pid": process.pid, "status": "running"})
            write_json(self.root / "commands.json", self.records)
            try:
                record["returncode"] = process.wait(timeout=timeout or self.timeout)
                record["status"] = "completed" if process.returncode == 0 else "failed"
            except subprocess.TimeoutExpired:
                print(f"[{name}] Hard timeout reached; terminating its process group", flush=True)
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                record["returncode"] = process.returncode
                record["status"] = "timeout"
        text = log_path.read_text(encoding="utf-8", errors="replace")
        record["elapsed_seconds"] = time.monotonic() - started
        record["log_sha256"] = sha256(log_path)
        write_json(self.root / "commands.json", self.records)
        if record["returncode"] != 0 or record["status"] == "timeout":
            raise RuntimeError(f"{name} failed ({record.get('status', record['returncode'])}); see {log_path}")
        return text


def make_case(design: str, trial: int, seed: int) -> dict[str, list[int] | int]:
    """Generate legal inputs without changing the benchmark's source semantics."""
    rng = random.Random(seed + trial * 104729)
    kind = trial % 4
    if design == "substring":
        values = {f"str_{i}": [0 if kind == 0 else
                  ((j + i * 4096) % 3 + 1 if kind == 1 else rng.randrange(5))
                  for j in range(4096)] for i in range(8)}
        if kind >= 2:
            for i in range(1, 7):
                values[f"str_{i}"][4094:] = [1, 2]
                values[f"str_{i + 1}"][0] = 3
                values[f"str_{i}"][4093] = 4
        return values
    if design == "bnn":
        def address(i: int) -> int:
            return i if kind == 0 else (rng.randrange(32) if kind == 2 else rng.randrange(10000))
        return {
            "addr_in": [address(i) for i in range(10000)],
            "alpha": [2, -3, 2, 0][kind],
            "w": [rng.randrange(16) for _ in range(10000)],
            "addr_out": [address(i) for i in range(10000)],
            "data": [rng.randrange(-1000, 1001) for _ in range(10000)],
            "in": [rng.randrange(16) for _ in range(10000)],
            "mean": [rng.randrange(-100, 101) for _ in range(10000)],
        }
    if design == "getTanh":
        if kind == 3:
            # Positive outputs can safely become subsequent positive inputs.
            return {
                "A": [rng.randrange(1000, 4001) for _ in range(1000)],
                "addr_in": [rng.randrange(1000) for _ in range(1000)],
                "addr_out": [rng.randrange(1000) for _ in range(1000)],
                "atanh": [rng.randrange(1, 33) for _ in range(12)],
                "sinh": [0] * 5, "cosh": [4096] * 5,
            }
        edges = [0, 1, 4095, 4096, 8191, 8192, 20479, 20480, 25000]
        angles = ([25000] * 500 if kind == 0 else
                  [edges[i % len(edges)] if kind == 1 else rng.randrange(25001)
                   for i in range(500)])
        return {
            "A": angles + [0x1357] * 500,
            "addr_in": [i % 500 if kind < 2 else rng.randrange(500) for i in range(1000)],
            "addr_out": [500 + (i % 500 if kind < 2 else rng.randrange(500)) for i in range(1000)],
            "atanh": [rng.randrange(1, 1025) for _ in range(12)],
            "sinh": [0] * 5, "cosh": [4096] * 5,
        }
    raise ValueError(f"Unknown design: {design}")


def pack_ints(values: list[int]) -> bytes:
    return struct.pack(f"<{len(values)}i", *values)


def write_hex(path: Path, values: list[int]) -> None:
    path.write_text("".join(f"{value & 0xffffffff:08x}\n" for value in values), encoding="ascii")


def wrapper_source(spec: dict, symbol: str | None = None) -> str:
    parameters = ", ".join("int *" if depth else "int" for _, depth in spec["arguments"])
    linkage = f" __asm__({json.dumps(symbol)})" if symbol else ""
    lines = ["#include <cstdint>", "#include <fstream>", "#include <iostream>",
             "#include <vector>", "static_assert(sizeof(int) == 4, \"32-bit int required\");",
             f"int dut({parameters}){linkage};", "int main(int argc, char **argv) {",
             "  if (argc != 3) return 2;", "  std::ifstream input(argv[1], std::ios::binary);",
             "  std::ofstream output(argv[2], std::ios::binary);",
             "  if (!input || !output) return 3;"]
    for name, depth in spec["arguments"]:
        if depth:
            lines.extend([f"  std::vector<int> {name}({depth});",
                          f"  input.read(reinterpret_cast<char *>({name}.data()), {depth} * sizeof(int));"])
        else:
            lines.extend([f"  int {name};", f"  input.read(reinterpret_cast<char *>(&{name}), sizeof(int));"])
    lines.append("  if (!input || input.peek() != std::char_traits<char>::eof()) return 4;")
    arguments = ", ".join(f"{name}.data()" if depth else name for name, depth in spec["arguments"])
    lines.extend([f"  const int result = dut({arguments});",
                  "  output.write(reinterpret_cast<const char *>(&result), sizeof(result));"])
    for name, depth in spec["arguments"]:
        if depth:
            lines.append(f"  output.write(reinterpret_cast<const char *>({name}.data()), {depth} * sizeof(int));")
    lines.extend(["  if (!output) return 5;", "  std::cout << \"REFERENCE_OK return=\" << result << '\\n';",
                  "  return 0;", "}"])
    return "\n".join(lines) + "\n"


def parse_ports(rtl: str) -> tuple[str, dict[str, tuple[str, int]]]:
    header = re.search(r"\bmodule\s+([A-Za-z_$][\w$]*)\s*\((.*?)\);", rtl, re.S)
    if not header:
        raise RuntimeError("Expected an ANSI-style generated RTL module header")
    ports = {}
    for item in header.group(2).split(","):
        item = re.sub(r"//[^\n]*", "", item).strip()
        match = re.fullmatch(r"(input|output)\s+(?:(?:wire|reg|logic)\s+)?(?:signed\s+)?"
                             r"(?:\[\s*(\d+)\s*:\s*(\d+)\s*\]\s+)?([A-Za-z_$][\w$]*)", item)
        if not match:
            raise RuntimeError(f"Unsupported RTL port declaration: {item}")
        width = abs(int(match[2]) - int(match[3])) + 1 if match[2] else 1
        ports[match[4]] = (match[1], width)
    return header[1], ports


def read_memories(reports: Path) -> list[dict]:
    paths = [reports / "binding/binding.json", reports / "memory/memory_objects.json"]
    for path in paths:
        if path.is_file():
            report = json.loads(path.read_text())
            if isinstance(report, list):
                return report
            if "memory_objects" in report:
                return report["memory_objects"]
    raise RuntimeError("No memory-object report found in the candidate reports")


def testbench_source(spec: dict, memories: list[dict], rtl: str, cases: list[dict],
                     output: Path, max_cycles: int) -> str:
    module, ports = parse_ports(rtl)
    arrays = {name: depth for name, depth in spec["arguments"] if depth}
    scalars = {name for name, depth in spec["arguments"] if not depth}
    required = {"ap_clk", "ap_rst", "ap_start", "ap_done", "ap_ready", "ap_idle", "ap_return"}
    if not required.issubset(ports):
        raise RuntimeError(f"Missing expected control/return ports: {sorted(required - ports.keys())}")
    connected_inputs = {"ap_clk", "ap_rst", "ap_start"}
    scalar_ports = {}
    for name in scalars:
        choices = [p for p in (name, name + "_r") if p in ports]
        if len(choices) != 1 or ports[choices[0]] != ("input", 32):
            raise RuntimeError(f"Cannot identify 32-bit scalar input {name}")
        scalar_ports[name] = choices[0]
        connected_inputs.add(choices[0])
    lines = ["`timescale 1ns/1ps", "module dapoii_tb;"]
    for name, (direction, width) in ports.items():
        kind = "reg" if direction == "input" else "wire"
        vector = f"[{width - 1}:0] " if width > 1 else ""
        initial = " = 0" if direction == "input" else ""
        lines.append(f"  {kind} {vector}{name}{initial};")
    for name, depth in arrays.items():
        lines.extend([f"  reg [31:0] mem_{name} [0:{depth - 1}];",
                      f"  reg [31:0] expected_{name} [0:{depth - 1}];"])
    lines.extend(["  integer tick = 0, accepted = -1, last_write = -1;",
                  "  integer reads = 0, writes = 0, index, cycles, observed_done;",
                  "  reg active = 0;", "  reg [31:0] expected_return;",
                  f"  {module} kernel (" + ", ".join(f".{name}({name})" for name in ports) + ");",
                  "  always #5 ap_clk = ~ap_clk;",
                  "  always @(posedge ap_clk) begin", "    tick = tick + 1;",
                  "    if (!ap_rst) begin",
                  "      if (ap_start && ap_ready) begin",
                  "        if (active) $fatal(1, \"Duplicate transaction acceptance\");",
                  "        accepted = tick; active = 1;", "      end"])
    for memory in memories:
        if memory.get("storage") != "external":
            continue
        name = memory["name"]
        if name not in arrays:
            raise RuntimeError(f"Unknown external memory object: {name}")
        if memory.get("protocol") != "ap_memory" or memory.get("banks", 1) != 1:
            raise RuntimeError(f"Only unpartitioned ap_memory is supported by this benchmark verifier: {name}")
        if memory.get("element_width") != 32 or memory.get("read_latency", 1) != 1:
            raise RuntimeError(f"Expected 32-bit, one-cycle memory {name}")
        prefix = memory.get("port_name", name)
        seen_ports = []
        for port in range(memory.get("ports", 1)):
            address, ce, we = (f"{prefix}_{suffix}{port}" for suffix in ("address", "ce", "we"))
            data, q = f"{prefix}_d{port}", f"{prefix}_q{port}"
            if ce not in ports or address not in ports:
                raise RuntimeError(f"Missing memory port {prefix}/{port}")
            read = q in ports
            write = we in ports
            if write and data not in ports:
                raise RuntimeError(f"Write-enable without write data for {prefix}/{port}")
            if read:
                connected_inputs.add(q)
            seen_ports.append((address, ce, we if write else None))
            lines.extend([f"      if ({ce} === 1'bx) $fatal(1, \"Unknown CE: {ce}\");",
                          f"      if ({ce}) begin",
                          f"        if (!active) $fatal(1, \"Memory access outside a transaction: {name}\");",
                          f"        if ((^{address}) === 1'bx || $unsigned({address}) >= {arrays[name]})",
                          f"          $fatal(1, \"Out-of-bounds/unknown address: {address}=%h\", {address});"])
            if read:
                lines.append(f"        {q} <= mem_{name}[{address}];")
            if write:
                lines.extend([f"        if ({we} === 1'bx) $fatal(1, \"Unknown WE: {we}\");",
                              f"        if ({we}) begin"])
                if name not in spec["writable"]:
                    lines.append(f"          $fatal(1, \"Unexpected write to read-only argument {name}\");")
                lines.extend([f"          if ((^{data}) === 1'bx) $fatal(1, \"Unknown write data: {data}\");",
                              f"          mem_{name}[{address}] <= {data};",
                              "          writes = writes + 1; last_write = tick;",
                              "        end" + (" else reads = reads + 1;" if read else "")])
            elif read:
                lines.append("        reads = reads + 1;")
            lines.append("      end")
        if len(seen_ports) == 2 and seen_ports[0][2] and seen_ports[1][2]:
            a0, c0, w0 = seen_ports[0]
            a1, c1, w1 = seen_ports[1]
            lines.extend([f"      if ({c0} && {w0} && {c1} && {w1} && {a0} == {a1})",
                          f"        $fatal(1, \"Simultaneous writes to the same address: {name}\");"])
    unconnected = {name for name, (direction, _) in ports.items() if direction == "input"} - connected_inputs
    if unconnected:
        raise RuntimeError(f"Unmodeled RTL input ports: {sorted(unconnected)}")
    lines.extend(["    end", "  end", "  initial begin", "    ap_rst = 1;",
                  "    repeat (3) @(negedge ap_clk);", "    ap_rst = 0;"])
    for trial, case in enumerate(cases):
        if trial > 0 and trial % 2 == 0:
            lines.extend(["    ap_rst = 1;", "    repeat (3) @(negedge ap_clk);", "    ap_rst = 0;"])
        case_root = output / f"case_{trial:03d}"
        for name in arrays:
            lines.append(f"    $readmemh({json.dumps(str(case_root / ('input_' + name + '.hex')))}, mem_{name});")
            lines.append(f"    $readmemh({json.dumps(str(case_root / ('expected_' + name + '.hex')))}, expected_{name});")
        for name, port in scalar_ports.items():
            lines.append(f"    {port} = 32'h{int(case['inputs'][name]) & 0xffffffff:08x};")
        lines.extend([f"    expected_return = 32'h{case['return'] & 0xffffffff:08x};",
                      "    accepted = -1; last_write = -1; reads = 0; writes = 0;",
                      "    observed_done = -1; cycles = 0; ap_start = 1;",
                      "    while (accepted < 0 && cycles < 100) begin",
                      "      @(negedge ap_clk); cycles = cycles + 1;", "    end",
                      "    if (accepted < 0) $fatal(1, \"No start acceptance\");",
                      "    ap_start = 0;"])
        for port in scalar_ports.values():
            lines.append(f"    {port} = ~{port};")
        lines.extend([f"    while (!ap_done && tick - accepted < {max_cycles}) @(negedge ap_clk);",
                      f"    if (!ap_done) $fatal(1, \"Transaction timeout: trial={trial}\");",
                      "    observed_done = tick;",
                      f"    if (ap_return !== expected_return) $fatal(1, \"Return mismatch trial={trial}: expected=%h actual=%h\", expected_return, ap_return);"])
        for name, depth in arrays.items():
            lines.extend([f"    for (index = 0; index < {depth}; index = index + 1)",
                          f"      if (mem_{name}[index] !== expected_{name}[index])",
                          f"        $fatal(1, \"Memory mismatch trial={trial} {name}[%0d]: expected=%h actual=%h\", index, expected_{name}[index], mem_{name}[index]);"])
        lines.extend([f"    $display(\"RESULT trial={trial} cycles=%0d last_write_cycle=%0d reads=%0d writes=%0d\", observed_done - accepted, last_write < 0 ? -1 : last_write - accepted, reads, writes);",
                      "    active = 0;", "    repeat (3) @(negedge ap_clk);"])
    lines.extend([f"    $display(\"DAPOII RTL VERIFICATION PASSED: {len(cases)} cases\");",
                  "    $finish;", "  end", "endmodule"])
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--design", choices=SPECS, required=True)
    parser.add_argument("--reports", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ir", type=Path)
    parser.add_argument("--cases", type=int, default=4)
    parser.add_argument("--seed", type=int, default=17)
    parser.add_argument("--timeout-seconds", type=int, default=300)
    parser.add_argument("--max-cycles", type=int, default=5000000)
    parser.add_argument("--reference-only", action="store_true")
    parser.add_argument("--clangxx", default="clang++")
    parser.add_argument("--llvm-dis", default="llvm-dis")
    parser.add_argument("--iverilog", default="iverilog")
    parser.add_argument("--vvp", default="vvp")
    args = parser.parse_args(argv)
    if args.cases <= 0 or args.timeout_seconds <= 0 or args.max_cycles <= 0:
        parser.error("Cases, timeout and maximum cycles must be positive")
    if not args.reference_only and args.reports is None:
        parser.error("--reports is required unless --reference-only is selected")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / "verification.json").exists():
        parser.error(f"Refusing to overwrite previous validation results: {output}")
    spec = SPECS[args.design]
    source = ROOT / spec["source"]
    result = {"schema_version": "1.0", "design": args.design, "status": "running",
              "source": str(source), "source_sha256": sha256(source), "seed": args.seed,
              "case_count": args.cases, "coverage": spec["coverage"],
              "cross_argument_aliasing": False, "reference_ubsan": False,
              "native_ir_equivalence": "pending" if args.ir else "not_requested", "rtl_correct": None,
              "latency_definition": "Clock edges from accepted ap_start && ap_ready to observed ap_done; final write latency is reported separately.",
              "reset_protocol": "Warm second transaction; synchronous reset between each subsequent pair; no reset-abort coverage.",
              "read_during_write": "One-cycle synchronous read-first memory; same-address dual writes rejected.",
              "cases": []}
    write_json(output / "verification.json", result)
    commands = Commands(output, args.timeout_seconds)
    try:
        clangxx = tool_path(args.clangxx)
        result["clang_version"] = commands.run("clang_version", [clangxx, "--version"])
        reference_wrapper = output / "reference.cpp"
        reference_wrapper.write_text(wrapper_source(spec), encoding="utf-8")
        reference_exe = output / "source_reference"
        commands.run("compile_source_reference", [clangxx, "-std=c++17", "-O0", "-g",
                     "-Wno-unknown-pragmas", "-fsanitize=undefined", "-fno-sanitize-recover=all",
                     str(source), str(reference_wrapper), "-o", str(reference_exe)])
        ir_exe = None
        if args.ir:
            ir = args.ir.resolve(strict=True)
            result.update({"ir": str(ir), "ir_sha256": sha256(ir)})
            if ir.suffix == ".ll":
                ir_text = ir.read_text()
            else:
                disassembly = output / "candidate.ll"
                commands.run("disassemble_ir", [tool_path(args.llvm_dis), str(ir), "-o", str(disassembly)])
                ir_text = disassembly.read_text()
            names = re.findall(r'^define\b[^@\n]*@(?:"([^"]+)"|([\w.$-]+))\(', ir_text, re.M)
            symbols = [a or b for a, b in names if (a or b) == "dut" or (a or b).startswith("_Z3dut")]
            if len(symbols) != 1:
                raise RuntimeError(f"Cannot identify unique native dut definition: {symbols}")
            ir_wrapper = output / "ir_reference.cpp"
            ir_wrapper.write_text(wrapper_source(spec, symbols[0]), encoding="utf-8")
            ir_exe = output / "ir_reference"
            commands.run("compile_ir_reference", [clangxx, "-std=c++17", "-O0", "-Wno-override-module",
                         str(ir), str(ir_wrapper), "-o", str(ir_exe)])
        case_payloads = []
        for trial in range(args.cases):
            inputs = make_case(args.design, trial, args.seed)
            case_root = output / f"case_{trial:03d}"
            case_root.mkdir(exist_ok=True)
            input_values = []
            for name, depth in spec["arguments"]:
                if depth:
                    input_values.extend(inputs[name])
                    write_hex(case_root / f"input_{name}.hex", inputs[name])
                else:
                    input_values.append(inputs[name])
            input_binary = case_root / "input.bin"
            input_binary.write_bytes(pack_ints(input_values))
            reference_binary = case_root / "reference.bin"
            commands.run(f"reference_{trial:03d}", [str(reference_exe), str(input_binary), str(reference_binary)])
            raw = reference_binary.read_bytes()
            expected_length = 4 * (1 + sum(depth for _, depth in spec["arguments"]))
            if len(raw) != expected_length:
                raise RuntimeError(f"Unexpected reference output size in case {trial}")
            values = struct.unpack(f"<{len(raw) // 4}i", raw)
            cursor = 1
            for name, depth in spec["arguments"]:
                if depth:
                    expected = list(values[cursor:cursor + depth])
                    if name not in spec["writable"] and expected != inputs[name]:
                        raise RuntimeError(f"Native source modified input-only argument {name}")
                    write_hex(case_root / f"expected_{name}.hex", expected)
                    cursor += depth
            if ir_exe:
                candidate_binary = case_root / "candidate.bin"
                commands.run(f"native_ir_{trial:03d}", [str(ir_exe), str(input_binary), str(candidate_binary)])
                if candidate_binary.read_bytes() != raw:
                    raise RuntimeError(f"Native optimized IR differs from source in case {trial}")
            case_payloads.append({"inputs": inputs, "return": values[0]})
            result["cases"].append({"case": trial, "trial": trial, "input_sha256": sha256(input_binary),
                                    "reference_sha256": sha256(reference_binary), "return": values[0]})
            write_json(output / "verification.json", result)
        result["reference_ubsan"] = True
        if ir_exe:
            result["native_ir_equivalence"] = "passed"
        if not args.reference_only:
            reports = args.reports.resolve(strict=True)
            rtl_path = reports / "rtl/top.v"
            result.update({"reports": str(reports), "rtl_sha256": sha256(rtl_path)})
            bench = output / "testbench.v"
            bench.write_text(testbench_source(spec, read_memories(reports), rtl_path.read_text(),
                                             case_payloads, output, args.max_cycles), encoding="utf-8")
            simulation = output / "rtl_simulation"
            commands.run("compile_rtl", [tool_path(args.iverilog), "-g2012", "-Wall", "-s", "dapoii_tb",
                         "-o", str(simulation), str(rtl_path), str(bench)])
            log = commands.run("simulate_rtl", [tool_path(args.vvp), str(simulation)])
            rows = re.findall(r"^RESULT trial=(\d+) cycles=(\d+) last_write_cycle=(-?\d+) reads=(\d+) writes=(\d+)$", log, re.M)
            if len(rows) != args.cases or "DAPOII RTL VERIFICATION PASSED" not in log:
                raise RuntimeError("Simulation did not report every expected case")
            for row, entry in zip(rows, result["cases"]):
                trial, cycles, last_write, reads, writes = map(int, row)
                if trial != entry["trial"]:
                    raise RuntimeError("Simulation returned unexpected case order")
                entry.update({"cycles": cycles, "last_write_cycle": last_write, "reads": reads, "writes": writes})
            result["rtl_correct"] = True
        result["status"] = "passed"
    except (OSError, RuntimeError, ValueError) as error:
        result.update({"status": "failed", "error": str(error)})
        if result["native_ir_equivalence"] == "pending":
            result["native_ir_equivalence"] = "not_completed"
        print(str(error), file=sys.stderr)
    write_json(output / "verification.json", result)
    print(f"Validation {result['status']}: {output / 'verification.json'}", flush=True)
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
