#!/usr/bin/env python3

import argparse
import json
import re
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class TestCase:
    name: str
    source: str
    top: str
    expected_pragmas: int
    expected_tasks: int
    expected_channels: int
    replacements: tuple[tuple[str, str], ...] = ()


TEST_CASES = (
    TestCase(
        "array_dataflow",
        "App/light_hls_dataflow/vector_array_dataflow.cc",
        "vector_array_dataflow",
        1,
        3,
        2,
    ),
    TestCase(
        "conv_stream_dataflow",
        "App/conv/conv.cc",
        "convs",
        3,
        3,
        2,
    ),
    TestCase(
        "stream_fifo",
        "App/light_hls_dataflow/vector_stream_dataflow.cc",
        "vector_stream_dataflow",
        3,
        3,
        2,
    ),
    TestCase(
        "stream_array_fifo",
        "App/light_hls_dataflow/vector_stream_array_dataflow.cc",
        "vector_stream_array_dataflow",
        2,
        2,
        6,
    ),
    TestCase(
        "fdtd_dataflow_optimized",
        "App/fdtd2d/fdtd_dataflow_optimized.cc",
        "kernel_fdtd_2d",
        6,
        3,
        2,
    ),
    TestCase(
        "fdtd_dataflow_stream_optimized",
        "App/fdtd2d/fdtd_dataflow_stream_optimized.cc",
        "kernel_fdtd_2d",
        8,
        3,
        2,
    ),
    TestCase(
        "stream_fifo_deep",
        "App/light_hls_dataflow/vector_stream_dataflow.cc",
        "vector_stream_dataflow",
        3,
        3,
        2,
        (
            ("depth=4 type=fifo", "depth=16 type=fifo"),
            ("depth=8 type=fifo", "depth=64 type=fifo"),
        ),
    ),
)

PRAGMA_RE = re.compile(r"Loaded\s+(\d+)\s+supported Vitis HLS pragma")
TASK_RE = re.compile(
    r"DATAFLOW function \[([^\]]+)\] has (\d+) task\(s\), "
    r"estimated II=(\d+)"
)
CHANNEL_RE = re.compile(
    r"DATAFLOW function \[([^\]]+)\] has (\d+) channel\(s\), "
    r"channel resource=(\d+) BRAM, (\d+) LUT, (\d+) FF"
)
FINAL_RE = re.compile(
    r"Done latency evaluation of top function: \[([^\]]+)\].*?"
    r"latency is (\d+).*?dataflow II is (\d+).*?"
    r"resource cost is \[DSP=(\d+), FF=(\d+), LUT=(\d+), BRAM=(\d+)\]"
)


def parse_args() -> argparse.Namespace:
    test_dir = Path(__file__).resolve().parent
    root = test_dir.parents[1]
    parser = argparse.ArgumentParser(
        description="Validate LightHLS DATAFLOW and STREAM source pragmas."
    )
    parser.add_argument("--root", type=Path, default=root)
    parser.add_argument(
        "--executable",
        type=Path,
        default=test_dir / "build/Light_HLS_Top",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=test_dir / "build/dataflow_pragma_tests",
    )
    parser.add_argument(
        "--freq",
        required=True,
        help="Required Light-HLS target frequency, for example 100MHz",
    )
    parser.add_argument("--timeout", type=int, default=180)
    return parser.parse_args()


def check_output(test: TestCase, output: str, returncode: int) -> list[str]:
    errors = []
    pragma = PRAGMA_RE.search(output)
    task = TASK_RE.search(output)
    channel = CHANNEL_RE.search(output)
    final = FINAL_RE.search(output)

    if returncode != 0:
        errors.append(f"return code is {returncode}")
    if not pragma or int(pragma.group(1)) != test.expected_pragmas:
        actual = pragma.group(1) if pragma else "missing"
        errors.append(
            f"pragma count is {actual}, expected {test.expected_pragmas}"
        )
    if (
        not task
        or task.group(1) != test.top
        or int(task.group(2)) != test.expected_tasks
        or int(task.group(3)) <= 0
    ):
        errors.append("DATAFLOW task count or II is invalid")
    if (
        not channel
        or channel.group(1) != test.top
        or int(channel.group(2)) != test.expected_channels
    ):
        errors.append("DATAFLOW channel count is invalid")
    if (
        not final
        or final.group(1) != test.top
        or int(final.group(2)) <= 0
        or int(final.group(3)) <= 0
    ):
        errors.append("final DATAFLOW latency/resource report is missing")
    return errors


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    executable = args.executable.resolve()
    output_dir = args.output.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    results = []
    for index, test in enumerate(TEST_CASES, 1):
        run_dir = output_dir / test.name
        run_dir.mkdir(parents=True, exist_ok=True)
        source = root / test.source
        source_text = source.read_text(encoding="utf-8")
        for old, new in test.replacements:
            if old not in source_text:
                raise RuntimeError(
                    f"{test.name}: replacement text not found: {old}"
                )
            source_text = source_text.replace(old, new, 1)
        run_source = run_dir / source.name
        run_source.write_text(source_text, encoding="utf-8")

        log_path = run_dir / "run.log"
        started = time.monotonic()
        returncode = None
        status = "ERROR"
        errors = []
        try:
            completed = subprocess.run(
                [
                    str(executable),
                    str(run_source),
                    test.top,
                    "--freq",
                    args.freq,
                ],
                cwd=run_dir,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=args.timeout,
                check=False,
            )
            returncode = completed.returncode
            log_path.write_text(completed.stdout, encoding="utf-8")
            errors = check_output(test, completed.stdout, returncode)
            status = "PASS" if not errors else "FAIL"
        except subprocess.TimeoutExpired as error:
            status = "TIMEOUT"
            captured = error.stdout or ""
            if isinstance(captured, bytes):
                captured = captured.decode("utf-8", errors="replace")
            log_path.write_text(captured, encoding="utf-8")
            errors = [f"exceeded {args.timeout}s timeout"]

        elapsed = time.monotonic() - started
        result = {
            "name": test.name,
            "source": test.source,
            "top": test.top,
            "status": status,
            "returncode": returncode,
            "elapsed_seconds": round(elapsed, 3),
            "errors": errors,
            "log": str(log_path),
        }
        results.append(result)
        detail = "" if not errors else f": {'; '.join(errors)}"
        print(
            f"[{index}/{len(TEST_CASES)}] {test.name}: {status} "
            f"({elapsed:.1f}s){detail}",
            flush=True,
        )

    summary_path = output_dir / "summary.json"
    summary_path.write_text(
        json.dumps(results, indent=2) + "\n", encoding="utf-8"
    )
    passed = sum(result["status"] == "PASS" for result in results)
    print(
        f"SUMMARY: {passed}/{len(results)} passed; {summary_path}",
        flush=True,
    )
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
