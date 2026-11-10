#!/usr/bin/env python3
"""Build a LightHLS instruction library from Vitis HLS reports."""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import datetime as dt
import json
import pathlib
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from typing import Iterable


DEFAULT_CLOCKS = ("3", "4", "5", "6", "7", "8", "9", "10",
                  "12.5", "15", "16", "17.5", "20", "25", "30")
DEFAULT_WIDTHS = (8, 16, 32, 64)
PSEUDO_OPERATIONS = ("br", "getelementptr", "ret")


@dataclasses.dataclass(frozen=True)
class Probe:
    operation: str
    width: int | None
    function: str
    definition: str
    call: str
    timing_operations: tuple[str, ...]
    resource_operations: tuple[str, ...]
    optional: bool = False
    zero_resource: bool = False
    aggregate_resource: bool = False
    use_core_delay: bool = False

    @property
    def key_width(self) -> int:
        return -1 if self.width is None else self.width


@dataclasses.dataclass(frozen=True)
class Result:
    operation: str
    width: int
    clock: str
    dsp: int
    ff: int
    lut: int
    latency: int
    delay: float
    ii: int
    core: str

    def row(self) -> str:
        delay = f"{self.delay:.6f}".rstrip("0").rstrip(".")
        # LightHLS uses -2 to defer load-cycle placement to its BRAM scheduler;
        # stores are placed in the current state. The delay remains board data.
        latency = {"load": -2, "store": 0}.get(
            self.operation, self.latency)
        return (
            f"{self.width} {self.width} {self.width} {self.clock} "
            f"{self.dsp} {self.ff} {self.lut} {latency} "
            f"{delay or '0'} {self.ii} {self.core}"
        )


def parse_csv(value: str) -> tuple[str, ...]:
    return tuple(item.strip() for item in value.split(",") if item.strip())


def integer_probe(operation: str, width: int, signed: bool,
                  expression: str) -> Probe:
    value_type = f"ap_{'int' if signed else 'uint'}<{width}>"
    function = f"probe_{operation}_{width}"
    definition = f"""
{value_type} {function}({value_type} a, {value_type} b, {value_type} c) {{
#pragma HLS inline off
    return {expression};
}}
"""
    arguments = "sa, sb, sc" if signed else "ua, ub, uc"
    timing_operations = (operation,)
    resource_operations = (operation,)
    optional = False
    if operation == "mac":
        timing_operations = ("mul",)
        resource_operations = ("muladd",)
        optional = True
    elif operation == "tadd":
        timing_operations = ("tadd", "add")
        resource_operations = ("tadd", "add")
    return Probe(
        operation=operation,
        width=width,
        function=function,
        definition=definition,
        call=f"{function}({arguments})",
        timing_operations=timing_operations,
        resource_operations=resource_operations,
        optional=optional,
    )


def conversion_probe(operation: str, width: int) -> Probe:
    function = f"probe_{operation}_{width}"
    signed_types = {
        8: "signed char",
        16: "short",
        32: "int",
        64: "long long",
    }
    unsigned_types = {
        8: "unsigned char",
        16: "unsigned short",
        32: "unsigned int",
        64: "unsigned long long",
    }
    if width not in signed_types:
        raise ValueError(
            f"conversion probes require a native C integer width, got {width}")
    if operation == "fptosi":
        return_type, argument_type, expression, argument = (
            signed_types[width], "float", "a", "fa")
    elif operation == "fptoui":
        return_type, argument_type, expression, argument = (
            unsigned_types[width], "float", "a", "fa")
    elif operation == "sitofp":
        return_type, argument_type, expression, argument = (
            "float", signed_types[width], "a", "sa")
    elif operation == "uitofp":
        return_type, argument_type, expression, argument = (
            "float", unsigned_types[width], "a", "ua")
    else:
        raise ValueError(f"unsupported conversion operation: {operation}")
    definition = f"""
{return_type} {function}({argument_type} a) {{
#pragma HLS inline off
    return {expression};
}}
"""
    return Probe(
        operation=operation,
        width=width,
        function=function,
        definition=definition,
        call=f"{function}({argument})",
        timing_operations=(operation,),
        resource_operations=(operation,),
        aggregate_resource=True,
    )


def floating_probe(operation: str, value_type: str, expression: str) -> Probe:
    function = f"probe_{operation}"
    return_type = "bool" if operation.endswith("cmp") else value_type
    definition = f"""
{return_type} {function}({value_type} a, {value_type} b) {{
#pragma HLS inline off
    return {expression};
}}
"""
    prefix = "f" if value_type == "float" else "d"
    timing = (operation, "fcmp") if operation.endswith("cmp") else (operation,)
    return Probe(
        operation=operation,
        width=None,
        function=function,
        definition=definition,
        call=f"{function}({prefix}a, {prefix}b)",
        timing_operations=timing,
        resource_operations=timing,
        aggregate_resource=True,
    )


def double_multiply_add_probe() -> Probe:
    return Probe(
        operation="dmuladd",
        width=None,
        function="probe_dmuladd",
        definition="""
double probe_dmuladd(double a, double b, double c) {
#pragma HLS inline off
#pragma HLS pipeline II=1
    return hls::fma(a, b, c);
}
""",
        call="probe_dmuladd(da, db, da)",
        timing_operations=("dmuladd", "fma"),
        resource_operations=("dmuladd", "fma"),
        aggregate_resource=True,
    )


def build_probes(widths: Iterable[int]) -> list[Probe]:
    probes: list[Probe] = []
    integer_specs = (
        ("add", True, "a + b"),
        ("sub", True, "a - b"),
        ("mul", True, "a * b"),
        ("and", False, "a & b"),
        ("or", False, "a | b"),
        ("xor", False, "a ^ b"),
        ("shl", False, "a << b"),
        ("lshr", False, "a >> b"),
        ("ashr", True, "a >> b"),
        ("sdiv", True, "a / b"),
        ("udiv", False, "a / b"),
        ("srem", True, "a % b"),
        ("urem", False, "a % b"),
        ("icmp", True, "a < b"),
        ("mac", True, "a * b + c"),
        ("tadd", True, "a + b + c"),
    )
    for width in widths:
        for operation, signed, expression in integer_specs:
            probes.append(integer_probe(operation, width, signed, expression))
        for operation in ("fptosi", "fptoui", "sitofp", "uitofp"):
            probes.append(conversion_probe(operation, width))

    for operation, value_type, expression in (
        ("fadd", "float", "a + b"),
        ("fsub", "float", "a - b"),
        ("fmul", "float", "a * b"),
        ("fdiv", "float", "a / b"),
        ("fcmp", "float", "a < b"),
        ("dadd", "double", "a + b"),
        ("dsub", "double", "a - b"),
        ("dmul", "double", "a * b"),
        ("ddiv", "double", "a / b"),
        ("dcmp", "double", "a < b"),
    ):
        probes.append(floating_probe(operation, value_type, expression))
    probes.append(double_multiply_add_probe())

    probes.extend((
        Probe(
            operation="load",
            width=None,
            function="probe_load",
            definition="""
ap_int<32> probe_load(ap_int<32> memory[16], ap_uint<4> index) {
#pragma HLS inline off
    return memory[index];
}
""",
            call="probe_load(memory, index)",
            timing_operations=("load",),
            resource_operations=(),
            zero_resource=True,
            use_core_delay=True,
        ),
        Probe(
            operation="store",
            width=None,
            function="probe_store",
            definition="""
void probe_store(ap_int<32> memory[16], ap_uint<4> index,
                  ap_int<32> value) {
#pragma HLS inline off
    memory[index] = value;
}
""",
            call="probe_store(memory, index, sa)",
            timing_operations=("store",),
            resource_operations=(),
            zero_resource=True,
            use_core_delay=True,
        ),
    ))
    return probes


def render_source(probes: list[Probe]) -> str:
    definitions = "\n".join(probe.definition.strip() for probe in probes)
    calls: list[str] = []
    output_index = 0
    for probe in probes:
        if probe.operation == "store":
            calls.append(f"    {probe.call};")
            continue
        calls.append(
            f"    out[{output_index}] = static_cast<ap_uint<64>>({probe.call});")
        output_index += 1
    calls_text = "\n".join(calls)
    return f"""#include <ap_int.h>
#include <hls_math.h>

{definitions}

void top(ap_int<64> sa, ap_int<64> sb, ap_int<64> sc,
         ap_uint<64> ua, ap_uint<64> ub, ap_uint<64> uc,
         float fa, float fb, double da, double db,
         ap_int<32> memory[16], ap_uint<4> index,
         ap_uint<64> out[{output_index}]) {{
{calls_text}
}}
"""


def render_tcl(part: str, clock: str) -> str:
    return f"""open_project -reset characterization
set_top top
add_files probes.cpp
open_solution -reset solution1
set_part {{{part}}}
create_clock -period {clock} -name default
csynth_design
exit
"""


def report_path(clock_dir: pathlib.Path, probe: Probe) -> pathlib.Path:
    return (
        clock_dir / "characterization" / "solution1" / ".autopilot" /
        "db" / f"{probe.function}.verbose.rpt"
    )


def summary_path(clock_dir: pathlib.Path, probe: Probe) -> pathlib.Path:
    return (
        clock_dir / "characterization" / "solution1" / "syn" / "report" /
        f"{probe.function}_csynth.xml"
    )


def parse_summary(path: pathlib.Path) -> tuple[int, float, int, int, int, int]:
    root = ET.parse(path).getroot()

    def integer(path_name: str) -> int:
        value = root.findtext(path_name)
        if value is None:
            raise RuntimeError(f"missing {path_name} in {path}")
        return int(value)

    def floating(path_name: str) -> float:
        value = root.findtext(path_name)
        if value is None:
            raise RuntimeError(f"missing {path_name} in {path}")
        return float(value)

    return (
        integer("./PerformanceEstimates/SummaryOfOverallLatency/Best-caseLatency"),
        floating("./PerformanceEstimates/SummaryOfTimingAnalysis/EstimatedClockPeriod"),
        max(1, integer(
            "./PerformanceEstimates/SummaryOfOverallLatency/Interval-min")),
        integer("./AreaEstimates/Resources/DSP"),
        integer("./AreaEstimates/Resources/FF"),
        integer("./AreaEstimates/Resources/LUT"),
    )


def parse_timing(report: str, operations: tuple[str, ...],
                 use_core_delay: bool = False) -> tuple[int, float, int, str] | None:
    candidates: list[tuple[int, float, int, str]] = []
    for operation in operations:
        marker = re.compile(rf"Operation \d+ '{re.escape(operation)}'")
        for line in report.splitlines():
            if not marker.search(line) or "<CoreInst" not in line:
                continue
            operation_delay = re.search(r"<Delay\s*=\s*([0-9.]+)>", line)
            core = re.search(
                r"<CoreInst\s*=\s*\"([^\"]+)\">.*?"
                r"<Latency\s*=\s*(-?\d+)>.*?"
                r"<II\s*=\s*(-?\d+)>.*?"
                r"<Delay\s*=\s*([0-9.]+)>",
                line,
            )
            if operation_delay and core:
                candidates.append((
                    int(core.group(2)),
                    float(core.group(4) if use_core_delay
                          else operation_delay.group(1)),
                    int(core.group(3)),
                    core.group(1),
                ))
    if not candidates:
        return None
    return max(candidates, key=lambda item: item[1])


def parse_resource_rows(report: str) -> dict[str, tuple[int, int, int]]:
    lines = report.splitlines()
    header: list[str] | None = None
    rows: dict[str, list[int]] = {}
    in_table = False
    current_operation = ""
    for line in lines:
        if line.strip() == "* Functional unit list:":
            in_table = True
            continue
        if not in_table:
            continue
        if line.startswith("Memories:"):
            break
        if "|" not in line:
            continue
        cells = [cell.strip() for cell in line.split("|")[1:-1]]
        if not cells or all(not cell or set(cell) == {"-"} for cell in cells):
            continue
        if "Operation" in cells and "Functional Unit" in cells:
            header = cells
            continue
        if header is None or len(cells) != len(header):
            continue
        operation = cells[header.index("Operation")]
        if operation:
            current_operation = operation
        if not current_operation or current_operation == "Total":
            continue
        values = []
        valid = True
        for resource in ("DSP", "FF", "LUT"):
            if resource not in header:
                values.append(0)
                continue
            raw_value = cells[header.index(resource)]
            try:
                values.append(int(raw_value or "0"))
            except ValueError:
                valid = False
                break
        if valid:
            aggregate = rows.setdefault(current_operation, [0, 0, 0])
            for index, value in enumerate(values):
                aggregate[index] += value
    return {name: tuple(values) for name, values in rows.items()}


def parse_probe(clock_dir: pathlib.Path, clock: str,
                probe: Probe) -> Result | None:
    path = report_path(clock_dir, probe)
    if not path.is_file():
        if probe.optional:
            return None
        raise RuntimeError(f"missing report for {probe.function}: {path}")
    report = path.read_text(encoding="utf-8", errors="replace")
    timing = parse_timing(
        report, probe.timing_operations, probe.use_core_delay)
    summary = None
    if timing is None or probe.aggregate_resource:
        summary_file = summary_path(clock_dir, probe)
        if not summary_file.is_file():
            raise RuntimeError(
                f"missing synthesis summary for {probe.function}: {summary_file}")
        summary = parse_summary(summary_file)
    if timing is None:
        if summary is None:
            if probe.optional:
                return None
            raise RuntimeError(
                f"cannot find timing for {probe.function} in {path}")
        timing = (summary[0], summary[1], summary[2], "---")

    if probe.zero_resource:
        resource = (0, 0, 0)
    elif probe.aggregate_resource:
        assert summary is not None
        resource = summary[3:6]
    else:
        resource_rows = parse_resource_rows(report)
        matches = [
            resource_rows[operation]
            for operation in probe.resource_operations
            if operation in resource_rows
        ]
        if not matches:
            if probe.optional:
                return None
            raise RuntimeError(
                f"cannot find resource row {probe.resource_operations} "
                f"for {probe.function} in {path}")
        resource = tuple(sum(values[index] for values in matches)
                         for index in range(3))

    latency, delay, ii, core = timing
    return Result(
        operation=probe.operation,
        width=probe.key_width,
        clock=clock,
        dsp=resource[0],
        ff=resource[1],
        lut=resource[2],
        latency=latency,
        delay=delay,
        ii=ii,
        core=core.replace(" ", "_"),
    )


def result_json(result: Result) -> dict[str, object]:
    return dataclasses.asdict(result)


def result_from_json(data: dict[str, object]) -> Result:
    return Result(
        operation=str(data["operation"]),
        width=int(data["width"]),
        clock=str(data["clock"]),
        dsp=int(data["dsp"]),
        ff=int(data["ff"]),
        lut=int(data["lut"]),
        latency=int(data["latency"]),
        delay=float(data["delay"]),
        ii=int(data["ii"]),
        core=str(data["core"]),
    )


def run_clock(clock: str, work_dir: pathlib.Path, source: str,
              probes: list[Probe], vitis_run: str, part: str,
              resume: bool) -> list[Result]:
    clock_name = clock.replace(".", "_")
    clock_dir = work_dir / f"clock_{clock_name}"
    result_file = clock_dir / "results.json"
    if resume and result_file.is_file():
        data = json.loads(result_file.read_text(encoding="utf-8"))
        return [result_from_json(item) for item in data]

    reports_dir = (
        clock_dir / "characterization" / "solution1" / ".autopilot" / "db")
    if not (resume and reports_dir.is_dir()):
        if clock_dir.exists():
            shutil.rmtree(clock_dir)
        clock_dir.mkdir(parents=True)
        (clock_dir / "probes.cpp").write_text(source, encoding="utf-8")
        (clock_dir / "run.tcl").write_text(render_tcl(part, clock), encoding="utf-8")
        log_path = clock_dir / "vitis_hls.log"
        with log_path.open("w", encoding="utf-8") as log:
            completed = subprocess.run(
                [vitis_run, "--mode", "hls", "--tcl", "run.tcl"],
                cwd=clock_dir,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )
        if completed.returncode != 0:
            raise RuntimeError(
                f"Vitis HLS failed for clock {clock}; see {log_path}")

    results = []
    for probe in probes:
        result = parse_probe(clock_dir, clock, probe)
        if result is not None:
            results.append(result)
    result_file.write_text(
        json.dumps([result_json(result) for result in results],
                   indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return results


def write_library(output_dir: pathlib.Path, board: str, part: str,
                  clocks: tuple[str, ...], widths: tuple[int, ...],
                  results: list[Result], tool_version: str) -> None:
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)

    grouped: dict[str, list[Result]] = {}
    for result in results:
        grouped.setdefault(result.operation, []).append(result)
    for operation in PSEUDO_OPERATIONS:
        grouped[operation] = [
            Result(operation, -1, clock, 0, 0, 0, 0, 0.0, 1, "---")
            for clock in clocks
        ]

    required_operations = {
        "add", "and", "ashr", "br", "dadd", "dcmp", "ddiv", "dmul",
        "dmuladd", "dsub", "fadd", "fcmp", "fdiv", "fmul", "fptosi", "fptoui",
        "fsub", "getelementptr", "icmp", "load", "lshr", "mac", "mul",
        "or", "ret", "sdiv", "shl", "sitofp", "srem", "store", "sub",
        "tadd", "udiv", "uitofp", "urem", "xor",
    }
    missing = sorted(required_operations - grouped.keys())
    if missing:
        raise RuntimeError(f"generated library is missing operations: {missing}")

    clock_order = {clock: index for index, clock in enumerate(clocks)}
    for operation in sorted(required_operations):
        operation_dir = output_dir / operation
        operation_dir.mkdir()
        operation_results = sorted(
            grouped[operation],
            key=lambda item: (item.width, clock_order[item.clock]),
        )
        (operation_dir / operation).write_text(
            "\n".join(result.row() for result in operation_results) + "\n",
            encoding="utf-8",
        )

    manifest = {
        "schema_version": 1,
        "board": board,
        "part": part,
        "tool": "Vitis HLS",
        "tool_version": tool_version,
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "clock_periods_ns": list(clocks),
        "integer_widths": list(widths),
        "notes": [
            "Each row was extracted from a Vitis HLS C-synthesis verbose report.",
            "MAC rows are emitted only when Vitis binds the expression to a muladd core.",
            "DMulAdd uses a pipelined hls::fma probe with target II=1.",
            "Load/store rows retain LightHLS scheduler latency sentinels while using the measured RAM delay.",
            "LightHLS uses its existing nearest-width fallback for other integer widths.",
        ],
    }
    (output_dir / "board.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def detect_version(vitis_run: str) -> str:
    completed = subprocess.run(
        [vitis_run, "--version"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    match = re.search(r"v(\d+\.\d+)", completed.stdout)
    return match.group(1) if match else "unknown"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", required=True)
    parser.add_argument("--part", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--work-dir", type=pathlib.Path, required=True)
    parser.add_argument("--clocks", default=",".join(DEFAULT_CLOCKS))
    parser.add_argument("--widths", default=",".join(map(str, DEFAULT_WIDTHS)))
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument(
        "--vitis-run",
        default=shutil.which("vitis-run") or
        "/vitis/2025.2/Vitis/bin/vitis-run",
    )
    args = parser.parse_args()

    clocks = parse_csv(args.clocks)
    widths = tuple(int(value) for value in parse_csv(args.widths))
    if not clocks or not widths:
        parser.error("at least one clock and integer width are required")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if not pathlib.Path(args.vitis_run).is_file():
        parser.error(f"cannot find vitis-run: {args.vitis_run}")

    probes = build_probes(widths)
    source = render_source(probes)
    args.work_dir.mkdir(parents=True, exist_ok=True)
    all_results: list[Result] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(
                run_clock, clock, args.work_dir, source, probes,
                args.vitis_run, args.part, args.resume
            ): clock
            for clock in clocks
        }
        for future in concurrent.futures.as_completed(futures):
            clock = futures[future]
            try:
                clock_results = future.result()
            except Exception as error:
                print(f"[{clock} ns] {error}", file=sys.stderr)
                return 1
            all_results.extend(clock_results)
            print(f"[{clock} ns] extracted {len(clock_results)} rows")

    write_library(
        args.output, args.board, args.part, clocks, widths, all_results,
        detect_version(args.vitis_run),
    )
    print(f"Wrote {len(all_results)} characterized rows to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
