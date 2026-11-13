#!/usr/bin/env python3
"""Bit-pattern floating-point RTL regression against a native C++ oracle."""
import argparse
import ctypes
import json
from pathlib import Path
import random
import shlex
import struct
import subprocess


def run(command, directory, name):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=180)
    (directory / (name + ".log")).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"{name} failed: {result.returncode}; inspect {directory}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--random-cases", type=int, default=100)
    parser.add_argument("--formats", type=int, nargs="+", choices=(16, 32, 64), default=[16, 32, 64])
    parser.add_argument("--native-half", action="store_true",
                        help="Emit native half IR for hardware testing; never execute AVX512 host code")
    parser.add_argument("--classic-binary", type=Path,
                        help="Validate the production source-driver RTL instead of DAPO RTL")
    args = parser.parse_args()
    if args.native_half and args.classic_binary:
        parser.error("Native half IR testing is separate from the production source driver")
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    results = []
    for width, kind, top in [(16, "_Float16", "backend_half"), (32, "float", "backend_floating"), (64, "double", "backend_double")]:
        if width not in args.formats:
            continue
        directory = output / kind
        directory.mkdir()
        source = root / "App/backend_floating" / (top + ".cpp")
        prepared, generated = directory / "prepared", directory / "generated"
        common = ["--board", "u250", "--freq", "100MHz"]
        run([args.binary.resolve(), "--prepare-source", source, top, *common, "--output", prepared], directory, "prepare")
        manifest = prepared / "reports/dapo_manifest.json"
        ir = json.loads(manifest.read_text())["ir"]
        if width == 16 and args.native_half:
            ir = directory / "native_half.ll"
            run(["/usr/local/bin/clang++", "-O0", "-Xclang", "-disable-O0-optnone",
                 "-mavx512fp16", "-S", "-emit-llvm", source, "-o", ir], directory, "native_half_ir")
            if "fdiv half" not in ir.read_text():
                raise RuntimeError("Native half test did not produce half arithmetic")
        run([args.binary.resolve(), ir, top, "--manifest", manifest, *common,
             "--latency-model", "scheduled", "--output", generated], directory, "generate")
        estimate = json.loads((generated / "reports/estimate/dapo_estimate.json").read_text())
        bounds = estimate["scheduled_latency"]["cycles"]
        if args.classic_binary:
            generated = directory / "classic"
            run([args.classic_binary.resolve(), source, top, *common, "--output", generated], directory, "classic_generate")
            bounds = None
        oracle = directory / "oracle.cpp"
        integer = {16: "uint16_t", 32: "uint32_t", 64: "uint64_t"}[width]
        oracle.write_text(f'''#include <cstdint>
#include <cstring>
#include "{source}"
extern "C" uint64_t evaluate(uint64_t a, uint64_t b, int op) {{
    {integer} ai = a, bi = b, oi;
    {kind} af, bf, result;
    std::memcpy(&af, &ai, sizeof(af));
    std::memcpy(&bf, &bi, sizeof(bf));
    result = {top}(af, bf, op);
    std::memcpy(&oi, &result, sizeof(oi));
    return oi;
}}
''')
        library = directory / "oracle.so"
        if width == 16:
            flags = subprocess.check_output(["/usr/local/bin/llvm-config", "--ldflags", "--libs", "support", "--system-libs"], text=True)
            run(["/usr/local/bin/clang++", "-shared", "-fPIC", "-std=c++17", "-I/usr/local/include",
                 Path(__file__).with_name("half_oracle.cpp"), "-o", library, *shlex.split(flags)], directory, "oracle")
            half_function = ctypes.CDLL(str(library)).evaluate_half
            half_function.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_int, ctypes.c_int]
            half_function.restype = ctypes.c_uint64
            function = lambda a, b, op: half_function(a, b, op, int(args.native_half))
        else:
            run(["/usr/local/bin/clang++", "-shared", "-fPIC", "-O0", "-ffp-contract=off", oracle, "-lm", "-o", library], directory, "oracle")
            function = ctypes.CDLL(str(library)).evaluate
            function.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_int]
            function.restype = ctypes.c_uint64
        fmt = {16: "e", 32: "f", 64: "d"}[width]
        def bits(value):
            return int.from_bytes(struct.pack("<" + fmt, value), "little")
        fraction = {16: 10, 32: 23, 64: 52}[width]
        exp_mask = ((1 << (width - fraction - 1)) - 1) << fraction
        def nan(value):
            return value & exp_mask == exp_mask and value & ((1 << fraction)-1) != 0
        edges = [0, 1, (1 << fraction)-1, 1 << fraction, bits(1.0), bits(-1.0),
                 bits(0.5), bits(3.0), exp_mask-1, exp_mask, exp_mask | (1 << (fraction-1)),
                 1 << (width-1), (1 << (width-1)) | exp_mask]
        rng = random.Random(117 + width)
        pairs = [(a, b) for a in edges for b in edges]
        pairs += [(rng.getrandbits(width), rng.getrandbits(width)) for _ in range(args.random_cases)]
        operations = [0, 1, 2, 3, 4, 5, 8, 9] if width in (16, 64) else [0, 1, 2, 3, 4, 5, 9]
        if width == 16:
            operations.append(10)
        vectors = [(a, b, op) for a, b in pairs for op in operations]
        vectors += [(bits(a), bits(0), op) for a in [-1023.75, -1.5, -0.0, 0.25, 1.75, 32752.0]
                    for op in [6, 7] if op != 7 or a >= 0]
        statements = []
        expected = []
        for index, (a, b, op) in enumerate(vectors):
            expected.append(function(a, b, op))
            statements.append(f"check({index}, {width}'h{a:x}, {width}'h{b:x}, 32'd{op});")
        tb = directory / "tb.v"
        tb.write_text(f'''module tb;
reg ap_clk=0, ap_rst=1, ap_start=0;
always #5 ap_clk=~ap_clk;
reg [{width-1}:0] a=0, b=0;
reg [31:0] operation=0;
wire ap_done, ap_idle, ap_ready;
wire [{width-1}:0] ap_return;
{top} dut(.ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .a(a), .b(b), .operation(operation), .ap_return(ap_return));
task check;
input integer index;
input [{width-1}:0] av, bv;
input [31:0] op;
integer cycles;
begin
    @(negedge ap_clk); a=av; b=bv; operation=op; ap_start=1;
    @(posedge ap_clk); #1;
    @(negedge ap_clk); ap_start=0;
    cycles=0;
    while (!ap_done && cycles < 500) begin
        @(posedge ap_clk); #1; cycles=cycles+1;
    end
    if (!ap_done) $fatal(1, "TIMEOUT %0d", index);
    $display("RESULT %0d %h %0d", index, ap_return, cycles);
end
endtask
initial begin
    repeat(3) @(negedge ap_clk); ap_rst=0;
    {chr(10).join(statements)}
    $finish;
end
endmodule
''')
        rtl = generated / "reports/rtl/top.v"
        run(["iverilog", "-g2012", "-s", "tb", "-o", directory / "sim", tb, rtl], directory, "iverilog")
        simulation = run(["vvp", directory / "sim"], directory, "simulation")
        rows = [line.split() for line in simulation.splitlines() if line.startswith("RESULT ")]
        if len(rows) != len(vectors):
            raise RuntimeError("Incomplete RTL vector output")
        failures = []
        for _, index, actual, cycles in rows:
            wanted = expected[int(index)]
            if "x" in actual.lower() or "z" in actual.lower():
                failures.append([int(index), "unknown RTL bits", actual])
                continue
            actual = int(actual, 16)
            if actual != wanted and not (nan(actual) and nan(wanted)):
                failures.append([int(index), hex(actual), hex(wanted), vectors[int(index)]])
            if bounds and not bounds["minimum"] <= int(cycles) <= bounds["maximum"]:
                failures.append([int(index), "cycle envelope", int(cycles), bounds])
        result = dict(format=kind, cases=len(rows), failures=failures, scheduled_cycles=bounds,
                      native_half=args.native_half and width == 16,
                      oracle="LLVM APFloat" if width == 16 else "native C++",
                      driver="source" if args.classic_binary else "ir",
                      measured_cycles=sorted({int(row[3]) for row in rows}))
        results.append(result)
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(kind, len(rows), "vectors;", len(failures), "failures", flush=True)
    if any(result["failures"] for result in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
