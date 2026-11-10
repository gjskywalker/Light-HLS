#!/usr/bin/env python3

import argparse
import csv
import json
import os
import re
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from run_app_cases import CASES


@dataclass(frozen=True)
class FunctionSpan:
    parameters_begin: int
    parameters_end: int
    body_begin: int
    body_end: int


@dataclass(frozen=True)
class Variant:
    name: str
    loop_pragmas: tuple[str, ...]
    partition_type: str


VARIANTS = (
    Variant(
        "pipeline_block",
        ("#pragma HLS PIPELINE II=1",),
        "block",
    ),
    Variant(
        "unroll_cyclic",
        ("#pragma HLS UNROLL factor=2",),
        "cyclic",
    ),
    Variant(
        "pipeline_unroll_complete",
        ("#pragma HLS PIPELINE II=1", "#pragma HLS UNROLL factor=2"),
        "complete",
    ),
)

FINAL_RESULT_RE = re.compile(
    r"Done latency evaluation of top function: \[(?P<top>[^\]]+)\]"
    r"[^\n]*?latency is (?P<latency>\d+)"
    r"[^\n]*?resource cost is \[DSP=(?P<dsp>\d+), FF=(?P<ff>\d+), "
    r"LUT=(?P<lut>\d+), BRAM=(?P<bram>\d+)\]",
)
SOURCE_PRAGMA_RE = re.compile(
    r"Loaded\s+(\d+)\s+supported Vitis HLS pragma"
)
PIPELINE_INFEASIBLE_RE = re.compile(
    r"Failed to pipeline the loop|Cannot pipeline .*required inner loop|"
    r"Cannot pipeline .*inner loop.*required for full unrolling|"
    r"should not be pipelined with subloops"
)
SUPPORTED_SOURCE_PRAGMA_LINE_RE = re.compile(
    r"(?im)^[ \t]*#[ \t]*pragma[ \t]+HLS[ \t]+"
    r"(?:PIPELINE|UNROLL|ARRAY_PARTITION)\b[^\r\n]*"
)
AP_INT_RE = re.compile(r"\bap_(?:u?int)\s*<|[\"<]ap_int\.h[\">]")


def mask_c_family_source(source: str) -> str:
    """Mask comments and literals while preserving offsets and newlines."""
    result = list(source)
    index = 0
    state = "code"
    quote = ""
    while index < len(source):
        current = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if current == "/" and following == "/":
                result[index] = result[index + 1] = " "
                index += 2
                state = "line_comment"
                continue
            if current == "/" and following == "*":
                result[index] = result[index + 1] = " "
                index += 2
                state = "block_comment"
                continue
            if current in ("'", '"'):
                quote = current
                result[index] = " "
                index += 1
                state = "literal"
                continue
        elif state == "line_comment":
            if current == "\n":
                state = "code"
            else:
                result[index] = " "
        elif state == "block_comment":
            if current == "*" and following == "/":
                result[index] = result[index + 1] = " "
                index += 2
                state = "code"
                continue
            if current != "\n":
                result[index] = " "
        else:
            if current == "\\" and following:
                if current != "\n":
                    result[index] = " "
                if following != "\n":
                    result[index + 1] = " "
                index += 2
                continue
            if current == quote:
                result[index] = " "
                state = "code"
            elif current != "\n":
                result[index] = " "
        index += 1
    return "".join(result)


def strip_supported_source_pragmas(source: str) -> str:
    """Remove prior supported directives while preserving source line numbers."""
    return SUPPORTED_SOURCE_PRAGMA_LINE_RE.sub("", source)


def skip_space(masked: str, position: int) -> int:
    while position < len(masked) and masked[position].isspace():
        position += 1
    return position


def matching_delimiter(masked: str, opening: int, left: str, right: str) -> int:
    depth = 0
    for position in range(opening, len(masked)):
        if masked[position] == left:
            depth += 1
        elif masked[position] == right:
            depth -= 1
            if depth == 0:
                return position
    raise ValueError(f"unmatched '{left}' at source offset {opening}")


def find_function(masked: str, function_name: str) -> FunctionSpan:
    pattern = re.compile(rf"\b{re.escape(function_name)}\s*\(")
    for match in pattern.finditer(masked):
        parameters_begin = masked.find("(", match.start())
        parameters_end = matching_delimiter(masked, parameters_begin, "(", ")")
        body_begin = skip_space(masked, parameters_end + 1)
        if body_begin >= len(masked) or masked[body_begin] != "{":
            continue
        body_end = matching_delimiter(masked, body_begin, "{", "}")
        return FunctionSpan(
            parameters_begin=parameters_begin,
            parameters_end=parameters_end,
            body_begin=body_begin,
            body_end=body_end,
        )
    raise ValueError(f"cannot find definition of top function '{function_name}'")


def statement_end(masked: str, position: int) -> int:
    position = skip_space(masked, position)
    if position >= len(masked):
        return position
    if masked[position] == "{":
        return matching_delimiter(masked, position, "{", "}")
    nested_for = re.match(r"for\b", masked[position:])
    if nested_for:
        paren = masked.find("(", position + nested_for.end())
        if paren >= 0:
            close = matching_delimiter(masked, paren, "(", ")")
            return statement_end(masked, close + 1)

    paren_depth = 0
    bracket_depth = 0
    brace_depth = 0
    for cursor in range(position, len(masked)):
        current = masked[cursor]
        if current == "(":
            paren_depth += 1
        elif current == ")":
            paren_depth -= 1
        elif current == "[":
            bracket_depth += 1
        elif current == "]":
            bracket_depth -= 1
        elif current == "{":
            brace_depth += 1
        elif current == "}":
            if brace_depth == 0:
                return cursor
            brace_depth -= 1
        elif (
            current == ";"
            and paren_depth == 0
            and bracket_depth == 0
            and brace_depth == 0
        ):
            return cursor
    return len(masked) - 1


def loop_spans(masked: str, begin: int, end: int) -> list[tuple[int, int]]:
    spans = []
    for match in re.finditer(r"\bfor\s*\(", masked[begin:end]):
        position = begin + match.start()
        paren = masked.find("(", position, end)
        if paren < 0:
            continue
        try:
            close = matching_delimiter(masked, paren, "(", ")")
            spans.append((position, statement_end(masked, close + 1)))
        except ValueError:
            continue
    return spans


def select_loop(masked: str, top: FunctionSpan) -> Optional[int]:
    spans = loop_spans(masked, top.body_begin + 1, top.body_end)
    if not spans:
        spans = loop_spans(masked, 0, len(masked))
    if not spans:
        return None

    def depth(span: tuple[int, int]) -> tuple[int, int, int]:
        position = span[0]
        containment = sum(
            outer_begin < position < outer_end
            for outer_begin, outer_end in spans
        )
        brace_depth = (
            masked.count("{", 0, position) - masked.count("}", 0, position)
        )
        return containment, brace_depth, position

    return max(spans, key=depth)[0]


def select_array(masked: str, top: FunctionSpan) -> Optional[str]:
    parameters = masked[top.parameters_begin + 1 : top.parameters_end]
    array = re.search(
        r"\b([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\[\]]*\]\s*)+",
        parameters,
    )
    if array:
        return array.group(1)

    pointer = re.search(r"\*\s*([A-Za-z_][A-Za-z0-9_]*)\b", parameters)
    if pointer:
        return pointer.group(1)

    body = masked[top.body_begin + 1 : top.body_end]
    local_array = re.search(
        r"\b[A-Za-z_][A-Za-z0-9_:<>]*\s+"
        r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\[\]]*\]\s*)+",
        body,
    )
    if local_array:
        return local_array.group(1)
    return None


def line_indent(source: str, position: int) -> str:
    line_begin = source.rfind("\n", 0, position) + 1
    prefix = source[line_begin:position]
    return prefix if not prefix.strip() else ""


def array_pragma(array_name: str, partition_type: str) -> str:
    common = (
        f"#pragma HLS ARRAY_PARTITION variable={array_name} "
        f"type={partition_type}"
    )
    if partition_type == "complete":
        return f"{common} dim=1"
    return f"{common} factor=2 dim=1"


def apply_insertions(source: str, insertions: list[tuple[int, str]]) -> str:
    result = source
    for position, content in sorted(insertions, reverse=True):
        result = result[:position] + content + result[position:]
    return result


def loop_pragma_insertions(
    source: str,
    masked: str,
    loop_position: int,
    pragmas: tuple[str, ...],
) -> list[tuple[int, str]]:
    loop_indent = line_indent(source, loop_position)
    loop_match = re.match(r"for\b", masked[loop_position:])
    if not loop_match:
        raise ValueError(f"expected for-loop at source offset {loop_position}")
    parameters_begin = masked.find("(", loop_position + loop_match.end())
    parameters_end = matching_delimiter(masked, parameters_begin, "(", ")")
    body_begin = skip_space(masked, parameters_end + 1)
    if body_begin >= len(masked):
        raise ValueError(f"for-loop at source offset {loop_position} has no body")

    if masked[body_begin] == "{":
        indent = loop_indent + "    "
        insertion = body_begin + 1
        pragma_text = "".join(f"{indent}{pragma}\n" for pragma in pragmas)
        if source[insertion : insertion + 1] == "\n":
            return [(insertion + 1, pragma_text)]
        return [(insertion, "\n" + pragma_text)]

    body_line_begin = source.rfind("\n", 0, body_begin) + 1
    if body_line_begin > parameters_end:
        indent = line_indent(source, body_begin)
        pragma_text = "".join(f"{indent}{pragma}\n" for pragma in pragmas)
        return [(body_line_begin, pragma_text)]

    indent = loop_indent + "    "
    body_end = statement_end(masked, body_begin)
    prefix = "{\n" + "".join(f"{indent}{pragma}\n" for pragma in pragmas) + indent
    suffix = "\n" + loop_indent + "}"
    return [(body_begin, prefix), (body_end + 1, suffix)]


def generate_variant(
    source: str,
    top_name: str,
    variant: Variant,
) -> tuple[Optional[str], dict]:
    masked = mask_c_family_source(source)
    top = find_function(masked, top_name)
    loop_position = select_loop(masked, top)
    array_name = select_array(masked, top)
    metadata = {
        "array": array_name,
        "partition_type": variant.partition_type if array_name else None,
        "loop_line": source.count("\n", 0, loop_position) + 1
        if loop_position is not None
        else None,
        "loop_in_top": (
            loop_position is not None
            and top.body_begin < loop_position < top.body_end
        ),
    }
    if loop_position is None and array_name is None:
        metadata["reason"] = (
            "top/source has no loop and top function has no partitionable array or pointer"
        )
        return None, metadata

    insertions = []
    if loop_position is not None:
        insertions.extend(
            loop_pragma_insertions(
                source, masked, loop_position, variant.loop_pragmas
            )
        )

    if array_name is not None:
        after_brace = top.body_begin + 1
        indent = line_indent(source, top.body_begin) + "    "
        prefix = "" if source[after_brace : after_brace + 1] == "\n" else "\n"
        partition_text = (
            f"{prefix}{indent}{array_pragma(array_name, variant.partition_type)}\n"
        )
        if source[after_brace : after_brace + 1] == "\n":
            after_brace += 1
        insertions.append((after_brace, partition_text))

    metadata["pragma_count"] = len(variant.loop_pragmas) + int(array_name is not None)
    return apply_insertions(source, insertions), metadata


def parse_result(log_text: str) -> dict:
    matches = list(FINAL_RESULT_RE.finditer(log_text))
    result = {
        "loaded_source_pragmas": None,
        "latency": None,
        "dsp": None,
        "ff": None,
        "lut": None,
        "bram": None,
    }
    pragma_match = SOURCE_PRAGMA_RE.search(log_text)
    if pragma_match:
        result["loaded_source_pragmas"] = int(pragma_match.group(1))
    if matches:
        values = matches[-1].groupdict()
        for key in ("latency", "dsp", "ff", "lut", "bram"):
            result[key] = int(values[key])
    return result


def write_summaries(output: Path, results: list[dict]) -> None:
    json_path = output / "summary.json"
    csv_path = output / "summary.csv"
    json_path.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    fieldnames = [
        "index",
        "case",
        "variant",
        "partition_type",
        "array",
        "loop_line",
        "loop_in_top",
        "expected_pragmas",
        "loaded_source_pragmas",
        "status",
        "returncode",
        "elapsed_seconds",
        "latency",
        "dsp",
        "ff",
        "lut",
        "bram",
        "source",
        "generated_source",
        "log",
        "reason",
    ]
    with csv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(results)


def main() -> int:
    test_dir = Path(__file__).resolve().parent
    root = test_dir.parents[1]
    parser = argparse.ArgumentParser(
        description=(
            "Generate and run three Vitis-style pragma combinations for App cases. "
            "The variants collectively cover block, cyclic, and complete partitioning."
        )
    )
    parser.add_argument("--root", type=Path, default=root)
    parser.add_argument("--build", type=Path, default=test_dir / "build-llvm18")
    parser.add_argument(
        "--executable",
        type=Path,
        help=(
            "Evaluator executable; defaults to the exp14 executable under --build"
        ),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=test_dir / "build-llvm18" / "app_pragma_variants",
    )
    parser.add_argument("--config", type=Path)
    parser.add_argument("--clock", default="10")
    parser.add_argument(
        "--freq",
        help="Pass an explicit target frequency to a Light_HLS_Top executable",
    )
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--indices", help="Comma-separated one-based case indices")
    parser.add_argument(
        "--variants",
        help="Comma-separated variant names; default runs all three",
    )
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument(
        "--reuse",
        action="store_true",
        help="Reuse existing logs in the output directory instead of rerunning",
    )
    parser.add_argument(
        "--baseline-summary",
        type=Path,
        help="run_app_cases.py summary.json used to classify baseline failures",
    )
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()

    root = args.root.resolve()
    build = args.build.resolve()
    output = args.output.resolve()
    executable = (
        args.executable.resolve()
        if args.executable
        else build / "LLVM_exp14_HI_WithDirectiveTimingResourceEvaluation"
    )
    if not args.generate_only and not executable.is_file():
        parser.error(f"executable does not exist: {executable}")

    selected_indices = None
    if args.indices:
        selected_indices = {int(value) for value in args.indices.split(",")}
    selected_variants = {variant.name for variant in VARIANTS}
    if args.variants:
        selected_variants = set(args.variants.split(","))
        unknown = selected_variants - {variant.name for variant in VARIANTS}
        if unknown:
            parser.error(f"unknown variants: {', '.join(sorted(unknown))}")

    output.mkdir(parents=True, exist_ok=True)
    previous_results = {}
    previous_summary = output / "summary.json"
    if args.reuse and previous_summary.is_file():
        for result in json.loads(previous_summary.read_text(encoding="utf-8")):
            previous_results[(result["index"], result["variant"])] = result

    baseline_status = {}
    if args.baseline_summary:
        baseline_path = args.baseline_summary.resolve()
        if not baseline_path.is_file():
            parser.error(f"baseline summary does not exist: {baseline_path}")
        for result in json.loads(baseline_path.read_text(encoding="utf-8")):
            baseline_status[result["name"]] = result["status"]

    def classify_baseline_failure(status: str, case_name: str) -> tuple[str, Optional[str]]:
        original_status = baseline_status.get(case_name)
        if status == "FAIL" and original_status and original_status != "PASS":
            return (
                "BASELINE_FAIL",
                f"original no-pragma case also reports {original_status}",
            )
        return status, None

    def classify_pipeline_infeasible(
        status: str, log_text: str
    ) -> tuple[str, Optional[str]]:
        if status == "FAIL" and PIPELINE_INFEASIBLE_RE.search(log_text):
            return (
                "PIPELINE_INFEASIBLE",
                "the requested pipeline cannot be implemented because its "
                "remaining function or loop hierarchy cannot be flattened",
            )
        return status, None

    results = []
    total = sum(
        1
        for index in range(1, len(CASES) + 1)
        if selected_indices is None or index in selected_indices
    ) * len(selected_variants)
    completed_count = 0

    for index, (case_name, relative_source, top_name) in enumerate(CASES, 1):
        if selected_indices is not None and index not in selected_indices:
            continue
        original_path = root / relative_source
        if not original_path.is_file():
            for suffix in (".c", ".cc", ".cpp"):
                candidate = original_path.with_suffix(suffix)
                if candidate.is_file():
                    original_path = candidate
                    break
        original = original_path.read_text(encoding="utf-8", errors="replace")
        uses_ap_int = AP_INT_RE.search(original) is not None
        for variant in VARIANTS:
            if variant.name not in selected_variants:
                continue
            completed_count += 1
            run_dir = output / f"{index:02d}_{case_name}" / variant.name
            run_dir.mkdir(parents=True, exist_ok=True)
            if uses_ap_int:
                result = {
                    "index": index,
                    "case": case_name,
                    "variant": variant.name,
                    "partition_type": variant.partition_type,
                    "array": None,
                    "loop_line": None,
                    "loop_in_top": None,
                    "expected_pragmas": 0,
                    "status": "UNSUPPORTED_AP_INT",
                    "returncode": None,
                    "elapsed_seconds": 0.0,
                    "source": str(original_path),
                    "generated_source": None,
                    "log": None,
                    "reason": "ap_int/ap_uint cases are outside the current support scope",
                    **parse_result(""),
                }
                results.append(result)
                print(
                    f"[{completed_count:03d}/{total}] {case_name}/"
                    f"{variant.name}: UNSUPPORTED_AP_INT",
                    flush=True,
                )
                write_summaries(output, results)
                continue

            generated, metadata = generate_variant(
                strip_supported_source_pragmas(original), top_name, variant
            )
            generated_path = run_dir / (
                original_path.stem + f"__{variant.name}" + original_path.suffix
            )
            log_path = run_dir / "run.log"
            common = {
                "index": index,
                "case": case_name,
                "variant": variant.name,
                "partition_type": metadata.get("partition_type"),
                "array": metadata.get("array"),
                "loop_line": metadata.get("loop_line"),
                "loop_in_top": metadata.get("loop_in_top"),
                "expected_pragmas": metadata.get("pragma_count", 0),
                "source": str(original_path),
                "generated_source": str(generated_path) if generated else None,
                "log": str(log_path) if generated else None,
                "reason": metadata.get("reason"),
            }
            if generated is None:
                result = {
                    **common,
                    "status": "NOT_APPLICABLE",
                    "returncode": None,
                    "elapsed_seconds": 0.0,
                    **parse_result(""),
                }
                results.append(result)
                print(
                    f"[{completed_count:03d}/{total}] {case_name}/"
                    f"{variant.name}: NOT_APPLICABLE",
                    flush=True,
                )
                write_summaries(output, results)
                continue

            generated_path.write_text(generated, encoding="utf-8")
            if args.generate_only:
                result = {
                    **common,
                    "status": "GENERATED",
                    "returncode": None,
                    "elapsed_seconds": 0.0,
                    **parse_result(""),
                }
                results.append(result)
                print(
                    f"[{completed_count:03d}/{total}] {case_name}/"
                    f"{variant.name}: GENERATED",
                    flush=True,
                )
                write_summaries(output, results)
                continue

            previous = previous_results.get((index, variant.name))
            if args.reuse and previous and log_path.is_file():
                log_text = log_path.read_text(encoding="utf-8", errors="replace")
                parsed = parse_result(log_text)
                status = (
                    "PASS"
                    if (
                        previous.get("returncode") == 0
                        and "Accomplished LLVM processing" in log_text
                        and parsed["loaded_source_pragmas"]
                        == common["expected_pragmas"]
                        and parsed["latency"] is not None
                    )
                    else "FAIL"
                )
                status, baseline_reason = classify_baseline_failure(status, case_name)
                status, pipeline_reason = classify_pipeline_infeasible(
                    status, log_text
                )
                if status == "PIPELINE_INFEASIBLE":
                    for key in ("latency", "dsp", "ff", "lut", "bram"):
                        parsed[key] = None
                result = {
                    **common,
                    "status": status,
                    "returncode": previous.get("returncode"),
                    "elapsed_seconds": previous.get("elapsed_seconds", 0.0),
                    **parsed,
                }
                if baseline_reason:
                    result["reason"] = baseline_reason
                if pipeline_reason:
                    result["reason"] = pipeline_reason
                results.append(result)
                print(
                    f"[{completed_count:03d}/{total}] {case_name}/"
                    f"{variant.name}: {status} (reused)",
                    flush=True,
                )
                write_summaries(output, results)
                continue

            command = [str(executable), str(generated_path), top_name]
            if args.config:
                command.append(str(args.config.resolve()))
            if args.freq:
                command.extend(["--freq", args.freq])
            if args.debug:
                command.append("DEBUG")
            environment = os.environ.copy()
            environment["HI_CLOCK_PERIOD"] = args.clock
            environment["HI_HLS_LIB_PATH"] = str(root / "HLS_Data_Lib") + "/"
            include_path = str(original_path.parent)
            if environment.get("CPATH"):
                include_path += os.pathsep + environment["CPATH"]
            environment["CPATH"] = include_path

            started = time.monotonic()
            status = "ERROR"
            returncode = None
            with log_path.open("w", encoding="utf-8") as log:
                try:
                    completed = subprocess.run(
                        command,
                        cwd=run_dir,
                        env=environment,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        timeout=args.timeout,
                        check=False,
                    )
                    returncode = completed.returncode
                    log_text = log_path.read_text(
                        encoding="utf-8", errors="replace"
                    )
                    parsed = parse_result(log_text)
                    if (
                        returncode == 0
                        and "Accomplished LLVM processing" in log_text
                        and parsed["loaded_source_pragmas"]
                        == common["expected_pragmas"]
                        and parsed["latency"] is not None
                    ):
                        status = "PASS"
                    else:
                        status = "FAIL"
                except subprocess.TimeoutExpired:
                    status = "TIMEOUT"
                    log_text = log_path.read_text(
                        encoding="utf-8", errors="replace"
                    )
                    parsed = parse_result(log_text)

            elapsed = time.monotonic() - started
            status, baseline_reason = classify_baseline_failure(status, case_name)
            status, pipeline_reason = classify_pipeline_infeasible(status, log_text)
            if status == "PIPELINE_INFEASIBLE":
                for key in ("latency", "dsp", "ff", "lut", "bram"):
                    parsed[key] = None
            result = {
                **common,
                "status": status,
                "returncode": returncode,
                "elapsed_seconds": round(elapsed, 3),
                **parsed,
            }
            if baseline_reason:
                result["reason"] = baseline_reason
            if pipeline_reason:
                result["reason"] = pipeline_reason
            if (
                result["loaded_source_pragmas"] is not None
                and result["loaded_source_pragmas"] != result["expected_pragmas"]
            ):
                result["reason"] = (
                    f"expected {result['expected_pragmas']} source pragmas, "
                    f"loaded {result['loaded_source_pragmas']}"
                )
            results.append(result)
            print(
                f"[{completed_count:03d}/{total}] {case_name}/{variant.name}: "
                f"{status} (rc={returncode}, {elapsed:.1f}s, "
                f"latency={result['latency']})",
                flush=True,
            )
            write_summaries(output, results)

    counts = {
        status: sum(result["status"] == status for result in results)
        for status in (
            "PASS",
            "FAIL",
            "BASELINE_FAIL",
            "TIMEOUT",
            "NOT_APPLICABLE",
            "UNSUPPORTED_AP_INT",
            "PIPELINE_INFEASIBLE",
            "GENERATED",
        )
    }
    print(
        "SUMMARY: "
        + ", ".join(f"{name}={count}" for name, count in counts.items() if count)
        + f"; {output / 'summary.csv'}",
        flush=True,
    )
    unsuccessful = counts["FAIL"] + counts["TIMEOUT"]
    return 1 if unsuccessful else 0


if __name__ == "__main__":
    raise SystemExit(main())
