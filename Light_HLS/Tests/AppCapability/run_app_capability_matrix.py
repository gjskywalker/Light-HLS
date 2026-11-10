#!/usr/bin/env python3

"""Classify every Light-HLS App directory against the RTL capability contract."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


class CapabilityError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_arguments() -> argparse.Namespace:
    script_directory = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--light-hls-top", required=True)
    parser.add_argument(
        "--manifest", default=str(script_directory / "app_capabilities.json")
    )
    parser.add_argument("--output", required=True)
    parser.add_argument("--board", default="u250")
    parser.add_argument("--freq", default="100MHz")
    parser.add_argument("--timeout-seconds", type=int, default=180)
    parser.add_argument("--regression-summary")
    parser.add_argument(
        "--app", action="append", help="Run only the selected App directory"
    )
    return parser.parse_args()


def load_regression_status(path_text: str | None) -> dict[str, str]:
    if not path_text:
        return {}
    path = Path(path_text).expanduser().resolve()
    if not path.is_file():
        raise CapabilityError(f"Regression summary does not exist: {path}")
    report = json.loads(path.read_text(encoding="utf-8"))
    return {
        result["suite"]: result["status"]
        for result in report.get("results", [])
        if result.get("suite") and result.get("status")
    }


def diagnostic_codes(path: Path) -> list[str]:
    if not path.is_file():
        return []
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return []
    codes = []
    for key in ("errors", "warnings"):
        for item in report.get(key, []):
            if isinstance(item, dict) and item.get("code"):
                codes.append(str(item["code"]))
    return sorted(set(codes))


def classify(
    expectation: str,
    returncode: int | None,
    rtl_valid: bool,
    codes: list[str],
    expected_codes: list[str],
) -> str:
    if expectation == "not_applicable":
        return "NOT_APPLICABLE"
    succeeded = returncode == 0 and rtl_valid
    if expectation == "success":
        return "PASS" if succeeded else "UNEXPECTED_FAILURE"
    if succeeded:
        return "UNEXPECTED_SUCCESS"
    if expected_codes and not set(expected_codes).intersection(codes):
        return "UNEXPECTED_DIAGNOSTIC"
    return "EXPECTED_REJECTION"


def write_human_report(path: Path, results: list[dict[str, Any]]) -> None:
    headings = (
        "App", "Expected", "Actual", "RTL", "Functional", "Training", "Reason"
    )
    widths = (31, 14, 23, 5, 10, 9, 40)

    def value(value: Any) -> str:
        if value is None:
            return "-"
        if isinstance(value, bool):
            return "yes" if value else "no"
        return str(value)

    def row(values: tuple[Any, ...]) -> str:
        cells = []
        for item, width in zip(values, widths):
            text = value(item)
            if len(text) > width:
                text = text[: width - 3] + "..."
            cells.append(text.ljust(width))
        return "| " + " | ".join(cells) + " |"

    separator = "+-" + "-+-".join("-" * width for width in widths) + "-+"
    lines = [
        "Light-HLS App Capability Matrix",
        "One representative compilation per App directory",
        "",
        separator,
        row(headings),
        separator,
    ]
    for result in results:
        lines.append(row((
            result["app"], result["expectation"], result["status"],
            result["rtl_valid"], result["functional_verified"],
            result["training_eligible"], result.get("unsupported_feature") or "",
        )))
    lines.extend([
        separator,
        "",
        "PASS means RTL generation and Icarus syntax validation succeeded.",
        "Training eligibility additionally requires a passing functional regression.",
        "EXPECTED_REJECTION identifies a documented unsupported feature.",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    arguments = parse_arguments()
    executable = Path(arguments.light_hls_top).expanduser().resolve()
    manifest_path = Path(arguments.manifest).expanduser().resolve()
    output_root = Path(arguments.output).expanduser().resolve()
    if not executable.is_file():
        raise CapabilityError(f"Light_HLS_Top does not exist: {executable}")
    if not manifest_path.is_file():
        raise CapabilityError(f"Capability manifest does not exist: {manifest_path}")
    if arguments.timeout_seconds <= 0:
        raise CapabilityError("Timeout must be positive")
    if output_root.exists() and any(output_root.iterdir()):
        raise CapabilityError(
            f"Output directory must be absent or empty: {output_root}"
        )
    output_root.mkdir(parents=True, exist_ok=True)

    light_hls_root = manifest_path.parents[2]
    app_root = light_hls_root / "App"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    entries = manifest.get("apps", [])
    manifest_names = [entry.get("app") for entry in entries]
    actual_names = sorted(path.name for path in app_root.iterdir() if path.is_dir())
    if len(manifest_names) != len(set(manifest_names)):
        raise CapabilityError("Capability manifest contains duplicate App entries")
    if sorted(manifest_names) != actual_names:
        missing = sorted(set(actual_names) - set(manifest_names))
        stale = sorted(set(manifest_names) - set(actual_names))
        raise CapabilityError(
            "Capability manifest does not match App directories; "
            f"missing={missing}, stale={stale}"
        )

    selected = set(arguments.app or ())
    unknown = selected - set(manifest_names)
    if unknown:
        raise CapabilityError(f"Unknown App directories: {sorted(unknown)}")
    regression_status = load_regression_status(arguments.regression_summary)
    executable_hash = sha256_file(executable)
    results = []
    for index, entry in enumerate(entries, 1):
        if selected and entry["app"] not in selected:
            continue
        expectation = entry["expectation"]
        source_relative = entry.get("source")
        config_relative = entry.get("config")
        source = light_hls_root / source_relative if source_relative else None
        config = light_hls_root / config_relative if config_relative else None
        case_root = output_root / f"{index:02d}_{entry['app']}"
        work = case_root / "work"
        case_root.mkdir(parents=True)
        returncode = None
        elapsed = 0.0
        rtl_valid = False
        syntax_returncode = None
        log_path = case_root / "light_hls.log"
        codes: list[str] = []
        if expectation != "not_applicable":
            if not source or not source.is_file():
                raise CapabilityError(f"Source does not exist: {source}")
            if config and not config.is_file():
                raise CapabilityError(f"Config does not exist: {config}")
            work.mkdir()
            command = [str(executable), str(source), entry["top"]]
            if config:
                command.append(str(config))
            command.extend((
                "--board", arguments.board, "--freq", arguments.freq,
                "--synthesis", "disable", "--output", str(case_root),
            ))
            started = time.monotonic()
            with log_path.open("w", encoding="utf-8") as log:
                try:
                    completed = subprocess.run(
                        command,
                        cwd=work,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        text=True,
                        timeout=arguments.timeout_seconds,
                        check=False,
                    )
                    returncode = completed.returncode
                except subprocess.TimeoutExpired:
                    returncode = -1
            elapsed = time.monotonic() - started
            codes = diagnostic_codes(case_root / "reports" / "rtl" / "failure.json")
            rtl = case_root / "reports" / "rtl" / "top.v"
            if returncode == 0 and rtl.is_file():
                syntax_log = case_root / "iverilog.log"
                with syntax_log.open("w", encoding="utf-8") as log:
                    syntax = subprocess.run(
                        ["iverilog", "-g2012", "-Wall", "-tnull", str(rtl)],
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        text=True,
                        check=False,
                    )
                syntax_returncode = syntax.returncode
                rtl_valid = syntax.returncode == 0

        status = classify(
            expectation,
            returncode,
            rtl_valid,
            codes,
            entry.get("expected_diagnostics", []),
        )
        suite = entry.get("functional_regression")
        functional_verified = bool(suite and regression_status.get(suite) == "PASS")
        result = {
            "index": index,
            "app": entry["app"],
            "source": source_relative,
            "source_sha256": sha256_file(source) if source else None,
            "top": entry.get("top"),
            "config": config_relative,
            "config_sha256": sha256_file(config) if config else None,
            "expectation": expectation,
            "status": status,
            "returncode": returncode,
            "syntax_returncode": syntax_returncode,
            "rtl_valid": rtl_valid,
            "diagnostic_codes": codes,
            "expected_diagnostics": entry.get("expected_diagnostics", []),
            "unsupported_feature": entry.get("unsupported_feature"),
            "functional_regression": suite,
            "functional_verified": functional_verified,
            "training_eligible": status == "PASS" and functional_verified,
            "elapsed_seconds": round(elapsed, 6),
            "log": (
                str(log_path.relative_to(output_root))
                if expectation != "not_applicable"
                else None
            ),
        }
        results.append(result)
        print(
            f"[{len(results):02d}/{len(entries)}] {entry['app']}: {status}",
            flush=True,
        )

    unexpected_statuses = {
        "UNEXPECTED_FAILURE", "UNEXPECTED_SUCCESS", "UNEXPECTED_DIAGNOSTIC"
    }
    summary = {
        "schema_version": "1.0",
        "stage": "light_hls_app_capability_matrix",
        "valid": not any(
            result["status"] in unexpected_statuses for result in results
        ),
        "board": arguments.board,
        "frequency": arguments.freq,
        "manifest": str(manifest_path.relative_to(light_hls_root)),
        "manifest_sha256": sha256_file(manifest_path),
        "light_hls_executable": executable.name,
        "light_hls_executable_sha256": executable_hash,
        "directory_count": len(entries),
        "selected_count": len(results),
        "counts": {
            status: sum(result["status"] == status for result in results)
            for status in sorted({result["status"] for result in results})
        },
        "results": results,
    }
    json_path = output_root / "app_capability_matrix.json"
    csv_path = output_root / "app_capability_matrix.csv"
    report_path = output_root / "app_capability_matrix.rpt"
    json_path.write_text(
        json.dumps(summary, indent=2, sort_keys=False) + "\n",
        encoding="utf-8",
    )
    fieldnames = [
        "index", "app", "source", "top", "config", "expectation", "status",
        "returncode", "syntax_returncode", "rtl_valid", "functional_regression",
        "functional_verified", "training_eligible", "unsupported_feature",
        "diagnostic_codes", "source_sha256", "config_sha256", "elapsed_seconds",
        "log",
    ]
    with csv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=fieldnames,
            extrasaction="ignore",
            lineterminator="\n",
        )
        writer.writeheader()
        for result in results:
            row = dict(result)
            row["diagnostic_codes"] = ",".join(result["diagnostic_codes"])
            writer.writerow(row)
    write_human_report(report_path, results)
    print(f"Capability JSON: {json_path}")
    print(f"Capability CSV: {csv_path}")
    print(f"Capability report: {report_path}")
    return 0 if summary["valid"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CapabilityError, OSError, json.JSONDecodeError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
