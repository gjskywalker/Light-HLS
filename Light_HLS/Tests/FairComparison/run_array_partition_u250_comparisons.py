#!/usr/bin/env python3

"""Run array-partition App cases through the common U250 RTL flow."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from run_u250_fair_comparison import FLOW_ID


@dataclass(frozen=True)
class PartitionCase:
    name: str
    kind: str
    source: str
    top: str
    testbench: str
    testbench_top: str
    vitis_defines: tuple[str, ...] = ()


CASES = (
    PartitionCase(
        "cyclic", "cyclic + pipeline", "backend_array_partition_cyclic.cpp",
        "backend_array_partition_cyclic",
        "backend_array_partition_cyclic_tb.v",
        "backend_array_partition_cyclic_tb",
    ),
    PartitionCase(
        "block", "block + pipeline", "backend_array_partition_block.cpp",
        "backend_array_partition_block", "backend_array_partition_block_tb.v",
        "backend_array_partition_block_tb",
    ),
    PartitionCase(
        "complete", "complete", "backend_array_partition_complete.cpp",
        "backend_array_partition_complete",
        "backend_array_partition_complete_tb.v",
        "backend_array_partition_complete_tb",
        ("VITIS_NO_CLOCK",),
    ),
    PartitionCase(
        "pipeline_complete", "complete + pipeline",
        "backend_array_partition_pipeline_complete.cpp",
        "backend_array_partition_pipeline_complete",
        "backend_array_partition_pipeline_complete_tb.v",
        "backend_array_partition_pipeline_complete_tb",
    ),
    PartitionCase(
        "complete_rw", "complete read/write",
        "backend_array_partition_complete_rw.cpp",
        "backend_array_partition_complete_rw",
        "backend_array_partition_complete_rw_tb.v",
        "backend_array_partition_complete_rw_tb",
    ),
    PartitionCase(
        "multidim", "multidimensional complete + pipeline",
        "backend_array_partition_multidim.cpp",
        "backend_array_partition_multidim",
        "backend_array_partition_multidim_tb.v",
        "backend_array_partition_multidim_tb",
        ("VITIS_MULTIDIM_PORTS",),
    ),
    PartitionCase(
        "multidim_cyclic", "multidimensional cyclic + pipeline",
        "backend_array_partition_multidim_cyclic.cpp",
        "backend_array_partition_multidim_cyclic",
        "backend_array_partition_multidim_cyclic_tb.v",
        "backend_array_partition_multidim_cyclic_tb",
    ),
    PartitionCase(
        "local", "local cyclic", "backend_array_partition_local.cpp",
        "backend_array_partition_local", "backend_array_partition_local_tb.v",
        "backend_array_partition_local_tb",
    ),
)


class BatchError(RuntimeError):
    pass


def executable(value: str, description: str) -> Path:
    candidate = shutil.which(value)
    if not candidate:
        raise BatchError(f"Cannot find {description}: {value}")
    return Path(candidate).resolve()


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--light-hls-top", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--clock-period-ns", type=float, default=10.0)
    parser.add_argument("--timeout-seconds", type=int, default=900)
    parser.add_argument(
        "--resume", action="store_true",
        help="Reuse completed cases and preserve incomplete case directories",
    )
    parser.add_argument(
        "--case", action="append", choices=[case.name for case in CASES],
        help="Run only the selected case; may be repeated",
    )
    return parser.parse_args()


def first_pipeline(report: dict[str, Any]) -> dict[str, Any]:
    pipelines = report["results"]["light_hls"]["schedule"].get(
        "pipelines", []
    )
    return pipelines[0] if pipelines else {}


def summarize(case: PartitionCase, report: dict[str, Any]) -> dict[str, Any]:
    functional = report["functional_equivalence"]
    light = report["results"]["light_hls"]
    vitis = report["results"]["vitis_hls"]
    return {
        "case": case.name,
        "partition": case.kind,
        "top": case.top,
        "source": str(report["source"]),
        "comparison_id": report.get("comparison_id"),
        "source_sha256": report.get("source_sha256"),
        "config_sha256": report.get("config_sha256"),
        "testbench_sha256": report.get("testbench_sha256"),
        "light_hls_executable_sha256": report.get(
            "light_hls_executable_sha256"
        ),
        "input_fingerprints": report.get("input_fingerprints", {}),
        "functional_equivalence": functional.get("valid") is True,
        "exact_interface_equivalence": report[
            "native_interface_equivalence"
        ]["valid"],
        "normalized_interface_equivalence": report[
            "interface_equivalence"
        ]["valid"],
        "reward_eligible": report["reward_eligible"],
        "light_hls": {
            "pipeline": first_pipeline(report),
            "schedule": light["schedule"],
            "post_rtl_synthesis": light["post_rtl_synthesis"],
        },
        "vitis_hls": {
            "estimate": vitis["estimate"],
            "post_rtl_synthesis": vitis["post_rtl_synthesis"],
        },
        "native_interface_difference": report[
            "native_interface_equivalence"
        ],
        "interface_normalization": report["interface_normalization"],
        "simulation_cycles": {
            "light_hls": functional["light_hls"]["mean_transaction_cycles"],
            "vitis_hls": functional["vitis_hls"]["mean_transaction_cycles"],
        },
        "result_trace_count": len(functional["light_hls"]["trace"]),
        "deltas": report["deltas"],
    }


def value_text(value: Any) -> str:
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.3f}"
    return str(value)


def vitis_iis(summary: dict[str, Any]) -> str:
    values = [
        loop["achieved_ii"]
        for loop in summary["vitis_hls"]["estimate"].get("loops", [])
        if loop.get("achieved_ii") is not None
    ]
    return ",".join(str(value) for value in values) or "-"


def write_human_report(path: Path, summaries: list[dict[str, Any]]) -> None:
    headings = (
        "Case", "L-II", "V-II", "L-Cycles", "V-Cycles", "L-LUT",
        "V-LUT", "L-FF", "V-FF", "L-WNS", "V-WNS", "Func", "Native IF",
        "Nrm IF",
    )
    widths = (20, 6, 6, 9, 9, 7, 7, 7, 7, 8, 8, 6, 9, 6)

    def row(values: tuple[Any, ...]) -> str:
        return "| " + " | ".join(
            value_text(value).ljust(width)
            for value, width in zip(values, widths)
        ) + " |"

    separator = "+-" + "-+-".join("-" * width for width in widths) + "-+"
    lines = [
        "Light-HLS/Vitis HLS Array-Partition Comparison",
        "Board: U250 (xcu250-figd2104-2L-e)",
        "Flow: common Vivado out-of-context synthesis",
        "",
        separator,
        row(headings),
        separator,
    ]
    for summary in summaries:
        light_synth = summary["light_hls"]["post_rtl_synthesis"]
        vitis_synth = summary["vitis_hls"]["post_rtl_synthesis"]
        lines.append(row((
            summary["case"],
            summary["light_hls"]["pipeline"].get("achieved_ii"),
            vitis_iis(summary),
            summary["simulation_cycles"]["light_hls"],
            summary["simulation_cycles"]["vitis_hls"],
            light_synth["resources"]["lut"],
            vitis_synth["resources"]["lut"],
            light_synth["resources"]["ff"],
            vitis_synth["resources"]["ff"],
            light_synth["timing"]["worst_slack_ns"],
            vitis_synth["timing"]["worst_slack_ns"],
            "yes" if summary["functional_equivalence"] else "no",
            "yes" if summary["exact_interface_equivalence"] else "no",
            "yes" if summary["normalized_interface_equivalence"] else "no",
        )))
    lines.extend([
        separator,
        "",
        "Cycle counts use the same self-checking RTL testbench.",
        "LUT, FF, and WNS use the same Vivado OOC flow and U250 part.",
        "Reward eligibility requires functional and normalized-interface checks.",
        "Native IF is retained separately to expose compiler interface differences.",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    arguments = parse_arguments()
    script_directory = Path(__file__).resolve().parent
    app_directory = script_directory.parent.parent / "App" / "backend_array_partition"
    comparison_script = script_directory / "run_u250_fair_comparison.py"
    light_hls_top = executable(arguments.light_hls_top, "Light_HLS_Top")
    output_root = Path(arguments.output).expanduser().resolve()
    if arguments.clock_period_ns <= 0 or arguments.timeout_seconds <= 0:
        raise BatchError("Clock period and timeout must be positive")
    if output_root.exists() and any(output_root.iterdir()) and not arguments.resume:
        raise BatchError(f"Output directory must be absent or empty: {output_root}")
    output_root.mkdir(parents=True, exist_ok=True)

    selected_names = set(arguments.case or ())
    selected_cases = [
        case for case in CASES
        if not selected_names or case.name in selected_names
    ]
    summaries: list[dict[str, Any]] = []
    for case in selected_cases:
        case_output = output_root / case.name
        report_path = case_output / "u250_fair_comparison.json"
        if arguments.resume and report_path.is_file():
            report = json.loads(report_path.read_text(encoding="utf-8"))
            if (
                report.get("valid") is True
                and report.get("reward_eligible") is True
            ):
                summaries.append(summarize(case, report))
                print(f"[{case.name}] reusing completed comparison", flush=True)
                continue
        if arguments.resume and case_output.exists():
            backup_index = 1
            backup = output_root / f"{case.name}.failed.{backup_index}"
            while backup.exists():
                backup_index += 1
                backup = output_root / f"{case.name}.failed.{backup_index}"
            case_output.rename(backup)
            print(f"[{case.name}] preserved incomplete run at {backup}", flush=True)

        command = [
            sys.executable, str(comparison_script), "--light-hls-top",
            str(light_hls_top), "--source", str(app_directory / case.source),
            "--top", case.top, "--testbench", str(app_directory / case.testbench),
            "--testbench-top", case.testbench_top, "--clock-period-ns",
            str(arguments.clock_period_ns), "--timeout-seconds",
            str(arguments.timeout_seconds), "--output", str(case_output),
        ]
        for define in case.vitis_defines:
            command.extend(("--vitis-testbench-define", define))
        print(f"[{case.name}] running Light-HLS, Vitis HLS, and Vivado", flush=True)
        log_path = output_root / f"{case.name}.log"
        with log_path.open("w", encoding="utf-8") as log:
            completed = subprocess.run(
                command, cwd=script_directory.parent.parent.parent,
                stdout=log, stderr=subprocess.STDOUT, text=True, check=False,
            )
        if not report_path.is_file():
            raise BatchError(f"Comparison did not produce {report_path}")
        report = json.loads(report_path.read_text(encoding="utf-8"))
        failure_codes = {
            error.get("code") for error in report.get("errors", [])
            if error.get("code") != "FAIR_COMPARISON_INTERFACE_MISMATCH"
        }
        if completed.returncode not in (0, 1) or failure_codes:
            raise BatchError(
                f"Case {case.name} failed; see {log_path}: "
                + ", ".join(sorted(failure_codes))
            )
        summary = summarize(case, report)
        if not summary["functional_equivalence"]:
            raise BatchError(f"Case {case.name} failed functional comparison")
        summaries.append(summary)
        print(
            f"[{case.name}] functionally matched; native interface="
            f"{summary['exact_interface_equivalence']}; normalized interface="
            f"{summary['normalized_interface_equivalence']}", flush=True,
        )

    aggregate = {
        "schema_version": "1.0",
        "stage": "u250_array_partition_fair_comparison_batch",
        "valid": all(
            item["functional_equivalence"] and
            item["normalized_interface_equivalence"]
            for item in summaries
        ),
        "functional_equivalence": all(
            item["functional_equivalence"] for item in summaries
        ),
        "exact_interface_equivalence": all(
            item["exact_interface_equivalence"] for item in summaries
        ),
        "normalized_interface_equivalence": all(
            item["normalized_interface_equivalence"] for item in summaries
        ),
        "reward_eligible": all(item["reward_eligible"] for item in summaries),
        "board": "u250",
        "part": "xcu250-figd2104-2L-e",
        "clock_period_ns": arguments.clock_period_ns,
        "flow": FLOW_ID,
        "cases": summaries,
    }
    json_path = output_root / "u250_array_partition_comparison.json"
    human_path = output_root / "u250_array_partition_comparison.rpt"
    json_path.write_text(
        json.dumps(aggregate, indent=2, sort_keys=False) + "\n",
        encoding="utf-8",
    )
    write_human_report(human_path, summaries)
    print(f"Array-partition comparison JSON: {json_path}")
    print(f"Array-partition comparison report: {human_path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (BatchError, OSError, json.JSONDecodeError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
