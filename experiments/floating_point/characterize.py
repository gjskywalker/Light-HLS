#!/usr/bin/env python3
"""Characterize isolated, actual Light-HLS floating functional-unit RTL.

Each unit is extracted from the generated scalar kernel, with constant inputs
made ports to avoid constant folding. Controller, mux and context-register costs
are excluded. This is an additive operator model, not whole-kernel synthesis.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
IMPLEMENTATION = "hardfloat_release1_scalar_ce_v2"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(command, directory, name, timeout=600):
    started = time.time()
    command = list(map(str, command))
    with (directory / (name + ".log")).open("w") as log:
        result = subprocess.run(command, cwd=directory, stdout=log,
                                stderr=subprocess.STDOUT, timeout=timeout)
    receipt = dict(command=command, exit_code=result.returncode,
                   elapsed_seconds=time.time()-started)
    (directory / (name + ".command.json")).write_text(json.dumps(receipt, indent=2))
    if result.returncode:
        raise RuntimeError(name + " failed; see " + str(directory))


def key(instance):
    return "|".join([instance["opcode"], str(instance["bit_width"]),
                     ",".join(map(str, instance["operand_widths"]))])


def report_resources(path):
    text = Path(path).read_text()
    result = {}
    for name, label in [("lut", r"CLB LUTs\*?"), ("ff", "Register as Flip Flop"), ("dsp", "DSPs")]:
        match = re.search(r"\|\s*" + label + r"\s*\|\s*([\d,]+)\s*\|", text)
        if not match: raise RuntimeError("Missing utilization row: " + name)
        result[name] = int(match.group(1).replace(",", ""))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_DAPO")
    parser.add_argument("--vivado", type=Path, default=Path("/vitis/2025.2/Vivado/bin/vivado"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--formats", type=int, nargs="+", default=[16, 32, 64])
    parser.add_argument("--operations", nargs="+", default=["fadd", "fsub", "fmul", "fdiv", "sqrt", "fma", "fabs", "copysign"])
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=args.resume)
    binary = args.binary.resolve()
    model = dict(schema_version=1, implementation=IMPLEMENTATION, board="u250",
                 part="xcu250-figd2104-2L-e", clock_period_ns=10,
                 scope="isolated_floating_operators_only", status="running",
                 binary_sha256=digest(binary), entries={}, failures=[],
                 rtl_emitter_sha256=digest(ROOT / "Light_HLS/Implementations/HI_RTLGeneration/HI_FloatingPointRTL.cc"),
                 note="Measured OOC LUT/FF/DSP; excludes input muxes, context storage, controller, memory, cross-unit synthesis optimization, and stall-enable overhead.")
    if args.resume:
        previous = json.loads((output / "model.json").read_text())
        if previous["binary_sha256"] != model["binary_sha256"]:
            raise RuntimeError("Resume binary mismatch")
        model = previous
        model["status"] = "running"
    for width in args.formats:
        kind = {16: "half", 32: "float", 64: "double"}[width]
        ctype = {16: "_Float16", 32: "float", 64: "double"}[width]
        prepared = output / ("prepared" + str(width))
        prepared.mkdir(exist_ok=args.resume)
        source = prepared / "source.cpp"
        source.write_text(f'extern "C" {ctype} dut({ctype} a, {ctype} b, {ctype} c) {{ return a; }}\n')
        run([binary, "--prepare-source", source, "dut", "--board", "u250", "--freq", "100MHz", "--output", prepared], prepared, "prepare")
        manifest = prepared / "reports/dapo_manifest.json"
        for operation in args.operations:
            conversion = re.fullmatch(r"(fpext|fptrunc)(16|32|64)", operation)
            result_width = int(conversion.group(2)) if conversion else width
            result_kind = {16: "half", 32: "float", 64: "double"}[result_width]
            if conversion and ((conversion.group(1) == "fpext") != (result_width > width) or result_width == width):
                continue
            directory = output / (operation + str(width))
            if args.resume and any(Path(entry["report"]).parent == directory for entry in model["entries"].values()):
                continue
            directory.mkdir(exist_ok=args.resume)
            try:
                arguments = 3 if operation == "fma" else 1 if operation in ("sqrt", "fabs") or conversion else 2
                params = ", ".join(f"{kind} %{name}" for name in "abc"[:arguments])
                if conversion:
                    declaration = ""
                    expression = f"{conversion.group(1)} {kind} %a to {result_kind}"
                elif operation in ("fadd", "fsub", "fmul", "fdiv"):
                    declaration = ""
                    expression = f"{operation} {kind} %a, %b"
                else:
                    function = f"llvm.{operation}.f{width}"
                    declaration = f"declare {kind} @{function}({', '.join([kind]*arguments)})\n"
                    expression = f"call {kind} @{function}({params})"
                ir = directory / "unit.ll"
                ir.write_text(declaration + f"define {result_kind} @dut({kind} %a, {kind} %b, {kind} %c) {{\nentry:\n %v = {expression}\n ret {result_kind} %v\n}}\n")
                run([binary, ir, "dut", "--manifest", manifest, "--board", "u250", "--freq", "100MHz", "--latency-model", "scheduled", "--output", directory / "generated"], directory, "generate")
                reports = directory / "generated/reports"
                binding = json.loads((reports / "binding/binding.json").read_text())
                instances = binding["operator_instances"]
                if len(instances) != 1:
                    raise RuntimeError("Characterization requires exactly one bound operator")
                instance = instances[0]
                name = instance["name"]
                rtl = (reports / "rtl/top.v").read_text()
                begin = re.search(r"^  wire .*\b" + re.escape(name) + r"_in0\s*=", rtl, re.M).start()
                # FU declarations end at the blank line before controller logic.
                end = rtl.index("\n\n", begin)
                body = rtl[begin:end]
                for index in range(arguments):
                    body, count = re.subn(r"(" + re.escape(name) + f"_in{index} = )[^;]+;", rf"\g<1>in{index};", body)
                    if count != 1: raise RuntimeError("Unexpected FU input declaration")
                body = re.sub(r"\(state == \d+\)", "start", body)
                library = rtl[rtl.index("`ifndef LIGHT_HLS_HARDFLOAT_LIBRARY"):]
                ports = ["input ap_clk", "input ap_rst", "input start"]
                ports += [f"input [{w-1}:0] in{i}" for i, w in enumerate(instance["operand_widths"])]
                ports += [f"output [{result_width-1}:0] result"]
                unit = directory / "unit.v"
                unit.write_text("module fp_unit(" + ",".join(ports) + ");\n" + body +
                                f"\nassign result = {name}_out;\nendmodule\n" + library)
                tcl = directory / "synth.tcl"
                (directory / "constraints.xdc").write_text('''create_clock -name ap_clk -period 10 [get_ports ap_clk]
set_input_delay 2 -clock ap_clk [get_ports -filter {DIRECTION == IN && NAME != ap_clk}]
set_output_delay 2 -clock ap_clk [all_outputs]
''')
                tcl.write_text('''set_param general.maxThreads 1
read_verilog unit.v
read_xdc constraints.xdc
synth_design -top fp_unit -part xcu250-figd2104-2L-e -mode out_of_context -flatten_hierarchy rebuilt
report_utilization -file utilization.rpt
report_timing_summary -file timing.rpt
set fd [open counts.txt w]
foreach {name filter} {lut {REF_NAME =~ LUT*} ff {REF_NAME =~ FD*} dsp {REF_NAME =~ DSP*}} {
 puts $fd "$name [llength [get_cells -hier -filter $filter]]"
}
close $fd
exit
''')
                run([args.vivado, "-mode", "batch", "-source", tcl, "-notrace"], directory, "vivado")
                # Raw get_cells includes DSP decomposition cells and unpacked
                # LUT primitives. Use physical utilization rows, not that sum.
                counts = report_resources(directory / "utilization.rpt")
                model["entries"][key(instance)] = dict(resources=counts, rtl_sha256=digest(unit),
                    report=str(directory / "utilization.rpt"), operand_widths=instance["operand_widths"],
                    opcode=instance["opcode"], result_width=result_width)
                print(operation, width, counts, flush=True)
            except Exception as error:
                model["failures"].append(dict(operation=operation, width=width, error=str(error)))
                print("FAILED", operation, width, str(error), flush=True)
            (output / "model.json").write_text(json.dumps(model, indent=2) + "\n")
    model["status"] = "completed" if not model["failures"] else "partial"
    if digest(binary) != model["binary_sha256"]:
        model["status"] = "invalid_binary_changed"
    (output / "model.json").write_text(json.dumps(model, indent=2) + "\n")


if __name__ == "__main__":
    main()
