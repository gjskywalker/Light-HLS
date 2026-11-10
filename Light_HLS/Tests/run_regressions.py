#!/usr/bin/env python3

"""Run all local Light-HLS RTL regressions with one normalized summary."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class Suite:
    name: str
    script: str
    kind: str = "shell"


SUITES = (
    Suite("AppCorrectness", "AppCorrectness/run_supported_apps.sh"),
    Suite("RTL_Backend", "RTL_Backend/run_iverilog_test.sh"),
    Suite("PipelineBackend", "PipelineBackend/run_pipeline_tests.sh"),
    Suite("PipelineStorage", "PipelineStorage/run_storage_tests.sh"),
    Suite(
        "ArrayPartitionBackend",
        "ArrayPartitionBackend/run_array_partition_tests.sh",
    ),
    Suite("OrdinaryBankAnalysis", "OrdinaryBankAnalysis/run_tests.sh"),
    Suite("LocalWriteData", "LocalWriteData/run_tests.sh"),
    Suite("FIFOEvents", "FIFOEvents/run_tests.py", "compiler"),
    Suite("OrdinaryFeedback", "OrdinaryFeedback/run_tests.py", "compiler"),
    Suite("OrdinaryBackedge", "OrdinaryBackedge/run_tests.py", "compiler"),
    Suite("OrdinaryLoopEdges", "OrdinaryLoopEdges/run_tests.py", "compiler"),
    Suite("TransactionEdges", "TransactionEdges/run_tests.py", "compiler"),
    Suite("ScalarBankEvents", "ScalarBankEvents/run_tests.py", "compiler"),
    Suite("ScalarReturns", "ScalarReturns/run_tests.py", "compiler"),
    Suite("PIPOBanks", "PIPOBanks/run_tests.py", "compiler"),
    Suite("BindingCost", "BindingCost/run_tests.py", "compiler"),
    Suite("QoRPressure", "QoRPressure/run_tests.py", "compiler"),
    Suite("BankConflict", "BankConflict/run_tests.py", "compiler"),
    Suite("ArithmeticMapping", "ArithmeticMapping/run_tests.py", "compiler"),
    Suite("DataflowCompletion", "DataflowCompletion/run_tests.py", "compiler"),
    Suite(
        "StorageBindingBackend",
        "StorageBindingBackend/run_storage_binding_tests.sh",
    ),
    Suite("DataflowBackend", "DataflowBackend/run_dataflow_backend_tests.sh"),
    Suite(
        "FairComparisonInterface",
        "FairComparison/test_interface_normalization.py",
        "python",
    ),
    Suite(
        "AppCapability",
        "AppCapability/run_app_capability_matrix.py",
        "capability",
    ),
    Suite("FairComparisonQoR", "FairComparison/test_qor_checkpoint.py", "python"),
    Suite("TransactionEventMetrics", "FairComparison/test_transaction_events.py", "python"),
    Suite("EvaluationContract", "FairComparison/test_evaluation_contract.py", "python"),
)


class RegressionError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--light-hls-top", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--timeout-seconds", type=int, default=600)
    parser.add_argument("--keep-going", action="store_true")
    parser.add_argument(
        "--suite",
        action="append",
        choices=[suite.name for suite in SUITES],
        help="Run only the selected suite; may be repeated",
    )
    return parser.parse_args()


def write_summary(
    path: Path,
    executable: Path,
    executable_hash: str,
    results: list[dict[str, Any]],
    expected_suites: int,
) -> None:
    summary = {
        "schema_version": "1.0",
        "stage": "light_hls_local_regression_suite",
        "valid": len(results) == expected_suites and all(result["status"] == "PASS" for result in results),
        "expected_suites": expected_suites,
        "completed_suites": len(results),
        "light_hls_executable": str(executable),
        "light_hls_executable_sha256": executable_hash,
        "results": results,
    }
    path.write_text(
        json.dumps(summary, indent=2, sort_keys=False) + "\n",
        encoding="utf-8",
    )


def write_human_report(path: Path, results: list[dict[str, Any]]) -> None:
    headings = ("Suite", "Status", "Runtime (s)", "Script SHA256", "Log")
    widths = (28, 8, 12, 16, 55)

    def row(values: tuple[Any, ...]) -> str:
        cells = []
        for item, width in zip(values, widths):
            text = str(item)
            if len(text) > width:
                text = text[: width - 3] + "..."
            cells.append(text.ljust(width))
        return "| " + " | ".join(cells) + " |"

    separator = "+-" + "-+-".join("-" * width for width in widths) + "-+"
    lines = [
        "Light-HLS Unified Local Regression",
        "",
        separator,
        row(headings),
        separator,
    ]
    for result in results:
        lines.append(row((
            result["suite"], result["status"], result["elapsed_seconds"],
            result["script_sha256"][:16], result["log"],
        )))
    lines.extend([separator, ""])
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    arguments = parse_arguments()
    tests_root = Path(__file__).resolve().parent
    executable = Path(arguments.light_hls_top).expanduser().resolve()
    output = Path(arguments.output).expanduser().resolve()
    if not executable.is_file():
        raise RegressionError(f"Light_HLS_Top does not exist: {executable}")
    if arguments.timeout_seconds <= 0:
        raise RegressionError("Timeout must be positive")
    if output.exists() and any(output.iterdir()):
        raise RegressionError(f"Output directory must be absent or empty: {output}")
    output.mkdir(parents=True, exist_ok=True)

    selected = set(arguments.suite or ())
    suites = [suite for suite in SUITES if not selected or suite.name in selected]
    executable_hash = sha256_file(executable)
    summary_path = output / "local_regressions.json"
    results = []
    for suite in suites:
        script = tests_root / suite.script
        if not script.is_file():
            raise RegressionError(f"Regression script does not exist: {script}")
        suite_output = output / suite.name
        log_path = output / f"{suite.name}.log"
        if suite.kind == "shell":
            command = [str(script), str(executable)]
        elif suite.kind == "python":
            command = [sys.executable, str(script)]
        elif suite.kind == "compiler":
            command = [sys.executable, str(script), str(executable),
                       "--output", str(suite_output)]
        else:
            command = [
                sys.executable,
                str(script),
                "--light-hls-top",
                str(executable),
                "--output",
                str(suite_output),
                "--regression-summary",
                str(summary_path),
            ]
        print(f"[{suite.name}] starting", flush=True)
        started = time.monotonic()
        with log_path.open("w", encoding="utf-8") as log:
            try:
                completed = subprocess.run(
                    command,
                    cwd=tests_root,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    text=True,
                    timeout=arguments.timeout_seconds,
                    check=False,
                )
                returncode = completed.returncode
                status = "PASS" if returncode == 0 else "FAIL"
            except subprocess.TimeoutExpired:
                returncode = None
                status = "TIMEOUT"
        elapsed = time.monotonic() - started
        result = {
            "suite": suite.name,
            "status": status,
            "returncode": returncode,
            "elapsed_seconds": round(elapsed, 6),
            "script": str(script),
            "script_sha256": sha256_file(script),
            "log": str(log_path),
            "result_directory": (
                str(suite_output) if suite.kind in ("capability", "compiler") else None
            ),
        }
        results.append(result)
        write_summary(summary_path, executable, executable_hash, results, len(suites))
        print(f"[{suite.name}] {status} ({elapsed:.1f}s)", flush=True)
        if status != "PASS" and not arguments.keep_going:
            break

    csv_path = output / "local_regressions.csv"
    report_path = output / "local_regressions.rpt"
    with csv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=list(results[0]) if results else [],
            lineterminator="\n",
        )
        if results:
            writer.writeheader()
            writer.writerows(results)
    write_human_report(report_path, results)
    valid = len(results) == len(suites) and all(
        result["status"] == "PASS" for result in results
    )
    return 0 if valid else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RegressionError, OSError, json.JSONDecodeError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
