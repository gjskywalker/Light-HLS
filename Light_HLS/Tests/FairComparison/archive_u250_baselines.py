#!/usr/bin/env python3

"""Create a compact, machine-readable archive from raw U250 comparisons."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


class ArchiveError(RuntimeError):
    pass


def portable_path(value: Any) -> Any:
    """Keep repository paths useful without embedding one checkout location."""
    if not isinstance(value, str):
        return value
    marker = "/Light_HLS/"
    normalized = value.replace("\\", "/")
    if marker in normalized:
        return "Light_HLS/" + normalized.split(marker, 1)[1]
    return Path(normalized).name


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input",
        action="append",
        required=True,
        metavar="SUITE=REPORT.json",
        help="Comparison report to archive; may be repeated",
    )
    parser.add_argument("--output", required=True)
    return parser.parse_args()


def parse_input(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise ArchiveError(f"Expected SUITE=REPORT.json, received: {value}")
    suite, path_text = value.split("=", 1)
    if not suite or not path_text:
        raise ArchiveError(f"Expected SUITE=REPORT.json, received: {value}")
    path = Path(path_text).expanduser().resolve()
    if not path.is_file():
        raise ArchiveError(f"Comparison report does not exist: {path}")
    return suite, path


def first_pipeline_ii(schedule: dict[str, Any]) -> Any:
    pipelines = schedule.get("pipelines", [])
    if pipelines:
        return pipelines[0].get("achieved_ii")
    return schedule.get("achieved_ii")


def vitis_iis(estimate: dict[str, Any]) -> list[int]:
    return [
        loop["achieved_ii"]
        for loop in estimate.get("loops", [])
        if loop.get("achieved_ii") is not None
    ]


def compact_qor(result: dict[str, Any]) -> dict[str, Any]:
    return {
        "tool_version": result.get("tool_version", ""),
        "flow_id": result.get("flow_id"),
        "runtime_seconds": result.get("runtime_seconds"),
        "resources": result.get("resources", {}),
        "timing": result.get("timing", {}),
    }


def normalize_single(suite: str, report: dict[str, Any]) -> dict[str, Any]:
    functional = report.get("functional_equivalence", {})
    light = report.get("results", {}).get("light_hls", {})
    vitis = report.get("results", {}).get("vitis_hls", {})
    schedule = light.get("schedule", {})
    dataflow = schedule.get("dataflow", {})
    return {
        "suite": suite,
        "case": report.get("top_function", suite),
        "top": report.get("top_function"),
        "comparison_id": report.get("comparison_id"),
        "source": portable_path(report.get("source")),
        "source_sha256": report.get("source_sha256"),
        "config_sha256": report.get("config_sha256"),
        "testbench_sha256": report.get("testbench_sha256"),
        "light_hls_executable_sha256": report.get(
            "light_hls_executable_sha256"
        ),
        "valid": report.get("valid") is True,
        "reward_eligible": report.get("reward_eligible") is True,
        "functional_equivalence": functional.get("valid") is True,
        "native_interface_equivalence": report.get(
            "native_interface_equivalence", {}
        ).get("valid") is True,
        "normalized_interface_equivalence": report.get(
            "interface_equivalence", {}
        ).get("valid") is True,
        "trace_count": len(functional.get("light_hls", {}).get("trace", [])),
        "simulation_cycles": {
            "light_hls": functional.get("light_hls", {}).get(
                "mean_transaction_cycles"
            ),
            "vitis_hls": functional.get("vitis_hls", {}).get(
                "mean_transaction_cycles"
            ),
        },
        "simulation_events": {
            generator: {key: functional.get(generator, {}).get(key, [])
                        for key in ("transaction_events", "final_commit_cycles",
                                    "completion_signal_cycles", "observed_acceptance_intervals")}
            for generator in ("light_hls", "vitis_hls")
        },
        "initiation_interval": {
            "light_hls": first_pipeline_ii(schedule),
            "light_hls_loops": [
                {key: pipeline.get(key) for key in ("loop", "achieved_ii", "limiting_factor",
                                                   "pipeline_latency", "memory_mii", "recurrence_mii")}
                for pipeline in schedule.get("pipelines", [])
            ],
            "light_hls_dataflow_graph": dataflow.get("graph_ii"),
            "vitis_hls_loops": vitis_iis(vitis.get("estimate", {})),
            "vitis_hls_top": vitis.get("estimate", {}).get(
                "interval_cycles", {}
            ),
        },
        "light_hls": compact_qor(light.get("post_rtl_synthesis", {})),
        "vitis_hls": compact_qor(vitis.get("post_rtl_synthesis", {})),
        "post_route": {generator: report.get("results", {}).get(generator, {}).get("post_route")
                       for generator in ("light_hls", "vitis_hls")},
        "deltas": report.get("deltas", {}),
    }


def normalize_batch_case(suite: str, case: dict[str, Any]) -> dict[str, Any]:
    light = case.get("light_hls", {})
    vitis = case.get("vitis_hls", {})
    schedule = light.get("schedule", {})
    pipeline = light.get("pipeline", {})
    dataflow = light.get("dataflow", schedule.get("dataflow", {}))
    light_ii = pipeline.get("achieved_ii")
    if light_ii is None:
        light_ii = first_pipeline_ii(schedule)
    return {
        "suite": suite,
        "case": case.get("case"),
        "top": case.get("top"),
        "comparison_id": case.get("comparison_id"),
        "source": portable_path(case.get("source")),
        "source_sha256": case.get("source_sha256"),
        "config_sha256": case.get("config_sha256"),
        "testbench_sha256": case.get("testbench_sha256"),
        "light_hls_executable_sha256": case.get(
            "light_hls_executable_sha256"
        ),
        "valid": (
            case.get("functional_equivalence") is True
            and case.get("normalized_interface_equivalence") is True
        ),
        "reward_eligible": case.get("reward_eligible") is True,
        "functional_equivalence": case.get("functional_equivalence") is True,
        "native_interface_equivalence": case.get(
            "exact_interface_equivalence"
        ) is True,
        "normalized_interface_equivalence": case.get(
            "normalized_interface_equivalence"
        ) is True,
        "trace_count": case.get("result_trace_count", 0),
        "simulation_cycles": case.get("simulation_cycles", {}),
        "initiation_interval": {
            "light_hls": light_ii,
            "light_hls_dataflow_graph": dataflow.get("graph_ii"),
            "vitis_hls_loops": vitis_iis(vitis.get("estimate", {})),
            "vitis_hls_top": vitis.get("estimate", {}).get(
                "interval_cycles", {}
            ),
        },
        "light_hls": compact_qor(light.get("post_rtl_synthesis", {})),
        "vitis_hls": compact_qor(vitis.get("post_rtl_synthesis", {})),
        "deltas": case.get("deltas", {}),
    }


def scalar_field(case: dict[str, Any], *path: str) -> Any:
    value: Any = case
    for key in path:
        if not isinstance(value, dict):
            return None
        value = value.get(key)
    return value


def csv_row(case: dict[str, Any]) -> dict[str, Any]:
    return {
        "suite": case["suite"],
        "case": case["case"],
        "top": case["top"],
        "comparison_id": case["comparison_id"],
        "source_sha256": case["source_sha256"],
        "config_sha256": case["config_sha256"],
        "valid": case["valid"],
        "reward_eligible": case["reward_eligible"],
        "functional_equivalence": case["functional_equivalence"],
        "native_interface_equivalence": case["native_interface_equivalence"],
        "normalized_interface_equivalence": case[
            "normalized_interface_equivalence"
        ],
        "trace_count": case["trace_count"],
        "light_cycles": scalar_field(
            case, "simulation_cycles", "light_hls"
        ),
        "vitis_cycles": scalar_field(
            case, "simulation_cycles", "vitis_hls"
        ),
        "light_ii": scalar_field(case, "initiation_interval", "light_hls"),
        "light_graph_ii": scalar_field(
            case, "initiation_interval", "light_hls_dataflow_graph"
        ),
        "vitis_loop_iis": ",".join(
            str(value)
            for value in scalar_field(
                case, "initiation_interval", "vitis_hls_loops"
            ) or []
        ),
        "light_lut": scalar_field(case, "light_hls", "resources", "lut"),
        "vitis_lut": scalar_field(case, "vitis_hls", "resources", "lut"),
        "light_ff": scalar_field(case, "light_hls", "resources", "ff"),
        "vitis_ff": scalar_field(case, "vitis_hls", "resources", "ff"),
        "light_dsp": scalar_field(case, "light_hls", "resources", "dsp"),
        "vitis_dsp": scalar_field(case, "vitis_hls", "resources", "dsp"),
        "light_bram_tiles": scalar_field(
            case, "light_hls", "resources", "bram_tiles"
        ),
        "vitis_bram_tiles": scalar_field(
            case, "vitis_hls", "resources", "bram_tiles"
        ),
        "light_wns_ns": scalar_field(
            case, "light_hls", "timing", "worst_slack_ns"
        ),
        "vitis_wns_ns": scalar_field(
            case, "vitis_hls", "timing", "worst_slack_ns"
        ),
        "light_tool_version": scalar_field(case, "light_hls", "tool_version"),
        "vitis_tool_version": scalar_field(case, "vitis_hls", "tool_version"),
    }


def write_human_report(path: Path, cases: list[dict[str, Any]]) -> None:
    headings = (
        "Suite", "Case", "L-Cycles", "V-Cycles", "L-LUT", "V-LUT",
        "L-FF", "V-FF", "L-WNS", "V-WNS", "Reward",
    )
    widths = (16, 22, 9, 9, 7, 7, 7, 7, 8, 8, 7)

    def text(value: Any) -> str:
        if value is None:
            return "-"
        if isinstance(value, float):
            return f"{value:.3f}"
        return str(value)

    def row(values: tuple[Any, ...]) -> str:
        return "| " + " | ".join(
            text(value).ljust(width)
            for value, width in zip(values, widths)
        ) + " |"

    separator = "+-" + "-+-".join("-" * width for width in widths) + "-+"
    lines = [
        "DAPO-II U250 Ground-Truth Baseline Archive",
        "Part: xcu250-figd2104-2L-e",
        "QoR: common normalized-wrapper Vivado OOC synthesis",
        "",
        separator,
        row(headings),
        separator,
    ]
    for case in cases:
        values = csv_row(case)
        lines.append(row((
            values["suite"], values["case"], values["light_cycles"],
            values["vitis_cycles"], values["light_lut"], values["vitis_lut"],
            values["light_ff"], values["vitis_ff"], values["light_wns_ns"],
            values["vitis_wns_ns"], "yes" if values["reward_eligible"] else "no",
        )))
    lines.extend([
        separator,
        "",
        "Only functionally equivalent, normalized-interface-compatible cases",
        "are reward-eligible. Raw Vivado and Vitis projects stay outside Git.",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    arguments = parse_arguments()
    output = Path(arguments.output).expanduser().resolve()
    if output.exists() and any(output.iterdir()):
        raise ArchiveError(f"Output directory must be absent or empty: {output}")
    output.mkdir(parents=True, exist_ok=True)

    inputs = []
    cases = []
    for value in arguments.input:
        suite, path = parse_input(value)
        report = json.loads(path.read_text(encoding="utf-8"))
        inputs.append({
            "suite": suite,
            "report": path.name,
            "report_sha256": sha256_file(path),
            "stage": report.get("stage"),
            "clock_period_ns": report.get("clock_period_ns"),
        })
        if isinstance(report.get("cases"), list):
            cases.extend(
                normalize_batch_case(suite, case) for case in report["cases"]
            )
        else:
            cases.append(normalize_single(suite, report))

    archive = {
        "schema_version": "1.0",
        "stage": "u250_ground_truth_baseline_archive",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "valid": all(case["valid"] for case in cases),
        "reward_eligible": all(case["reward_eligible"] for case in cases),
        "board": "u250",
        "part": "xcu250-figd2104-2L-e",
        "clock_period_ns": sorted({
            item["clock_period_ns"]
            for item in inputs
            if item["clock_period_ns"] is not None
        }),
        "flow": next(iter({case[generator].get("flow_id") for case in cases
                           for generator in ("light_hls", "vitis_hls")})) if cases else None,
        "inputs": inputs,
        "cases": cases,
    }
    flows = {case[generator].get("flow_id") for case in cases
             for generator in ("light_hls", "vitis_hls")}
    if len(flows) != 1 or None in flows:
        raise ArchiveError("Do not mix legacy and constrained synthesis flows in one baseline")
    json_path = output / "u250_baselines.json"
    csv_path = output / "u250_baselines.csv"
    report_path = output / "u250_baselines.rpt"
    json_path.write_text(
        json.dumps(archive, indent=2, sort_keys=False) + "\n",
        encoding="utf-8",
    )
    rows = [csv_row(case) for case in cases]
    with csv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=list(rows[0]) if rows else [],
            lineterminator="\n",
        )
        if rows:
            writer.writeheader()
            writer.writerows(rows)
    write_human_report(report_path, cases)
    print(f"Normalized baseline JSON: {json_path}")
    print(f"Normalized baseline CSV: {csv_path}")
    print(f"Normalized baseline report: {report_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ArchiveError, OSError, json.JSONDecodeError) as error:
        print(str(error))
        raise SystemExit(1)
