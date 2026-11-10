#!/usr/bin/env python3
"""Check unchanged kernels in a shared, physically routed on-chip context."""
import argparse
import json
import os
from pathlib import Path
import re

from run_u250_fair_comparison import (
    affinity_prefix, executable_path, parse_route_metrics, run_command,
    run_simulation, sha256_file, tcl_brace, write_common_vivado_tcl,
)

FLOW_ID = "vivado_registered_pressure_context_v1"
SCOPE = ("Closed on-chip pressure harness with a routed BUFG, registered scalar source, "
         "one-cycle RAM response, and retained output observers; not board IO signoff")


def resource_counts(text):
    patterns = {"lut": r"(?:Slice LUTs|CLB LUTs)\*?", "ff": r"(?:Slice Registers|CLB Registers)",
                "dsp": "DSPs", "bram_tiles": "Block RAM Tile"}
    result = {}
    for key, label in patterns.items():
        match = re.search(r"\|\s*" + label + r"\s*\|\s*([0-9.]+)", text)
        if not match:
            raise ValueError(f"Missing resource count: {key}")
        result[key] = float(match[1]) if key == "bram_tiles" else int(match[1])
    return result


def write_shell(path, dataflow):
    template = Path(__file__).with_name("registered_pressure_shell.v").read_text()
    declarations = "wire input_r_we0; wire [31:0] input_r_d0; wire [31:0] output_r_q0 = 0;"
    connections = (", .input_r_we0(input_r_we0), .input_r_d0(input_r_d0), "
                   ".output_r_q0(output_r_q0)")
    path.write_text(template.replace("// EXTRA_DECLARATIONS", declarations if dataflow else "")
                   .replace("// EXTRA_CONNECTIONS", connections if dataflow else ""))


def write_flow(directory, rtl, period):
    script = directory / "run.tcl"
    write_common_vivado_tcl(script, rtl, "registered_pressure_shell", period, directory, True)
    # There are no virtual data IO ports in this *different* physical context.
    # Do not replace or reinterpret the failed virtual-IO flow's constraints.
    (directory / "constraints.xdc").write_text(
        f"create_clock -name ap_clk -period {period:.12g} [get_ports ap_clk]\n")
    content = script.read_text()
    content = content.replace('io_budget_fraction=0.2', 'io_budget_fraction=0')
    check = ('if {[llength [get_ports]] != 1 || [get_property NAME [get_ports]] ne "ap_clk"} '
             '{ error "Unexpected unconstrained external interface" }\n'
             'if {[llength [get_cells -hier -filter {REF_NAME =~ BUFG*}]] != 1} '
             '{ error "A physical global clock buffer is required" }\n')
    content = content.replace("\nopt_design\n", "\n" + check + "opt_design\n", 1)
    content = content.replace("exit\n", "\n".join([
        f"report_utilization -cells [get_cells kernel] -file {tcl_brace(directory / 'kernel_utilization.rpt')}",
        f"report_utilization -hierarchical -file {tcl_brace(directory / 'hierarchical_utilization.rpt')}",
        f"report_clock_utilization -file {tcl_brace(directory / 'clock_utilization.rpt')}",
        f"report_timing -delay_type min -max_paths 10 -file {tcl_brace(directory / 'hold_paths.rpt')}",
        f"report_timing -delay_type max -max_paths 10 -file {tcl_brace(directory / 'setup_paths.rpt')}",
        "exit", ""]))
    script.write_text(content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--comparison", required=True, type=Path)
    parser.add_argument("--light-hls-top", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--simulate-only", action="store_true")
    args = parser.parse_args()
    comparison = args.comparison.resolve(strict=True)
    baseline = json.loads(comparison.read_text())
    source = Path(baseline["source"]).resolve(strict=True)
    fixture = Path(__file__).resolve().parents[1] / "QoRPressure"
    if (source.name not in ("dsp_map.cpp", "dsp_dataflow.cpp", "bram_banked.cpp") or
            sha256_file(source) != baseline["source_sha256"] or
            sha256_file(source) != sha256_file(fixture / source.name)):
        raise ValueError("Only unchanged pressure fixtures are supported")
    if baseline["top_function"] != "qor_pressure" or baseline["part"] != "xcu250-figd2104-2L-e":
        raise ValueError("Unexpected kernel or device")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("Refusing to overwrite a measurement")
    compiler = args.light_hls_top.resolve(strict=True)
    period = float(baseline["clock_period_ns"])
    result = {"valid": False, "physical_valid": False, "flow_id": FLOW_ID, "scope": SCOPE,
              "baseline_comparison_sha256": sha256_file(comparison), "source_sha256": sha256_file(source),
              "compiler_sha256": sha256_file(compiler), "part": baseline["part"], "clock_period_ns": period,
              "source": str(source), "output": str(output), "results": {},
              "not_interchangeable_with_virtual_io_qor": True}
    try:
        os.environ["HI_HLS_LIB_PATH"] = str(fixture.parents[1] / "HLS_Data_Lib")
        light = output / "light_hls"
        light.mkdir()
        run_command([str(compiler), str(source), "qor_pressure", "--board", "u250", "--freq",
                     f"{1000 / period:g}MHz", "--output", str(light)], light, light / "compiler.log", 300)
        run_command(["clang++", "-std=c++17", "-Wno-unknown-pragmas", "-I", str(fixture / "software"),
                     str(source), str(fixture / "sustained_reference.cpp"), "-o", str(output / "reference")],
                    output, output / "reference_build.log", 120)
        run_command([str(output / "reference")], output, output / "reference.log", 120)
        reference = [line for line in (output / "reference.log").read_text().splitlines() if line.startswith("RESULT ")]
        if len(reference) != 8192:
            raise ValueError("Incomplete native C reference")
        shell = output / "registered_shell.v"
        write_shell(shell, source.name == "dsp_dataflow.cpp")
        result["shell_sha256"] = sha256_file(shell)
        result["testbench_sha256"] = sha256_file(Path(__file__).with_name("registered_pressure_tb.v"))
        for generator in ("light_hls", "vitis_hls"):
            directory = output / (generator + "_context")
            directory.mkdir()
            native = ([light / "reports/rtl/top.v"] if generator == "light_hls" else
                      sorted((comparison.parent / "vitis_hls/vitis_project/solution/syn/verilog").glob("*.v")))
            wrapper = Path(baseline["interface_normalization"][generator + "_wrapper"])
            rtl = native + [wrapper, shell]
            hashes = {str(p): sha256_file(p) for p in rtl}
            simulation = run_simulation(executable_path("iverilog", "Icarus"), rtl,
                Path(__file__).with_name("registered_pressure_tb.v"), "registered_pressure_tb",
                directory, 900, ["SHELL_SIM_CLOCK"], "xsim")
            if simulation["trace"] != reference:
                raise ValueError(f"{generator} shell output differs from native C")
            entry = {"functional_valid": True, "reference_outputs": len(reference), "rtl_sha256": hashes}
            result["results"][generator] = entry
            if not args.simulate_only:
                write_flow(directory, rtl, period)
                runtime = run_command(affinity_prefix() + [str(executable_path("vivado", "Vivado")),
                    "-mode", "batch", "-source", str(directory / "run.tcl")], directory, directory / "vivado.log", 1800)
                physical = parse_route_metrics(directory, period)
                physical.update(flow_id=FLOW_ID, clock_scope=SCOPE, qor_kind="measured_registered_context",
                                runtime_seconds=runtime)
                physical.pop("runtime_note", None)
                # The legacy virtual IO budget is inapplicable, not a relaxed constraint.
                physical["timing"].pop("io_budget_fraction", None)
                physical["timing"]["critical_path_delay_note"] = (
                    "Period minus routed WNS in the closed registered context; not a frequency sweep or board Fmax")
                physical["kernel_resources"] = resource_counts((directory / "kernel_utilization.rpt").read_text())
                required = "bram_tiles" if source.name == "bram_banked.cpp" else "dsp"
                if physical["kernel_resources"][required] <= 0:
                    raise ValueError(f"Required kernel {required} disappeared in the physical context")
                physical["shell_resources"] = {key: value - physical["kernel_resources"][key]
                    for key, value in physical["resources"].items()}
                if min(physical["shell_resources"].values()) < 0:
                    raise ValueError("Inconsistent hierarchical resource accounting")
                entry["post_route"] = physical
            if hashes != {str(p): sha256_file(p) for p in rtl}:
                raise ValueError("RTL changed during verification")
            (output / "registered_context.json").write_text(json.dumps(result, indent=2) + "\n")
        if sha256_file(compiler) != result["compiler_sha256"]:
            raise ValueError("Compiler changed during verification")
        result["valid"] = True
        result["physical_valid"] = not args.simulate_only and all(
            entry["post_route"]["timing_met"] for entry in result["results"].values())
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        (output / "registered_context.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"valid": result["valid"], "physical_valid": result["physical_valid"]}))


if __name__ == "__main__":
    main()
