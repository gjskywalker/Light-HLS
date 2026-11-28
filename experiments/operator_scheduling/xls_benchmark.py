"""Run native XLS pipelines on the five collected, public ISDC counterparts."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import random
import re
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BUNDLE = ROOT / "benchmarks/hls_scheduling_2024_2026/sources/xls_public_counterparts"
# Periods in ps from DATE 2024, Table I. Widths follow the checked-in DSLX top interfaces.
CASES = {
    "crc32": ("xls/examples/crc32/crc32.x", "main", 2500, [8], 32),
    "hsv2rgb": ("third_party/xls_colors/hsv2rgb.x", "hsv2rgb", 5000, [16, 8, 8], 24),
    "float32_fast_rsqrt": ("xls/dslx/stdlib/float32.x", "fast_rsqrt", 5000, [32], 32),
    "sha256": ("xls/examples/sha256.x", "sha256", 2500, [512], 256),
    "fpexp_32": ("third_party/xls_go_math/fpexp_32.x", "fpexp_32", 5000, [32], 32),
}
TAG = "v0.0.0-11174-g32dc47058"
TARBALL_SHA256 = "d62488b1c8270ac626aab13a03dfddb238458270f64164b8bf3ccb5d782d0fe7"
STRATEGIES = ("sdc", "asap")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, folder, label, *, output=None):
    command = [str(a) for a in command]
    stdout = output if output else folder / f"{label}.stdout"
    start = time.monotonic()
    with stdout.open("wb") as f, (folder / f"{label}.stderr").open("wb") as e:
        try:
            result = subprocess.run(command, cwd=folder, stdout=f, stderr=e, timeout=300)
            status = result.returncode
        except subprocess.TimeoutExpired:
            status = "timeout"
    with (folder / "commands.jsonl").open("a") as log:
        log.write(json.dumps({"label": label, "args": command, "status": status,
                              "wall_seconds": time.monotonic() - start}) + "\n")
    if status != 0:
        detail = (folder / f"{label}.stderr").read_text(errors="replace")[-800:]
        raise RuntimeError(f"{label}: {status}, {detail}")


def bit_width(type_spec):
    """Count live register storage in XLS block IR, including unpacked arrays and tuples."""
    type_spec = type_spec.strip()
    bit = re.fullmatch(r"bits\[(\d+)\]", type_spec)
    if bit:
        return int(bit.group(1))
    if type_spec == "token":
        return 0
    if type_spec.endswith("]"):
        array_type, count = type_spec.rsplit("[", 1)
        return int(count[:-1]) * bit_width(array_type)
    if type_spec.startswith("(") and type_spec.endswith(")"):
        return sum(bit_width(field) for field in split_fields(type_spec[1:-1]) if field.strip())
    raise ValueError(f"unrecognized register type {type_spec}")


def split_fields(s):
    fields, depth, start = [], 0, 0
    for i, char in enumerate(s):
        if char == "," and depth == 0:
            fields.append(s[start:i])
            start = i + 1
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
    return fields + [s[start:]]


def testbench(vectors, input_names, widths, output_width, latency):
    ports = [f"  reg [{w-1}:0] {n};" for n, w in zip(input_names, widths)]
    lines = ["module tb;", "  reg clk = 0; always #5 clk = ~clk;", *ports,
             f"  wire [{output_width-1}:0] out;", "  dut impl (.*);", "  initial begin"]
    for i in range(len(vectors) + latency - 1):
        vector = vectors[i] if i < len(vectors) else [0] * len(input_names)
        lines.append("    @(negedge clk);")
        for n, w, v in zip(input_names, widths, vector):
            lines.append(f"    {n} = {w}'h{v:x};")
        lines.append("    @(posedge clk); #1;")
        if i >= latency-1:
            lines.append(f'    $display("OBS {i-latency+1} %h", out);')
    return "\n".join(lines + ["    $finish;", "  end", "endmodule", ""])


def check_rtl(name, source, top, tool, flags, folder, widths, output_width, vectors, signature):
    # Each vector enters on a consecutive cycle; the interpreter checks the matching result
    # by evaluating the original DSLX function on that exact input.
    ports = re.findall(r'data_ports \{\s*direction: PORT_DIRECTION_INPUT\s*name: "(\w+)"\s*width: (\d+)', signature)
    if [int(w) for _, w in ports] != widths:
        raise ValueError(f"unexpected ports {ports}, expected {widths}")
    latency = int(re.search(r'pipeline \{\s*latency: (\d+)', signature)[1])
    if latency < 1:
        raise ValueError("need a registered pipeline")
    (folder / "tb.v").write_text(testbench(vectors, [n for n, _ in ports], widths, output_width, latency))
    run(["iverilog", "-g2012", "-s", "tb", "-o", "test.vvp", "rtl.v", "tb.v"], folder, "elaboration")
    run(["vvp", "test.vvp"], folder, "simulation")
    outputs = re.findall(r"^OBS (\d+) ([0-9a-fA-F]+)$", (folder / "simulation.stdout").read_text(), re.M)
    if [int(i) for i, _ in outputs] != list(range(len(vectors))):
        raise ValueError("missing output vector or unknown X/Z bits")
    assertions = []
    for vector, (_, hexword) in zip(vectors, outputs):
        args = [f"bits[{w}]:0x{v:x}" for w, v in zip(widths, vector)]
        if name in ("fpexp_32", "float32_fast_rsqrt"):
            prefix = "float32::" if name == "fpexp_32" else ""
            args = [f"{prefix}unflatten({args[0]})"]
        expression = f"{top}({', '.join(args)})"
        if name in ("fpexp_32", "float32_fast_rsqrt"):
            expression = f"{prefix}flatten({expression})"
        if name in ("sha256", "hsv2rgb"):
            count = 8 if name == "sha256" else 3
            fields = [f"v{i}" for i in range(count)]
            expression = "{ let (" + ", ".join(fields) + f") = {expression}; " + " ++ ".join(fields) + " }"
        assertions.append(f"    assert_eq({expression}, bits[{output_width}]:0x{hexword});")
    augmented = folder / source.name
    augmented.write_text(source.read_text() + "\n#[test]\nfn generated_rtl_crosscheck() {\n"
                         + "\n".join(assertions) + "\n}\n")
    run([tool / "interpreter_main", *flags, "--test_filter=generated_rtl_crosscheck", augmented],
        folder, "dslx_rtl_check")
    if not re.search(r"\[ RUN UNITTEST\s*\] generated_rtl_crosscheck", (folder/"dslx_rtl_check.stderr").read_text()):
        raise ValueError("crosscheck test was not executed")
    return {"tested": len(vectors), "signature_latency": latency,
            "rtl_outputs_sha256": digest(folder / "simulation.stdout")}


def case(name, tool, output):
    relative, top, clock, widths, output_width = CASES[name]
    source = BUNDLE / relative
    folder = output / name
    folder.mkdir()
    flags = [f"--dslx_stdlib_path={BUNDLE}/xls/dslx/stdlib", f"--dslx_path={BUNDLE}"]
    row = {"source": relative, "sha256": digest(source), "top": top, "clock_ps": clock, "strategies": {}}
    try:
        run([tool / "interpreter_main", *flags, "--seed=20261125", source], folder, "upstream_tests")
        row["upstream_tests"] = (folder / "upstream_tests.stderr").read_text().splitlines()[-5:]
        run([tool / "ir_converter_main", *flags, "--top=" + top, source], folder, "dslx_ir", output=folder / "raw.ir")
        run([tool / "opt_main", "--delay_model=sky130", folder / "raw.ir"], folder, "opt", output=folder / "opt.ir")
        row["optimized_ir_sha256"] = digest(folder / "opt.ir")
    except (ValueError, RuntimeError) as exc:
        row["error"] = str(exc)
        return name, row
    rng = random.Random(20261125)
    vectors = [[0]*len(widths), [(1 << w)-1 for w in widths]]
    vectors += [[rng.getrandbits(w) for w in widths] for _ in range(14)]
    if name in ("fpexp_32", "float32_fast_rsqrt"):
        vectors[2:7] = [[x] for x in (0x3F800000, 0xBF800000, 0x7F800000, 0x00000001, 0x80000000)]
    for strategy in STRATEGIES:
        directory = folder / strategy
        directory.mkdir()
        values = {}
        try:
            run([tool / "codegen_main", "--generator=pipeline", "--delay_model=sky130",
                 f"--clock_period_ps={clock}", f"--scheduling_strategy={strategy}", "--module_name=dut",
                 "--use_system_verilog=false", "--flop_inputs=true", "--flop_outputs=true",
                 "--output_verilog_path=rtl.v", "--output_schedule_path=schedule.txt",
                 "--output_signature_path=signature.txt", "--output_block_ir_path=block.ir", folder / "opt.ir"],
                directory, "codegen")
            schedule = (directory / "schedule.txt").read_text()
            lengths = re.findall(r"^    length: (\d+)$", schedule, re.M)
            if len(lengths) != 1:
                raise ValueError(f"expected one function schedule: {lengths}")
            regs = re.findall(r"^  reg [^(]+\((.+)\)$", (directory / "block.ir").read_text(), re.M)
            values.update({"schedule_stages": int(lengths[0]), "block_register_bits": sum(
                bit_width(split_fields(reg)[0]) for reg in regs), "registers": len(regs),
                "rtl_sha256": digest(directory / "rtl.v")})
            values.update(check_rtl(name, source, top, tool, flags, directory, widths, output_width, vectors,
                                    (directory / "signature.txt").read_text()))
            values["status"] = "passed"
        except (ValueError, RuntimeError) as error:
            values.update(status="failed", error=str(error))
        row["strategies"][strategy] = values
        print(name, strategy, values, flush=True)
    return name, row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tool, output = args.tools.resolve(), args.output.resolve()
    output.mkdir(parents=True)
    state = {"release_tag": TAG, "archive_sha256": TARBALL_SHA256, "tools_sha256": {
                 n: digest(tool / n) for n in ("interpreter_main", "ir_converter_main", "opt_main", "codegen_main")},
             "source_commit": "f0ad0e58eafcb062cbb08b4e7751c7442ac8346c", "delay_model": "sky130",
             "source_equivalence": "broader_suite", "input_seed": 20261125, "cases": {}}
    with ThreadPoolExecutor(max_workers=3) as pool:
        futures = [pool.submit(case, n, tool, output) for n in CASES]
        for fut in futures:
            name, row = fut.result()
            state["cases"][name] = row
            (output / "results.json").write_text(json.dumps(state, indent=2) + "\n")


if __name__ == "__main__":
    main()
