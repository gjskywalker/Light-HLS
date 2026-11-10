#!/usr/bin/env python3

"""Run and archive the complete DAPO-II U250 comparison baseline."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class BaselineSuite:
    name: str
    script: str
    report: str


SUITES = (
    BaselineSuite(
        "scalar", "run_backend_scalar_u250.sh", "u250_fair_comparison.json"
    ),
    BaselineSuite(
        "pipeline",
        "run_pipeline_u250_comparisons.py",
        "u250_pipeline_comparison.json",
    ),
    BaselineSuite(
        "array_partition",
        "run_array_partition_u250_comparisons.py",
        "u250_array_partition_comparison.json",
    ),
    BaselineSuite(
        "dataflow",
        "run_dataflow_u250_comparisons.py",
        "u250_dataflow_comparison.json",
    ),
)


class BaselineError(RuntimeError):
    pass


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--light-hls-top", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--clock-period-ns", type=float, default=10.0)
    parser.add_argument("--timeout-seconds", type=int, default=900)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--keep-going", action="store_true")
    parser.add_argument(
        "--suite",
        action="append",
        choices=[suite.name for suite in SUITES],
        help="Run only the selected suite; may be repeated",
    )
    return parser.parse_args()


def complete_report(path: Path) -> bool:
    if not path.is_file():
        return False
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    functional = report.get("functional_equivalence")
    functional_valid = (
        functional is True
        or (isinstance(functional, dict) and functional.get("valid") is True)
    )
    return report.get("valid") is True and functional_valid


def preserve_path(path: Path, label: str) -> Path:
    index = 1
    backup = path.parent / f"{path.name}.{label}.{index}"
    while backup.exists():
        index += 1
        backup = path.parent / f"{path.name}.{label}.{index}"
    path.rename(backup)
    return backup


def suite_command(
    suite: BaselineSuite,
    script_directory: Path,
    executable: Path,
    output: Path,
    arguments: argparse.Namespace,
) -> list[str]:
    script = script_directory / suite.script
    if suite.name == "scalar":
        light_hls_root = script_directory.parent.parent
        return [
            sys.executable,
            str(script_directory / "run_u250_fair_comparison.py"),
            "--light-hls-top",
            str(executable),
            "--source",
            str(light_hls_root / "App" / "backend_scalar" / "backend_scalar.c"),
            "--top",
            "backend_scalar",
            "--testbench",
            str(
                light_hls_root
                / "App"
                / "backend_scalar"
                / "backend_scalar_tb.v"
            ),
            "--testbench-top",
            "backend_scalar_tb",
            "--clock-period-ns",
            str(arguments.clock_period_ns),
            "--timeout-seconds",
            str(arguments.timeout_seconds),
            "--output",
            str(output),
        ]
    command = [
        sys.executable,
        str(script),
        "--light-hls-top",
        str(executable),
        "--output",
        str(output),
        "--clock-period-ns",
        str(arguments.clock_period_ns),
        "--timeout-seconds",
        str(arguments.timeout_seconds),
    ]
    if arguments.resume:
        command.append("--resume")
    return command


def write_run_summary(path: Path, results: list[dict]) -> None:
    summary = {
        "schema_version": "1.0",
        "stage": "u250_baseline_suite_run",
        "valid": all(result["status"] == "PASS" for result in results),
        "results": results,
    }
    path.write_text(
        json.dumps(summary, indent=2, sort_keys=False) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    arguments = parse_arguments()
    script_directory = Path(__file__).resolve().parent
    executable = Path(arguments.light_hls_top).expanduser().resolve()
    output_root = Path(arguments.output).expanduser().resolve()
    if not executable.is_file():
        raise BaselineError(f"Light_HLS_Top does not exist: {executable}")
    if arguments.clock_period_ns <= 0 or arguments.timeout_seconds <= 0:
        raise BaselineError("Clock period and timeout must be positive")
    if output_root.exists() and any(output_root.iterdir()) and not arguments.resume:
        raise BaselineError(f"Output directory must be absent or empty: {output_root}")
    output_root.mkdir(parents=True, exist_ok=True)

    selected = set(arguments.suite or ())
    suites = [suite for suite in SUITES if not selected or suite.name in selected]
    results = []
    successful_inputs: list[tuple[str, Path]] = []
    for suite in suites:
        suite_output = output_root / suite.name
        report_path = suite_output / suite.report
        if arguments.resume and complete_report(report_path):
            print(f"[{suite.name}] reusing complete comparison", flush=True)
            results.append({
                "suite": suite.name,
                "status": "PASS",
                "reused": True,
                "elapsed_seconds": 0.0,
                "report": str(report_path),
                "log": None,
            })
            successful_inputs.append((suite.name, report_path))
            continue
        if suite_output.exists() and suite.name == "scalar":
            backup = preserve_path(suite_output, "incomplete")
            print(f"[{suite.name}] preserved incomplete run at {backup}", flush=True)

        command = suite_command(
            suite, script_directory, executable, suite_output, arguments
        )
        log_path = output_root / f"{suite.name}.log"
        print(f"[{suite.name}] starting U250 comparison", flush=True)
        started = time.monotonic()
        with log_path.open("w", encoding="utf-8") as log:
            completed = subprocess.run(
                command,
                cwd=script_directory.parent.parent.parent,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )
        elapsed = time.monotonic() - started
        status = (
            "PASS"
            if completed.returncode == 0 and complete_report(report_path)
            else "FAIL"
        )
        result = {
            "suite": suite.name,
            "status": status,
            "reused": False,
            "returncode": completed.returncode,
            "elapsed_seconds": round(elapsed, 6),
            "report": str(report_path),
            "log": str(log_path),
        }
        results.append(result)
        write_run_summary(output_root / "u250_baseline_run.json", results)
        print(
            f"[{suite.name}] {status} ({elapsed:.1f}s); log={log_path}",
            flush=True,
        )
        if status == "PASS":
            successful_inputs.append((suite.name, report_path))
        elif not arguments.keep_going:
            raise BaselineError(f"Suite {suite.name} failed; see {log_path}")

    if not successful_inputs:
        raise BaselineError("No complete comparison is available for archiving")
    normalized_output = output_root / "normalized"
    if normalized_output.exists():
        backup = preserve_path(normalized_output, "previous")
        print(f"Preserved previous normalized archive at {backup}", flush=True)
    archive_command = [
        sys.executable,
        str(script_directory / "archive_u250_baselines.py"),
        "--output",
        str(normalized_output),
    ]
    for suite, path in successful_inputs:
        archive_command.extend(("--input", f"{suite}={path}"))
    archive_log = output_root / "archive.log"
    with archive_log.open("w", encoding="utf-8") as log:
        completed = subprocess.run(
            archive_command,
            cwd=script_directory.parent.parent.parent,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    if completed.returncode != 0:
        raise BaselineError(f"Baseline archive failed; see {archive_log}")
    write_run_summary(output_root / "u250_baseline_run.json", results)
    print(f"Normalized archive: {normalized_output}")
    return 0 if all(result["status"] == "PASS" for result in results) else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (BaselineError, OSError, json.JSONDecodeError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
