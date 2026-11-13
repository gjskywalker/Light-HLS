#!/usr/bin/env python3
"""Check all six FP16/FP32/FP64 conversion paths against LLVM APFloat."""
import argparse
import ctypes
import json
from pathlib import Path
import random
import shlex
import struct
import subprocess

from verify_floating import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).resolve().parents[2] / "App/backend_floating/backend_conversion.cpp"
    binary = args.binary.resolve()
    common = ["--board", "u250", "--freq", "100MHz"]
    run([binary, "--prepare-source", source, "backend_conversion", *common,
         "--output", output / "prepared"], output, "prepare")
    manifest = output / "prepared/reports/dapo_manifest.json"
    ir = json.loads(manifest.read_text())["ir"]
    run([binary, ir, "backend_conversion", "--manifest", manifest, *common,
         "--latency-model", "scheduled", "--output", output / "generated"], output, "generate")
    flags = subprocess.check_output(["llvm-config", "--ldflags", "--libs", "support", "--system-libs"], text=True)
    library = output / "oracle.so"
    run(["clang++", "-shared", "-fPIC", "-std=c++17", "-I/usr/local/include",
         Path(__file__).with_name("half_oracle.cpp"), "-o", library, *shlex.split(flags)], output, "oracle")
    oracle = ctypes.CDLL(str(library)).convert_bits
    oracle.argtypes = [ctypes.c_uint64, ctypes.c_uint, ctypes.c_uint]
    oracle.restype = ctypes.c_uint64
    pairs = [(16, 32), (16, 64), (32, 16), (32, 64), (64, 16), (64, 32)]
    vectors = []
    rng = random.Random(1716)
    for op, (iw, ow) in enumerate(pairs):
        fraction = {16: 10, 32: 23, 64: 52}[iw]
        exponent = ((1 << (iw - fraction - 1)) - 1) << fraction
        patterns = [0, 1, (1 << fraction)-1, 1 << fraction, exponent-1, exponent,
                    exponent+1, exponent | (1 << (fraction-1))]
        for number in [0.5, 1.0, 1.00048828125, 1.0004882812500002, 65504.0,
                       65520.0, 2.0**-25, 2.0**-24, 2.0**-14, 2.0**-126, 2.0**-149]:
            try:
                patterns.append(int.from_bytes(struct.pack("<" + {16: "e", 32: "f", 64: "d"}[iw], number), "little"))
            except OverflowError:
                pass
        patterns += [value | (1 << (iw-1)) for value in patterns]
        patterns += [rng.getrandbits(iw) for _ in range(500)]
        for value in patterns:
            vectors.append((op, value, ow, oracle(value, iw, ow)))
    checks = "\n".join(f"check({index}, 64'h{value:x}, {op});"
                       for index, (op, value, _, _) in enumerate(vectors))
    tb = output / "tb.v"
    tb.write_text('''module tb;
reg ap_clk=0, ap_rst=1, ap_start=0;
always #5 ap_clk=~ap_clk;
reg [63:0] input_bits=0;
reg [31:0] operation=0;
wire ap_done, ap_ready, ap_idle;
wire [63:0] ap_return;
backend_conversion dut(.ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
 .ap_done(ap_done), .ap_ready(ap_ready), .ap_idle(ap_idle), .ap_return(ap_return),
 .value_bits(input_bits), .operation(operation));
task check;
input integer index;
input [63:0] value;
input [31:0] op;
integer cycles;
begin
 @(negedge ap_clk); input_bits=value; operation=op; ap_start=1;
 @(posedge ap_clk); #1;
 @(negedge ap_clk); ap_start=0; cycles=0;
 while (!ap_done && cycles<500) begin
  @(posedge ap_clk); #1; cycles=cycles+1;
 end
 if (!ap_done) $fatal(1, "TIMEOUT");
 $display("RESULT %0d %h %0d", index, ap_return, cycles);
end
endtask
initial begin
 repeat(3) @(negedge ap_clk); ap_rst=0;
''' + checks + '\n$finish;\nend\nendmodule\n')
    rtl = output / "generated/reports/rtl/top.v"
    run(["iverilog", "-g2012", "-s", "tb", "-o", output / "sim", tb, rtl], output, "iverilog")
    simulation = run(["vvp", output / "sim"], output, "simulation")
    rows = [line.split() for line in simulation.splitlines() if line.startswith("RESULT ")]
    bounds = json.loads((output / "generated/reports/estimate/dapo_estimate.json").read_text())["scheduled_latency"]["cycles"]
    failures = []
    for _, index, value, cycles in rows:
        op, source_bits, width, expected = vectors[int(index)]
        fraction = {16: 10, 32: 23, 64: 52}[width]
        exponent = ((1 << (width-fraction-1))-1) << fraction
        def nan(bits):
            return bits & exponent == exponent and bits & ((1 << fraction)-1) != 0
        if "x" in value.lower() or "z" in value.lower():
            failures.append([index, "unknown bits", value])
            continue
        actual = int(value, 16)
        if actual != expected and not (nan(actual) and nan(expected)):
            failures.append([index, op, hex(source_bits), hex(actual), hex(expected)])
        if not bounds["minimum"] <= int(cycles) <= bounds["maximum"]:
            failures.append([index, "cycles", cycles])
    if len(rows) != len(vectors):
        failures.append(["incomplete", len(rows), len(vectors)])
    result = dict(cases=len(rows), failures=failures, oracle="LLVM APFloat",
                  directions=pairs, scheduled_cycles=bounds,
                  measured_cycles=sorted({int(row[3]) for row in rows}))
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(len(rows), "conversion vectors;", len(failures), "failures")
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
