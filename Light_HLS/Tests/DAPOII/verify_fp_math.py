#!/usr/bin/env python3
"""Validate native sqrt/fabs/copysign IR with independent libm references."""
import argparse
import ctypes
import json
import math
from pathlib import Path
import random
import struct
from verify_floating import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--random-cases", type=int, default=1000)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    binary = args.binary.resolve()
    results = []
    libm = ctypes.CDLL("libm.so.6")
    libm.sqrtf.argtypes = [ctypes.c_float]
    libm.sqrtf.restype = ctypes.c_float
    libm.sqrt.argtypes = [ctypes.c_double]
    libm.sqrt.restype = ctypes.c_double
    for width, kind, ctype, fmt in [(16, "half", "_Float16", "e"), (32, "float", "float", "f"), (64, "double", "double", "d")]:
        directory = output / str(width)
        directory.mkdir()
        source = directory / "source.cpp"
        source.write_text(f'extern "C" {ctype} dut({ctype} a, {ctype} b, int op) {{ return a; }}\n')
        run([binary, "--prepare-source", source, "dut", "--board", "u250", "--freq", "100MHz", "--output", directory / "prepared"], directory, "prepare")
        ir = directory / "math.ll"
        ir.write_text(f'''declare {kind} @llvm.sqrt.f{width}({kind})
declare {kind} @llvm.fabs.f{width}({kind})
declare {kind} @llvm.copysign.f{width}({kind}, {kind})
define {kind} @dut({kind} %a, {kind} %b, i32 %op) {{
entry:
 switch i32 %op, label %copy [i32 0, label %sqrt i32 1, label %abs]
sqrt:
 %s = call {kind} @llvm.sqrt.f{width}({kind} %a)
 ret {kind} %s
abs:
 %f = call {kind} @llvm.fabs.f{width}({kind} %a)
 ret {kind} %f
copy:
 %c = call {kind} @llvm.copysign.f{width}({kind} %a, {kind} %b)
 ret {kind} %c
}}
''')
        run([binary, ir, "dut", "--manifest", directory / "prepared/reports/dapo_manifest.json", "--board", "u250", "--freq", "100MHz", "--latency-model", "scheduled", "--output", directory / "generated"], directory, "generate")
        fraction = {16: 10, 32: 23, 64: 52}[width]
        exponent = ((1 << (width-fraction-1))-1) << fraction
        sign = 1 << (width-1)
        edges = [0, 1, (1 << fraction)-1, 1 << fraction, exponent-1, exponent, exponent+1, exponent+(1 << (fraction-1))]
        edges += [int.from_bytes(struct.pack("<"+fmt, v), "little") for v in (0.25, 1, 2, 3, 4, 9)]
        edges += [x | sign for x in edges]
        rng = random.Random(456+width)
        pairs = [(a, b) for a in edges for b in edges]
        pairs += [(rng.getrandbits(width), rng.getrandbits(width)) for _ in range(args.random_cases)]
        expected = []
        checks = []
        for a, b in pairs:
            for op in range(3):
                if op == 1: wanted = a & ~sign
                elif op == 2: wanted = (a & ~sign) | (b & sign)
                else:
                    value = struct.unpack("<"+fmt, a.to_bytes(width//8, "little"))[0]
                    # binary16 inputs are exactly representable in double.
                    result = libm.sqrtf(value) if width == 32 else libm.sqrt(value)
                    wanted = int.from_bytes(struct.pack("<"+fmt, result), "little")
                checks.append(f"check({len(expected)}, {width}'h{a:x}, {width}'h{b:x}, {op});")
                expected.append(wanted)
        tb = directory / "tb.v"
        tb.write_text(f'''module tb;
reg ap_clk=0, ap_rst=1, ap_start=0;
always #5 ap_clk=~ap_clk;
reg [{width-1}:0] a=0, b=0;
reg [31:0] op=0;
wire ap_done, ap_idle, ap_ready;
wire [{width-1}:0] ap_return;
dut dut_i(.ap_clk(ap_clk),.ap_rst(ap_rst),.ap_start(ap_start),.ap_done(ap_done),
 .ap_idle(ap_idle),.ap_ready(ap_ready),.ap_return(ap_return),.a(a),.b(b),.op(op));
task check;
input integer id;
input [{width-1}:0] av,bv;
input [31:0] operation;
integer cycles;
begin
 @(negedge ap_clk); a=av; b=bv; op=operation; ap_start=1;
 @(posedge ap_clk); #1;
 @(negedge ap_clk); ap_start=0; cycles=0;
 while (!ap_done && cycles<200) begin @(posedge ap_clk); #1; cycles=cycles+1; end
 if (!ap_done) $fatal(1,"TIMEOUT");
 $display("RESULT %0d %h %0d",id,ap_return,cycles);
end
endtask
initial begin
 repeat(3) @(negedge ap_clk); ap_rst=0;
{chr(10).join(checks)}
 $finish;
end
endmodule
''')
        run(["iverilog", "-g2012", "-s", "tb", "-o", directory / "sim", tb, directory / "generated/reports/rtl/top.v"], directory, "iverilog")
        simulation = run(["vvp", directory / "sim"], directory, "simulation")
        rows = [row.split() for row in simulation.splitlines() if row.startswith("RESULT ")]
        bounds = json.loads((directory / "generated/reports/estimate/dapo_estimate.json").read_text())["scheduled_latency"]["cycles"]
        failures = []
        def nan(bits): return bits & exponent == exponent and bits & ((1 << fraction)-1) != 0
        for _, index, text, cycles in rows:
            wanted = expected[int(index)]
            if "x" in text.lower() or "z" in text.lower():
                failures.append([index, text, "unknown"])
                continue
            actual = int(text, 16)
            if actual != wanted and not (nan(actual) and nan(wanted)):
                failures.append([index, hex(actual), hex(wanted)])
            if not bounds["minimum"] <= int(cycles) <= bounds["maximum"]:
                failures.append([index, "cycles", cycles])
        if len(rows) != len(expected): failures.append(["incomplete"])
        results.append(dict(width=width, cases=len(rows), failures=failures, bounds=bounds))
        (output / "results.json").write_text(json.dumps(results, indent=2)+"\n")
        print(width, len(rows), len(failures), flush=True)
    if any(row["failures"] for row in results): raise SystemExit(1)


if __name__ == "__main__": main()
