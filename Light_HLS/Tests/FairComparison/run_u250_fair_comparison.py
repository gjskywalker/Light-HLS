#!/usr/bin/env python3

"""Compare Light-HLS and Vitis HLS RTL with one U250 synthesis flow."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any, Iterable, Optional


U250_PART = "xcu250-figd2104-2L-e"
FLOW_ID = "vivado_ooc_normalized_v3_pre_synth_io20"


class ComparisonError(RuntimeError):
    pass


def absolute_existing_file(value: str, description: str) -> Path:
    path = Path(value).expanduser().resolve()
    if not path.is_file():
        raise ComparisonError(f"{description} does not exist: {path}")
    return path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def executable_path(value: str, description: str) -> Path:
    candidate = shutil.which(value)
    if not candidate:
        raise ComparisonError(f"Cannot find {description} executable: {value}")
    return Path(candidate).resolve()


def affinity_prefix() -> list[str]:
    taskset = shutil.which("taskset")
    if not taskset or not hasattr(os, "sched_getaffinity"):
        return []
    available_cpus = sorted(os.sched_getaffinity(0))
    if not available_cpus:
        return []
    return [taskset, "-c", str(available_cpus[0])]


def tcl_brace(value: Path | str) -> str:
    text = str(value).replace("\\", "\\\\").replace("}", "\\}")
    return "{" + text + "}"


def run_command(
    command: list[str], cwd: Path, log_path: Path, timeout_seconds: int
) -> float:
    start = time.monotonic()
    with log_path.open("w", encoding="utf-8") as log:
        log.write("Command: " + " ".join(command) + "\n\n")
        log.flush()
        try:
            completed = subprocess.run(
                command,
                cwd=cwd,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=timeout_seconds,
                check=False,
            )
        except subprocess.TimeoutExpired as error:
            raise ComparisonError(
                f"Command timed out after {timeout_seconds} seconds; see {log_path}"
            ) from error
    runtime = time.monotonic() - start
    if completed.returncode != 0:
        raise ComparisonError(
            f"Command failed with exit code {completed.returncode}; see {log_path}"
        )
    return runtime


def write_vitis_hls_tcl(
    path: Path, source: Path, top: str, clock_period_ns: float
) -> None:
    content = "\n".join(
        [
            "open_project -reset vitis_project",
            f"set_top {tcl_brace(top)}",
            f"add_files {tcl_brace(source)}",
            "open_solution -reset solution -flow_target vivado",
            f"set_part {tcl_brace(U250_PART)}",
            f"create_clock -period {clock_period_ns:.12g} -name default",
            "config_compile -pipeline_loops 0",
            "csynth_design",
            "close_project",
            "exit",
            "",
        ]
    )
    path.write_text(content, encoding="utf-8")


def write_common_vivado_tcl(
    path: Path,
    rtl_files: Iterable[Path],
    top: str,
    clock_period_ns: float,
    output_directory: Path,
    implement: bool = False,
) -> None:
    rtl_arguments = " ".join(tcl_brace(item) for item in rtl_files)
    utilization = output_directory / "utilization.rpt"
    timing = output_directory / "timing_summary.rpt"
    checkpoint = output_directory / "post_synth.dcp"
    metrics = output_directory / "metrics.txt"
    constraints = output_directory / "constraints.xdc"
    constraints.write_text("\n".join([
        "# Identical external timing budget for both normalized interfaces.",
        "set clock_ports [get_ports -quiet ap_clk]",
        f"if {{[llength $clock_ports]}} {{ create_clock -name ap_clk -period {clock_period_ns:.12g} $clock_ports }} else {{ create_clock -name ap_clk -period {clock_period_ns:.12g} }}",
        "set data_inputs [get_ports -filter {DIRECTION == IN && NAME != ap_clk}]",
        f"if {{[llength $data_inputs]}} {{ set_input_delay -clock ap_clk -max {clock_period_ns * 0.2:.12g} $data_inputs; set_input_delay -clock ap_clk -min 0 $data_inputs }}",
        f"if {{[llength [all_outputs]]}} {{ set_output_delay -clock ap_clk -max {clock_period_ns * 0.2:.12g} [all_outputs]; set_output_delay -clock ap_clk -min 0 [all_outputs] }}",
        "",
    ]), encoding="utf-8")
    commands = [
            "set_param general.maxThreads 1",
            f"read_verilog -sv {rtl_arguments}",
            f"read_xdc -unmanaged {tcl_brace(constraints)}",
            f"synth_design -top {tcl_brace(top)} -part {tcl_brace(U250_PART)} -mode out_of_context -flatten_hierarchy rebuilt",
            "set clock_ports [get_ports -quiet ap_clk]",
            'if {[llength [get_clocks -quiet ap_clk]] != 1} { error "Required pre-synthesis clock constraint is missing" }',
            f"write_checkpoint -force {tcl_brace(checkpoint)}",
            f"report_utilization -file {tcl_brace(utilization)}",
            f"report_timing_summary -delay_type max -max_paths 10 -file {tcl_brace(timing)}",
            "set utilization [report_utilization -return_string]",
            "set lut -1",
            "set ff -1",
            "set dsp -1",
            "set bram -1",
            "regexp -line {\\|\\s*(Slice LUTs|CLB LUTs)\\*?\\s*\\|\\s*([0-9]+)} $utilization match label lut",
            "regexp -line {\\|\\s*(Slice Registers|CLB Registers)\\s*\\|\\s*([0-9]+)} $utilization match label ff",
            "regexp -line {\\|\\s*DSPs\\s*\\|\\s*([0-9]+)} $utilization match dsp",
            "regexp -line {\\|\\s*Block RAM Tile\\s*\\|\\s*([0-9.]+)} $utilization match bram",
            "set wns nan",
            "set timing_paths [get_timing_paths -delay_type max -max_paths 1]",
            "if {[llength $timing_paths] > 0} { set wns [get_property SLACK [lindex $timing_paths 0]] }",
            f"set metrics [open {tcl_brace(metrics)} w]",
            'puts $metrics "tool_version=[version -short]"',
            'puts $metrics "lut=$lut"',
            'puts $metrics "ff=$ff"',
            'puts $metrics "dsp=$dsp"',
            'puts $metrics "bram_tiles=$bram"',
            'puts $metrics "clock_constrained=[expr {[llength [get_clocks -quiet ap_clk]] > 0}]"',
            'puts $metrics "constraints_before_synthesis=1"',
            'puts $metrics "io_budget_fraction=0.2"',
            'puts $metrics "worst_slack_ns=$wns"',
            "foreach port [lsort [get_ports]] {",
            "  set port_name [get_property NAME $port]",
            "  set direction [get_property DIRECTION $port]",
            "  set left [get_property -quiet LEFT $port]",
            "  set right [get_property -quiet RIGHT $port]",
            '  puts $metrics "port=$port_name|$direction|$left|$right"',
            "}",
            "close $metrics",
        ]
    if implement:
        # Reuse exactly the same metric collection after routing. Synthesis
        # metrics and DCP remain separate, and the XDC survives in the DCP.
        measurement = commands[commands.index("set utilization [report_utilization -return_string]"):]
        commands += ["opt_design", "place_design", "phys_opt_design", "route_design",
            f"write_checkpoint -force {tcl_brace(output_directory / 'post_route.dcp')}",
            f"report_utilization -file {tcl_brace(output_directory / 'route_utilization.rpt')}",
            f"report_timing_summary -delay_type min_max -report_unconstrained -file {tcl_brace(output_directory / 'route_timing_summary.rpt')}",
            f"report_route_status -file {tcl_brace(output_directory / 'route_status.rpt')}",
            f"report_drc -file {tcl_brace(output_directory / 'route_drc.rpt')}"]
        commands += [line.replace(tcl_brace(metrics), tcl_brace(output_directory / "route_metrics.txt"))
                     for line in measurement]
        commands += [f"set metrics [open {tcl_brace(output_directory / 'route_metrics.txt')} a]",
            "set hold_paths [get_timing_paths -delay_type min -max_paths 1]",
            'puts $metrics "worst_hold_slack_ns=[get_property SLACK $hold_paths]"',
            'puts $metrics "unrouted_nets=[llength [get_nets -hier -filter {ROUTE_STATUS == UNROUTED || ROUTE_STATUS == PARTIALLY_ROUTED}]]"',
            "close $metrics"]
    path.write_text("\n".join(commands + ["exit", ""]), encoding="utf-8")


def normalize_port_signature(bit_ports: Iterable[str]) -> list[str]:
    groups: dict[tuple[str, str], set[int]] = {}
    scalar_ports: set[tuple[str, str]] = set()
    for entry in bit_ports:
        fields = entry.split("|", 3)
        if len(fields) != 4:
            raise ComparisonError(f"Invalid Vivado port signature entry: {entry}")
        name, direction = fields[0], fields[1]
        indexed = re.fullmatch(r"(.+)\[([0-9]+)\]", name)
        if indexed:
            key = (indexed.group(1), direction)
            groups.setdefault(key, set()).add(int(indexed.group(2)))
        else:
            scalar_ports.add((name, direction))

    signature = [
        f"{name}|{direction}|1" for name, direction in scalar_ports
    ]
    signature.extend(
        f"{name}|{direction}|{len(indices)}"
        for (name, direction), indices in groups.items()
    )
    return sorted(signature)


def parse_interface_signature(entries: Iterable[str]) -> dict[str, dict[str, Any]]:
    ports: dict[str, dict[str, Any]] = {}
    for entry in entries:
        fields = entry.split("|")
        if len(fields) != 3 or fields[1] not in {"IN", "OUT", "INOUT"}:
            raise ComparisonError(f"Invalid normalized port signature: {entry}")
        try:
            width = int(fields[2])
        except ValueError as error:
            raise ComparisonError(f"Invalid port width in signature: {entry}") from error
        if width <= 0 or fields[0] in ports:
            raise ComparisonError(f"Invalid or duplicate port signature: {entry}")
        ports[fields[0]] = {"direction": fields[1], "width": width}
    return ports


def row_major_coordinates(index: int, dimensions: list[int]) -> list[int]:
    coordinates = [0] * len(dimensions)
    for reverse in range(len(dimensions) - 1, -1, -1):
        coordinates[reverse] = index % dimensions[reverse]
        index //= dimensions[reverse]
    return coordinates


def scalar_partition_aliases(
    binding_path: Path,
    light_ports: dict[str, dict[str, Any]],
    vitis_ports: dict[str, dict[str, Any]],
) -> dict[str, str]:
    binding = json.loads(binding_path.read_text(encoding="utf-8"))
    aliases: dict[str, str] = {}
    for memory in binding.get("memory_objects", []):
        if memory.get("storage") != "external" or memory.get("bank_interface") != "scalar":
            continue
        dimensions = memory.get("dimensions", [])
        banks = memory.get("banks", 0)
        if not dimensions or math.prod(dimensions) != banks:
            continue
        source_base = memory.get("name", "")
        canonical_base = memory.get("port_name", source_base)
        access_mode = memory.get("access_mode")
        for bank in range(banks):
            canonical_stem = f"{canonical_base}_{bank}"
            coordinate_suffix = "".join(
                f"_{coordinate}"
                for coordinate in row_major_coordinates(bank, dimensions)
            )
            vitis_stem = source_base + coordinate_suffix
            suffixes = [""]
            if access_mode == "write_only":
                suffixes = ["", "_ap_vld"]
            elif access_mode == "read_write":
                suffixes = ["_i", "_o", "_o_ap_vld"]
            for suffix in suffixes:
                canonical = canonical_stem + suffix
                native = vitis_stem + suffix
                if canonical in light_ports and native in vitis_ports:
                    aliases[native] = canonical
    return aliases


def make_normalized_contract(
    light_signature: list[str],
    vitis_signature: list[str],
    aliases: dict[str, str],
) -> tuple[dict[str, dict[str, Any]], dict[str, str], dict[str, str]]:
    light_ports = parse_interface_signature(light_signature)
    vitis_ports = parse_interface_signature(vitis_signature)
    light_mapping = {name: name for name in light_ports}
    vitis_mapping = {name: aliases.get(name, name) for name in vitis_ports}
    contract: dict[str, dict[str, Any]] = {}
    for native_ports, mapping in (
        (light_ports, light_mapping),
        (vitis_ports, vitis_mapping),
    ):
        used_canonical: set[str] = set()
        for native, port in native_ports.items():
            canonical = mapping[native]
            if canonical in used_canonical:
                raise ComparisonError(
                    f"More than one native port maps to canonical port {canonical}"
                )
            used_canonical.add(canonical)
            if canonical in contract and contract[canonical] != port:
                raise ComparisonError(
                    f"Canonical port {canonical} has incompatible native definitions"
                )
            contract[canonical] = dict(port)
    return contract, light_mapping, vitis_mapping


def verilog_range(width: int) -> str:
    return "" if width == 1 else f"[{width - 1}:0] "


def write_interface_wrapper(
    path: Path,
    wrapper_top: str,
    native_top: str,
    contract: dict[str, dict[str, Any]],
    native_signature: list[str],
    native_mapping: dict[str, str],
) -> None:
    native_ports = parse_interface_signature(native_signature)
    declarations = []
    for name in sorted(contract):
        port = contract[name]
        direction = {"IN": "input wire", "OUT": "output wire", "INOUT": "inout wire"}[
            port["direction"]
        ]
        declarations.append(f"  {direction} {verilog_range(port['width'])}{name}")
    connections = [
        f"    .{native}({native_mapping[native]})" for native in sorted(native_ports)
    ]
    driven_outputs = {
        native_mapping[native]
        for native, port in native_ports.items()
        if port["direction"] in {"OUT", "INOUT"}
    }
    tied_outputs = [
        f"  assign {name} = {contract[name]['width']}'d0;"
        for name in sorted(contract)
        if contract[name]["direction"] == "OUT" and name not in driven_outputs
    ]
    content = [
        "`timescale 1ns/1ps",
        "// Common fair-comparison interface wrapper.",
        f"module {wrapper_top} (",
        ",\n".join(declarations),
        ");",
        f"  {native_top} native_instance (",
        ",\n".join(connections),
        "  );",
        *tied_outputs,
        "endmodule",
        "",
    ]
    path.write_text("\n".join(content), encoding="utf-8")


def parse_metrics(path: Path, clock_period_ns: float, runtime: float) -> dict[str, Any]:
    scalar_values: dict[str, str] = {}
    ports: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key == "port":
            ports.append(value)
        else:
            scalar_values[key] = value

    try:
        lut = int(scalar_values["lut"])
        ff = int(scalar_values["ff"])
        dsp = int(scalar_values["dsp"])
        bram_tiles = float(scalar_values["bram_tiles"])
        slack_text = scalar_values.get("worst_slack_ns", "")
        parsed_slack = float(slack_text) if slack_text else math.nan
    except (KeyError, ValueError) as error:
        raise ComparisonError(f"Cannot parse Vivado metrics in {path}") from error
    if lut < 0 or ff < 0 or dsp < 0 or bram_tiles < 0 or not math.isfinite(bram_tiles):
        raise ComparisonError(f"Vivado returned invalid utilization metrics in {path}")
    clock_constrained = scalar_values.get("clock_constrained") == "1"
    worst_slack = parsed_slack if math.isfinite(parsed_slack) else None
    critical_path = (
        round(clock_period_ns - parsed_slack, 6)
        if clock_constrained and worst_slack is not None
        else None
    )

    return {
        "valid": True,
        "qor_kind": "measured_post_rtl_common_flow",
        "tool": "vivado",
        "tool_version": scalar_values.get("tool_version", ""),
        "part": U250_PART,
        "flow_id": FLOW_ID,
        "runtime_seconds": round(runtime, 6),
        "resources": {
            "lut": lut,
            "ff": ff,
            "dsp": dsp,
            "bram_tiles": bram_tiles,
        },
        "timing": {
            "clock_constrained": clock_constrained,
            "constraints_before_synthesis": scalar_values.get("constraints_before_synthesis") == "1",
            "io_budget_fraction": float(scalar_values.get("io_budget_fraction", "0")),
            "worst_slack_ns": worst_slack,
            "critical_path_delay_ns": critical_path,
            "critical_path_delay_note": "Period minus WNS includes external IO budgets; it is not a routed Fmax measurement.",
        },
        "interface_signature": normalize_port_signature(ports),
    }


def parse_route_metrics(directory: Path, clock_period_ns: float) -> dict[str, Any]:
    result = parse_metrics(directory / "route_metrics.txt", clock_period_ns, 0)
    values = dict(line.split("=", 1) for line in
                  (directory / "route_metrics.txt").read_text().splitlines() if "=" in line)
    route_status = (directory / "route_status.rpt").read_text()
    errors = re.search(r"# of nets with routing errors\.+\s*:\s*(\d+)", route_status)
    if not errors:
        raise ComparisonError("Missing routing error count")
    timing_report = (directory / "route_timing_summary.rpt").read_text()
    coverage = {}
    for name in ("no_clock", "unconstrained_internal_endpoints", "no_input_delay", "no_output_delay",
                 "partial_input_delay", "partial_output_delay"):
        match = re.search(r"checking " + name + r" \((\d+)\)", timing_report)
        if not match:
            raise ComparisonError(f"Missing timing coverage check: {name}")
        coverage[name] = int(match.group(1))
    result.update(qor_kind="measured_post_route_ooc", runtime_seconds=None,
        runtime_note="Included in the combined synthesis/implementation runtime",
        clock_scope="Virtual OOC IO boundary: no board shell, physical port locations or clock tree contract",
        unrouted_nets=int(values["unrouted_nets"]), routing_errors=int(errors.group(1)), timing_coverage=coverage)
    result["timing"]["worst_hold_slack_ns"] = float(values["worst_hold_slack_ns"])
    result["timing_met"] = (result["unrouted_nets"] == 0 and result["routing_errors"] == 0 and
        not any(coverage.values()) and result["timing"]["worst_slack_ns"] is not None and
        result["timing"]["worst_slack_ns"] >= 0 and result["timing"]["worst_hold_slack_ns"] >= 0)
    return result


def optional_integer(text: Optional[str]) -> Optional[int]:
    if text is None or not re.fullmatch(r"-?[0-9]+", text.strip()):
        return None
    value = int(text)
    return value if value >= 0 else None


def optional_float(text: Optional[str]) -> Optional[float]:
    if text is None:
        return None
    try:
        return float(text)
    except ValueError:
        return None


def parse_vitis_estimate(path: Path) -> dict[str, Any]:
    root = ET.parse(path).getroot()
    latency = root.find("./PerformanceEstimates/SummaryOfOverallLatency")
    resources = root.find("./AreaEstimates/Resources")
    timing = root.find("./PerformanceEstimates/SummaryOfTimingAnalysis")
    loops = []
    loop_summary = root.find("./PerformanceEstimates/SummaryOfLoopLatency")
    if loop_summary is not None:
        for loop in loop_summary:
            loops.append(
                {
                    "name": loop.tag,
                    "trip_count": optional_integer(loop.findtext("TripCount")),
                    "latency_cycles": optional_integer(loop.findtext("Latency")),
                    "achieved_ii": optional_integer(loop.findtext("PipelineII")),
                    "pipeline_depth": optional_integer(
                        loop.findtext("PipelineDepth")
                    ),
                }
            )
    return {
        "qor_kind": "vitis_hls_pre_rtl_estimate",
        "tool_version": root.findtext("./ReportVersion/Version", default=""),
        "latency_cycles": {
            "best": optional_integer(
                latency.findtext("Best-caseLatency") if latency is not None else None
            ),
            "average": optional_integer(
                latency.findtext("Average-caseLatency") if latency is not None else None
            ),
            "worst": optional_integer(
                latency.findtext("Worst-caseLatency") if latency is not None else None
            ),
        },
        "interval_cycles": {
            "minimum": optional_integer(
                latency.findtext("Interval-min") if latency is not None else None
            ),
            "maximum": optional_integer(
                latency.findtext("Interval-max") if latency is not None else None
            ),
        },
        "estimated_clock_period_ns": optional_float(
            timing.findtext("EstimatedClockPeriod") if timing is not None else None
        ),
        "loops": loops,
        "resources": {
            "lut": optional_integer(resources.findtext("LUT") if resources is not None else None),
            "ff": optional_integer(resources.findtext("FF") if resources is not None else None),
            "dsp": optional_integer(resources.findtext("DSP") if resources is not None else None),
            "bram_18k": optional_integer(
                resources.findtext("BRAM_18K") if resources is not None else None
            ),
            "uram": optional_integer(
                resources.findtext("URAM") if resources is not None else None
            ),
        },
    }


def parse_light_schedule(
    path: Path, pipeline_path: Path, dataflow_path: Path
) -> dict[str, Any]:
    schedule = json.loads(path.read_text(encoding="utf-8"))
    pipeline_schedule = json.loads(pipeline_path.read_text(encoding="utf-8"))
    dataflow_schedule = json.loads(dataflow_path.read_text(encoding="utf-8"))
    return {
        "qor_kind": schedule.get("qor_kind"),
        "latency_cycles": schedule.get("latency_cycles"),
        "latency_reason": schedule.get("latency_reason"),
        "state_count": schedule.get("state_count"),
        "achieved_ii": schedule.get("achieved_ii"),
        "memory_port_conflicts": schedule.get("memory_port_conflicts"),
        "memory_port_serialization_cycles": schedule.get(
            "memory_port_serialization_cycles"
        ),
        "pipelines": pipeline_schedule.get("pipelines", []),
        "dataflow": dataflow_schedule,
    }


def parse_transaction_events(lines: list[str]) -> dict[str, Any]:
    events = []
    for line in lines:
        if not line.startswith("EVENT "):
            continue
        event = {key: int(value) for key, value in re.findall(r"(\w+)=(-?\d+)", line)}
        if not {"trial", "accepted", "last_commit", "done"}.issubset(event):
            raise ComparisonError(f"Incomplete transaction event record: {line}")
        if min(event["accepted"], event["last_commit"], event["done"]) < 0:
            raise ComparisonError(f"Unobserved transaction event: {line}")
        if event["done"] < event["accepted"] or event["last_commit"] < event["accepted"]:
            raise ComparisonError(f"Transaction event precedes acceptance: {line}")
        if events and (event["trial"] <= events[-1]["trial"] or event["accepted"] <= events[-1]["accepted"]):
            raise ComparisonError(f"Non-monotone transaction event: {line}")
        events.append(event)
    return {
        "transaction_events": events,
        "final_commit_cycles": [event["last_commit"] - event["accepted"] for event in events],
        "completion_signal_cycles": [event["done"] - event["accepted"] for event in events],
        "observed_acceptance_intervals": [right["accepted"] - left["accepted"]
                                          for left, right in zip(events, events[1:])],
        "event_metric_note": "Acceptance intervals include testbench pacing; they are not a proven minimum II.",
    }


def run_simulation(
    iverilog: Path,
    rtl_files: list[Path],
    testbench: Path,
    testbench_top: str,
    work_directory: Path,
    timeout_seconds: int,
    defines: list[str],
    simulator: str = "iverilog",
) -> dict[str, Any]:
    executable = work_directory / "simulation.out"
    compile_log = work_directory / "iverilog.log"
    run_log = work_directory / "simulation.log"
    if simulator == "xsim":
        compile_runtime = run_command(
            # Existing DATAFLOW net initializers reference later wire declarations.
            # Relaxed parsing preserves the same RTL used by Icarus and synthesis.
            affinity_prefix() + [str(executable_path("xvlog", "Vivado Verilog compiler")), "--sv", "--relax",
                *(item for define in defines for item in ("-d", define)),
                *(str(path) for path in rtl_files), str(testbench)],
            work_directory, work_directory / "xvlog.log", timeout_seconds)
        compile_runtime += run_command(
            affinity_prefix() + [str(executable_path("xelab", "Vivado elaborator")),
                "-mt", "off", "-s", "comparison_sim", testbench_top],
            work_directory, work_directory / "xelab.log", timeout_seconds)
        run_runtime = run_command(
            affinity_prefix() + [str(executable_path("xsim", "Vivado simulator")),
                "comparison_sim", "-runall"], work_directory, run_log, timeout_seconds)
    elif simulator == "iverilog":
        compile_runtime = run_command(
            [
                str(iverilog), "-g2012", "-Wall", "-s", testbench_top,
                *(item for define in defines for item in ("-D", define)),
                "-o", str(executable), *(str(path) for path in rtl_files), str(testbench),
            ],
            work_directory, compile_log, timeout_seconds,
        )
        run_runtime = run_command(
            ["vvp", str(executable)], work_directory, run_log, timeout_seconds
        )
    else:
        raise ComparisonError(f"Unknown RTL simulator: {simulator}")
    lines = run_log.read_text(encoding="utf-8").splitlines()
    if any(re.search(r"(?i)\b(fatal|error):", line) for line in lines):
        raise ComparisonError(f"RTL simulator reported an error; see {run_log}")
    trace = [line for line in lines if line.startswith("RESULT ")]
    cycles = [int(match.group(1)) for line in lines if (match := re.search(r"cycles=([0-9]+)", line))]
    if not trace:
        raise ComparisonError(f"No RESULT trace was produced; see {run_log}")
    return {
        "valid": True,
        "simulator": simulator,
        "trace": trace,
        "transaction_cycles": cycles,
        "mean_transaction_cycles": (
            statistics.fmean(cycles) if cycles else None
        ),
        "compile_runtime_seconds": round(compile_runtime, 6),
        "simulation_runtime_seconds": round(run_runtime, 6),
        **parse_transaction_events(lines),
    }


def ratio(
    numerator: Optional[float], denominator: Optional[float]
) -> Optional[float]:
    if numerator is None or denominator in (None, 0):
        return None
    return round(numerator / denominator, 8)


def make_deltas(light: dict[str, Any], vitis: dict[str, Any]) -> dict[str, Any]:
    light_resources = light["resources"]
    vitis_resources = vitis["resources"]
    resource_ratios = {
        name: ratio(float(light_resources[name]), float(vitis_resources[name]))
        for name in ("lut", "ff", "dsp", "bram_tiles")
    }
    light_slack = light["timing"]["worst_slack_ns"]
    vitis_slack = vitis["timing"]["worst_slack_ns"]
    return {
        "light_hls_minus_vitis_hls": {
            "lut": light_resources["lut"] - vitis_resources["lut"],
            "ff": light_resources["ff"] - vitis_resources["ff"],
            "dsp": light_resources["dsp"] - vitis_resources["dsp"],
            "bram_tiles": light_resources["bram_tiles"]
            - vitis_resources["bram_tiles"],
            "worst_slack_ns": (
                light_slack - vitis_slack
                if light_slack is not None and vitis_slack is not None
                else None
            ),
        },
        "light_hls_over_vitis_hls": {
            "resources": resource_ratios,
            "critical_path_delay": ratio(
                light["timing"]["critical_path_delay_ns"],
                vitis["timing"]["critical_path_delay_ns"],
            ),
        },
    }


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--light-hls-top", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--top", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--config")
    parser.add_argument("--testbench")
    parser.add_argument("--testbench-top")
    parser.add_argument(
        "--testbench-define",
        action="append",
        default=[],
        help="Define passed to both Icarus simulations; may be repeated",
    )
    parser.add_argument(
        "--vitis-testbench-define",
        action="append",
        default=[],
        help="Additional define passed only to the Vitis RTL simulation",
    )
    parser.add_argument("--clock-period-ns", type=float, default=10.0)
    parser.add_argument(
        "--vitis-run", default="/vitis/2025.2/Vitis/bin/vitis-run"
    )
    parser.add_argument("--vivado", default="vivado")
    parser.add_argument("--iverilog", default="iverilog")
    parser.add_argument("--rtl-simulator", choices=("iverilog", "xsim"), default="iverilog")
    parser.add_argument("--implement", action="store_true",
                        help="Place and route both normalized designs; preserve synthesis results")
    parser.add_argument("--timeout-seconds", type=int, default=900)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    output_root = Path(arguments.output).expanduser().resolve()
    report_path = output_root / "u250_fair_comparison.json"
    report: dict[str, Any] = {
        "schema_version": "1.0",
        "stage": "u250_fair_comparison",
        "valid": False,
        "reward_eligible": False,
        "reward_scope": "post_route_ooc" if arguments.implement else "post_synthesis_ooc",
        "board": "u250",
        "part": U250_PART,
        "clock_period_ns": arguments.clock_period_ns,
        "top_function": arguments.top,
        "errors": [],
    }
    output_initialized = False

    try:
        if not math.isfinite(arguments.clock_period_ns) or arguments.clock_period_ns <= 0:
            raise ComparisonError("Clock period must be positive")
        if arguments.timeout_seconds <= 0:
            raise ComparisonError("Timeout must be positive")
        if output_root.exists() and not output_root.is_dir():
            raise ComparisonError(f"Output path is not a directory: {output_root}")
        if output_root.exists() and any(output_root.iterdir()):
            raise ComparisonError(
                f"Output directory must be absent or empty: {output_root}"
            )
        output_root.mkdir(parents=True, exist_ok=True)
        output_initialized = True

        light_hls_top = executable_path(arguments.light_hls_top, "Light_HLS_Top")
        vitis_run = executable_path(arguments.vitis_run, "vitis-run")
        vivado = executable_path(arguments.vivado, "Vivado")
        source = absolute_existing_file(arguments.source, "Source file")
        config = (
            absolute_existing_file(arguments.config, "Light-HLS config")
            if arguments.config
            else None
        )
        testbench = (
            absolute_existing_file(arguments.testbench, "RTL testbench")
            if arguments.testbench
            else None
        )
        if bool(testbench) != bool(arguments.testbench_top):
            raise ComparisonError(
                "--testbench and --testbench-top must be provided together"
            )
        for define in (
            arguments.testbench_define + arguments.vitis_testbench_define
        ):
            if not re.fullmatch(
                r"[A-Za-z_][A-Za-z0-9_]*(?:=[A-Za-z0-9_]+)?", define
            ):
                raise ComparisonError(f"Invalid Icarus define: {define}")
        iverilog = (
            executable_path(arguments.iverilog, "Icarus Verilog")
            if testbench
            else None
        )

        report["source"] = str(source)
        report["source_sha256"] = sha256_file(source)
        report["config_sha256"] = sha256_file(config) if config else None
        report["testbench_sha256"] = (
            sha256_file(testbench) if testbench else None
        )
        report["light_hls_executable_sha256"] = sha256_file(light_hls_top)
        identity = {
            "source_sha256": report["source_sha256"],
            "config_sha256": report["config_sha256"],
            "testbench_sha256": report["testbench_sha256"],
            "light_hls_executable_sha256": report[
                "light_hls_executable_sha256"
            ],
            "top_function": arguments.top,
            "board": "u250",
            "part": U250_PART,
            "clock_period_ns": arguments.clock_period_ns,
            "flow_id": FLOW_ID,
            "common_defines": arguments.testbench_define,
            "vitis_only_defines": arguments.vitis_testbench_define,
            "rtl_simulator": arguments.rtl_simulator,
            "implementation_requested": arguments.implement,
        }
        report["comparison_id"] = hashlib.sha256(
            json.dumps(identity, sort_keys=True, separators=(",", ":")).encode(
                "utf-8"
            )
        ).hexdigest()
        report["input_fingerprints"] = identity
        report["fairness_contract"] = {
            "same_source": True,
            "same_top_function": True,
            "same_clock_period": True,
            "same_fpga_part": True,
            "same_vivado_ooc_flow": True,
            "vitis_automatic_loop_pipelining": False,
            "top_level_boundary": "normalized_ap_ctrl_hs_superset_wrapper",
            "flow_id": FLOW_ID,
            "simulation_adapter": {
                "common_defines": arguments.testbench_define,
                "vitis_only_defines": arguments.vitis_testbench_define,
                "rtl_simulator": arguments.rtl_simulator,
            },
        }

        light_root = output_root / "light_hls"
        light_work = light_root / "work"
        light_reports = light_root / "reports"
        vitis_root = output_root / "vitis_hls"
        light_work.mkdir(parents=True)
        vitis_root.mkdir(parents=True)

        light_command = [str(light_hls_top), str(source), arguments.top]
        if config:
            light_command.append(str(config))
        light_frequency_mhz = 1000.0 / arguments.clock_period_ns
        light_command.extend(
            [
                "--board",
                "u250",
                "--freq",
                f"{light_frequency_mhz:.12g}MHz",
                "--synthesis",
                "disable",
                "--output",
                str(light_root),
            ]
        )
        light_generation_runtime = run_command(
            light_command,
            light_work,
            light_root / "light_hls.log",
            arguments.timeout_seconds,
        )
        light_rtl = light_reports / "rtl" / "top.v"
        if not light_rtl.is_file():
            raise ComparisonError(f"Light-HLS did not generate RTL: {light_rtl}")

        vitis_tcl = vitis_root / "run_hls.tcl"
        write_vitis_hls_tcl(
            vitis_tcl, source, arguments.top, arguments.clock_period_ns
        )
        vitis_generation_runtime = run_command(
            [str(vitis_run), "--mode", "hls", "--tcl", str(vitis_tcl)],
            vitis_root,
            vitis_root / "vitis_hls.log",
            arguments.timeout_seconds,
        )
        vitis_solution = vitis_root / "vitis_project" / "solution"
        vitis_rtl_directory = vitis_solution / "syn" / "verilog"
        vitis_rtl = sorted(vitis_rtl_directory.glob("*.v"))
        if not vitis_rtl:
            raise ComparisonError(
                f"Vitis HLS did not generate Verilog in {vitis_rtl_directory}"
            )
        vitis_csynth_xml = vitis_solution / "syn" / "report" / "csynth.xml"
        if not vitis_csynth_xml.is_file():
            raise ComparisonError(f"Vitis HLS report is missing: {vitis_csynth_xml}")

        native_synthesis_results: dict[str, dict[str, Any]] = {}
        for generator, rtl_files in (
            ("light_hls", [light_rtl]),
            ("vitis_hls", vitis_rtl),
        ):
            synthesis_directory = output_root / "common_synthesis" / generator
            synthesis_directory.mkdir(parents=True)
            synthesis_tcl = synthesis_directory / "run_vivado.tcl"
            write_common_vivado_tcl(
                synthesis_tcl,
                rtl_files,
                arguments.top,
                arguments.clock_period_ns,
                synthesis_directory,
            )
            runtime = run_command(
                affinity_prefix()
                + [
                    str(vivado),
                    "-mode",
                    "batch",
                    "-nolog",
                    "-nojournal",
                    "-source",
                    str(synthesis_tcl),
                ],
                synthesis_directory,
                synthesis_directory / "vivado.log",
                arguments.timeout_seconds,
            )
            native_synthesis_results[generator] = parse_metrics(
                synthesis_directory / "metrics.txt",
                arguments.clock_period_ns,
                runtime,
            )

        native_interface_compatible = (
            native_synthesis_results["light_hls"]["interface_signature"]
            == native_synthesis_results["vitis_hls"]["interface_signature"]
        )
        light_ports = parse_interface_signature(
            native_synthesis_results["light_hls"]["interface_signature"]
        )
        vitis_ports = parse_interface_signature(
            native_synthesis_results["vitis_hls"]["interface_signature"]
        )
        aliases = scalar_partition_aliases(
            light_reports / "binding" / "binding.json", light_ports, vitis_ports
        )
        contract, light_mapping, vitis_mapping = make_normalized_contract(
            native_synthesis_results["light_hls"]["interface_signature"],
            native_synthesis_results["vitis_hls"]["interface_signature"],
            aliases,
        )
        normalized_top = arguments.top + "__normalized"
        wrappers: dict[str, Path] = {}
        for generator, signature, mapping in (
            (
                "light_hls",
                native_synthesis_results["light_hls"]["interface_signature"],
                light_mapping,
            ),
            (
                "vitis_hls",
                native_synthesis_results["vitis_hls"]["interface_signature"],
                vitis_mapping,
            ),
        ):
            wrapper_directory = output_root / "interface_normalization" / generator
            wrapper_directory.mkdir(parents=True)
            wrapper = wrapper_directory / "normalized_wrapper.v"
            write_interface_wrapper(
                wrapper,
                normalized_top,
                arguments.top,
                contract,
                signature,
                mapping,
            )
            wrappers[generator] = wrapper

        synthesis_results: dict[str, dict[str, Any]] = {}
        for generator, rtl_files in (
            ("light_hls", [light_rtl, wrappers["light_hls"]]),
            ("vitis_hls", vitis_rtl + [wrappers["vitis_hls"]]),
        ):
            synthesis_directory = output_root / "normalized_synthesis" / generator
            synthesis_directory.mkdir(parents=True)
            synthesis_tcl = synthesis_directory / "run_vivado.tcl"
            write_common_vivado_tcl(
                synthesis_tcl,
                rtl_files,
                normalized_top,
                arguments.clock_period_ns,
                synthesis_directory,
                implement=arguments.implement,
            )
            runtime = run_command(
                affinity_prefix()
                + [
                    str(vivado),
                    "-mode",
                    "batch",
                    "-nolog",
                    "-nojournal",
                    "-source",
                    str(synthesis_tcl),
                ],
                synthesis_directory,
                synthesis_directory / "vivado.log",
                arguments.timeout_seconds,
            )
            synthesis_results[generator] = parse_metrics(
                synthesis_directory / "metrics.txt",
                arguments.clock_period_ns,
                runtime,
            )
        interface_compatible = (
            synthesis_results["light_hls"]["interface_signature"]
            == synthesis_results["vitis_hls"]["interface_signature"]
        )
        functional: dict[str, Any] = {
            "requested": testbench is not None,
            "valid": None,
        }
        if testbench and iverilog:
            simulations: dict[str, dict[str, Any]] = {}
            for generator, rtl_files in (
                ("light_hls", [light_rtl]),
                ("vitis_hls", vitis_rtl),
            ):
                simulation_directory = output_root / "simulation" / generator
                simulation_directory.mkdir(parents=True)
                simulations[generator] = run_simulation(
                    iverilog,
                    rtl_files,
                    testbench,
                    arguments.testbench_top,
                    simulation_directory,
                    arguments.timeout_seconds,
                    arguments.testbench_define
                    + (
                        arguments.vitis_testbench_define
                        if generator == "vitis_hls"
                        else []
                    ),
                    arguments.rtl_simulator,
                )
            trace_match = (
                simulations["light_hls"]["trace"]
                == simulations["vitis_hls"]["trace"]
            )
            functional = {
                "requested": True,
                "valid": trace_match,
                "trace_match": trace_match,
                "light_hls": simulations["light_hls"],
                "vitis_hls": simulations["vitis_hls"],
            }

        report["native_interface_equivalence"] = {
            "valid": native_interface_compatible,
            "light_hls": native_synthesis_results["light_hls"][
                "interface_signature"
            ],
            "vitis_hls": native_synthesis_results["vitis_hls"][
                "interface_signature"
            ],
        }
        report["interface_normalization"] = {
            "valid": interface_compatible,
            "wrapper_top": normalized_top,
            "contract": [
                {
                    "name": name,
                    "direction": contract[name]["direction"],
                    "width": contract[name]["width"],
                }
                for name in sorted(contract)
            ],
            "vitis_aliases": aliases,
            "light_hls_wrapper": str(wrappers["light_hls"]),
            "vitis_hls_wrapper": str(wrappers["vitis_hls"]),
        }
        report["interface_equivalence"] = {
            "valid": interface_compatible,
            "light_hls": synthesis_results["light_hls"][
                "interface_signature"
            ],
            "vitis_hls": synthesis_results["vitis_hls"][
                "interface_signature"
            ],
        }
        report["functional_equivalence"] = functional
        report["results"] = {
            "light_hls": {
                "generator_runtime_seconds": round(
                    light_generation_runtime, 6
                ),
                "schedule": parse_light_schedule(
                    light_reports / "scheduling" / "schedule.json",
                    light_reports / "scheduling" / "pipeline_schedule.json",
                    light_reports / "scheduling" / "dataflow_graph.json",
                ),
                "native_post_rtl_synthesis": native_synthesis_results[
                    "light_hls"
                ],
                "post_rtl_synthesis": synthesis_results["light_hls"],
            },
            "vitis_hls": {
                "generator_runtime_seconds": round(
                    vitis_generation_runtime, 6
                ),
                "estimate": parse_vitis_estimate(vitis_csynth_xml),
                "native_post_rtl_synthesis": native_synthesis_results[
                    "vitis_hls"
                ],
                "post_rtl_synthesis": synthesis_results["vitis_hls"],
            },
        }
        report["deltas"] = make_deltas(
            synthesis_results["light_hls"], synthesis_results["vitis_hls"]
        )
        if arguments.implement:
            for generator in ("light_hls", "vitis_hls"):
                directory = output_root / "normalized_synthesis" / generator
                report["results"][generator]["post_route"] = parse_route_metrics(directory, arguments.clock_period_ns)
        report["valid"] = interface_compatible and functional.get("valid") is not False
        synthesis_timing_met = all(result["timing"]["clock_constrained"] and
            result["timing"]["worst_slack_ns"] is not None and result["timing"]["worst_slack_ns"] >= 0
            for result in synthesis_results.values())
        report["synthesis_timing_met"] = synthesis_timing_met
        report["reward_eligible"] = interface_compatible and functional.get(
            "valid"
        ) is True and synthesis_timing_met
        if arguments.implement:
            report["reward_eligible"] &= all(report["results"][generator]["post_route"]["timing_met"]
                                             for generator in ("light_hls", "vitis_hls"))
        if not interface_compatible:
            report["errors"].append(
                {
                    "code": "FAIR_COMPARISON_INTERFACE_MISMATCH",
                    "message": "The synthesized top-level port signatures differ.",
                }
            )
        if not native_interface_compatible:
            report.setdefault("warnings", []).append(
                {
                    "code": "FAIR_COMPARISON_NATIVE_INTERFACE_NORMALIZED",
                    "message": "Native interfaces differ; measured comparison uses generated common wrappers.",
                }
            )
        if functional.get("valid") is False:
            report["errors"].append(
                {
                    "code": "FAIR_COMPARISON_FUNCTIONAL_MISMATCH",
                    "message": "Light-HLS and Vitis HLS RTL RESULT traces differ.",
                }
            )
        if functional.get("valid") is None:
            report.setdefault("warnings", []).append(
                {
                    "code": "FAIR_COMPARISON_NOT_FUNCTIONALLY_CHECKED",
                    "message": "No common RTL testbench was supplied; this result is not reward-eligible.",
                }
            )

        for path, expected in ((source, report["source_sha256"]),
                               (testbench, report["testbench_sha256"]),
                               (light_hls_top, report["light_hls_executable_sha256"]),
                               (config, report["config_sha256"])):
            if path and sha256_file(path) != expected:
                raise ComparisonError(f"Input changed during comparison: {path}")

    except (ComparisonError, OSError, ET.ParseError, ValueError, KeyError) as error:
        report["valid"] = False
        report["reward_eligible"] = False
        report["errors"].append(
            {"code": "FAIR_COMPARISON_FAILED", "message": str(error)}
        )
        print(str(error), file=sys.stderr)
        if output_initialized:
            report_path.write_text(
                json.dumps(report, indent=2, sort_keys=False) + "\n",
                encoding="utf-8",
            )
            print(f"Failure report: {report_path}", file=sys.stderr)
        return 1

    report_path.write_text(
        json.dumps(report, indent=2, sort_keys=False) + "\n",
        encoding="utf-8",
    )
    print(f"U250 fair comparison: {report_path}")
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    sys.exit(main())
