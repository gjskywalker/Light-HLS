#!/usr/bin/env python3
"""Compare independent and feedback-driven Codex scheduling candidates."""

import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import time


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SOURCES = ROOT / "benchmarks/hls_scheduling_2024_2026/sources"
DATA = SOURCES / "heurigym_dataset/operator_scheduling"
EXPERT = SOURCES / "heurigym_tools/operator_scheduling/baseline/baseline.json"
# Keep the graph-size series and paired FIR/JPEG families together.
HOLDOUT = {"dag_500", "dag_1000", "dag_1500", "fir1", "fir2", "jpeg_fdct", "jpeg_idct"}
RUN_TIMEOUT = 10
GENERATION_TIMEOUT = 240
OUTPUT_SCHEMA = {
    "type": "object",
    "properties": {"code": {"type": "string"}, "strategy": {"type": "string"}},
    "required": ["code", "strategy"],
    "additionalProperties": False,
}


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False, allow_nan=False) + "\n")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sandbox():
    return [
        "bwrap", "--unshare-all", "--die-with-parent", "--clearenv",
        "--ro-bind", "/usr", "/usr", "--symlink", "usr/lib", "/lib",
        "--symlink", "usr/lib64", "/lib64", "--dev", "/dev", "--proc", "/proc",
        "--tmpfs", "/tmp", "--chdir", "/tmp",
    ]


def score(graph, schedule_path):
    kinds = dict(graph["nodes"])
    durations = graph["delay"]
    starts = {}
    if schedule_path.is_symlink() or not schedule_path.is_file():
        raise ValueError("missing regular schedule file")
    if schedule_path.stat().st_size > 2 << 20:
        raise ValueError("schedule file exceeds 2 MiB")
    for line in schedule_path.read_text().splitlines():
        node, separator, raw_cycle = line.partition(":")
        node = node.strip()
        raw_cycle = raw_cycle.strip()
        if not separator or not re.fullmatch(r"[0-9]{1,12}", raw_cycle):
            raise ValueError("expected node:nonnegative_integer on each line")
        if node not in kinds or node in starts:
            raise ValueError(f"unknown or duplicate node {node[:80]}")
        starts[node] = int(raw_cycle)
    if starts.keys() != kinds.keys():
        raise ValueError(f"missing nodes {sorted(kinds.keys() - starts.keys())[:8]}")
    ends = {node: start + durations[kinds[node]] for node, start in starts.items()}
    for source, target, _ in graph["edges"]:
        if ends[source] > starts[target]:
            raise ValueError(f"{source} finishes at {ends[source]}, {target} starts at {starts[target]}")

    events = defaultdict(Counter)
    for node, kind in kinds.items():
        events[kind][starts[node]] += 1
        events[kind][ends[node]] -= 1
    for kind, changes in events.items():
        occupied = 0
        for cycle in sorted(changes):
            occupied += changes[cycle]
            if occupied > graph["resource"][kind]:
                raise ValueError(f"{kind} occupancy {occupied} exceeds {graph['resource'][kind]} at {cycle}")
    return max(ends.values())


def run_solver(code, graph_path, output, cpu=None):
    candidate_output = output / "candidate"
    candidate_output.mkdir(parents=True)
    command = sandbox() + [
        "--ro-bind", str(code), "/solver.py", "--ro-bind", str(graph_path), "/graph.json",
        "--ro-bind", str(HERE / "candidate_worker.py"), "/worker.py",
        "--bind", str(candidate_output), "/output", "/usr/bin/python3", "-I", "-B", "/worker.py",
    ]
    if cpu is not None:
        command = ["taskset", "-c", str(cpu)] + command
    with (output / "stdout.log").open("wb") as stdout, (output / "stderr.log").open("wb") as stderr:
        started = time.perf_counter()
        try:
            process = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=RUN_TIMEOUT)
        except subprocess.TimeoutExpired:
            return {"status": "timeout", "seconds": time.perf_counter() - started}
        elapsed = time.perf_counter() - started
    if process.returncode != 0:
        reason = (output / "stderr.log").read_bytes()[-600:].decode(errors="replace").strip()
        return {"status": "execution_error", "seconds": elapsed, "reason": reason}
    try:
        makespan = score(json.loads(graph_path.read_text()), candidate_output / "schedule.txt")
    except (OSError, ValueError) as error:
        return {"status": "invalid", "seconds": elapsed, "reason": str(error)}
    return {"status": "valid", "makespan": makespan, "seconds": elapsed}


def aggregation(results, baseline):
    valid = [name for name, row in results.items() if row["status"] == "valid"]
    return {
        "valid": len(valid),
        "total": len(results),
        "geomean_ratio": statistics.geometric_mean(
            results[name]["makespan"] / baseline[name]["makespan"] for name in valid
        ) if valid else None,
        "median_process_seconds": statistics.median(results[name]["seconds"] for name in valid) if valid else None,
    }


def compare(code, paths, directory, cpu=None):
    directory.mkdir()
    return {path.stem: run_solver(code, path, directory / path.stem, cpu) for path in paths}


def initial_prompt(demo):
    graphs = {path.stem: json.loads(path.read_text()) for path in demo}
    return f"""Develop a general-purpose Python 3.10 solver for resource-constrained operator scheduling.
Return the full source as `code` and a concise algorithm description as `strategy`.
Implement solve(input_file: str, solution_file: str). Standard library only.
Input JSON has nodes [id, type], edges [source, target, edge_name], delay by type,
and resource (capacity) by type. Each operation starts at a nonnegative integer
cycle and occupies one unit throughout [start, start + delay). A successor can
start only after each predecessor completes. Output exactly one `id:start` line
per node. Minimize max(start + delay). Preserve correctness on every valid DAG.
The solver has one independent process and 10 seconds per case. Reserve time for
a valid solution and bound any optimization search. Evaluation prioritizes legal
coverage, then geometric mean of makespan relative to critical-path list scheduling.
Use the public demonstration graphs below to reason about the format and constraints.
Keep the solver general and deterministic. No case-specific solutions, hard-coded
instances, external tools, or network. Return the requested JSON without tool calls.

Demo graphs:
{json.dumps(graphs, separators=(',', ':'))}
"""


def feedback_prompt(previous, baseline):
    previous_demo = {
        name: {**{k: v for k, v in row.items() if k in {"status", "makespan", "reason"}},
               "baseline_makespan": baseline[name]["makespan"]}
        for name, row in previous["demo"].items()
    }
    return (
        "\nThis is the immediately preceding candidate. Diagnose its behavior on the demos, "
        "then submit the best complete general solver you can under the same contract. "
        "Preserve valid behavior and try a substantive improvement.\n"
        f"Previous strategy: {previous['strategy']}\nPrevious demo results: {json.dumps(previous_demo)}\n"
        f"Previous Python code:\n{previous['code'].read_text() if previous['code'] else '(no usable code)'}\n"
    )


def generate(prompt, folder, model, effort):
    (folder / "prompt.txt").write_text(prompt)
    with tempfile.TemporaryDirectory(prefix="codex-dag-prompt-") as temporary:
        isolated = Path(temporary)
        response = isolated / "response.json"
        schema = isolated / "schema.json"
        write_json(schema, OUTPUT_SCHEMA)
        command = [
            "codex", "exec", "--ignore-user-config", "--ephemeral", "--skip-git-repo-check",
            "--sandbox", "read-only", "--disable", "shell_tool", "--disable", "multi_agent_v2",
            "-c", 'web_search="disabled"', "-c", f'model_reasoning_effort="{effort}"',
            "--model", model, "--color", "never", "--json", "--output-schema", str(schema),
            "--output-last-message", str(response), "-",
        ]
        with (folder / "model_events.jsonl").open("w") as events, (folder / "model_stderr.log").open("w") as errors:
            try:
                process = subprocess.run(command, cwd=isolated, input=prompt, text=True,
                                         stdout=events, stderr=errors, timeout=GENERATION_TIMEOUT)
            except subprocess.TimeoutExpired:
                return None, "generation_timeout", ""
        if process.returncode != 0:
            return None, f"generation_failed_{process.returncode}", ""
        try:
            parsed = json.loads(response.read_text())
            source = parsed["code"]
            strategy = parsed["strategy"]
            if not isinstance(source, str) or len(source.encode()) > 65536 or not source.strip():
                raise ValueError("invalid code size")
        except (OSError, ValueError, KeyError, TypeError):
            return None, "invalid_model_response", ""
        code = folder / "solver.py"
        code.write_text(source + "\n")
        return code, "generated", strategy


def select(group):
    def order(row):
        summary = row["validation_summary"]
        ratio = summary["geomean_ratio"]
        return (-summary["valid"], ratio if ratio is not None else math.inf, row["round"])
    return min(group, key=order)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--model", default="gpt-6.1-sol")
    parser.add_argument("--effort", default="medium", choices=["low", "medium", "high"])
    parser.add_argument("--rounds", default=10, type=int)
    parser.add_argument("--parallel", default=3, type=int)
    args = parser.parse_args()
    if args.rounds < 1 or args.parallel < 1:
        parser.error("rounds and parallel must be positive")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    cases = sorted((DATA / "eval").glob("*.json"))
    split = {
        "demo": sorted((DATA / "demo").glob("*.json")),
        "validation": [path for path in cases if path.stem not in HOLDOUT],
        "holdout": [path for path in cases if path.stem in HOLDOUT],
    }
    if [len(paths) for paths in split.values()] != [5, 12, 7]:
        raise ValueError("unexpected size of the pinned HeuriGym collection")
    cpu = min(os.sched_getaffinity(0))
    protocol = {
        "git_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "codex_version": subprocess.check_output(["codex", "--version"], text=True).strip(),
        "model": args.model, "effort": args.effort, "rounds_per_arm": args.rounds,
        "split": {name: [{"path": str(path.relative_to(ROOT)), "sha256": digest(path)} for path in paths]
                  for name, paths in split.items()},
        "experiment_sources": {path.name: digest(path) for path in sorted(HERE.glob("*.py"))},
        "expert_baseline_sha256": digest(EXPERT), "solver_wall_timeout_seconds": RUN_TIMEOUT,
        "model_wall_timeout_seconds": GENERATION_TIMEOUT, "holdout_cpu": cpu,
        "selection": "maximize validation legal coverage, minimize geomean ratio, then prefer earliest round",
        "feedback": "immediate prior candidate code and demo scores only",
    }
    write_json(out / "protocol.json", protocol)
    probe = subprocess.run(sandbox() + ["/usr/bin/true"], capture_output=True, text=True, timeout=10)
    if probe.returncode:
        raise RuntimeError(f"failed to initialize candidate sandbox: {probe.stderr.strip()}")
    baseline_code = HERE / "baseline.py"
    baseline_demo = compare(baseline_code, split["demo"], out / "baseline_demo")
    baseline_val = compare(baseline_code, split["validation"], out / "baseline_validation")
    baseline = {**baseline_demo, **baseline_val}
    if any(row["status"] != "valid" for row in baseline.values()):
        raise ValueError(f"baseline failed: {baseline}")
    write_json(out / "baseline_development.json", baseline)
    print(f"Baseline legal on {len(baseline)}/{len(baseline)} development graphs", flush=True)
    base = initial_prompt(split["demo"])

    def iteration(group, index, previous=None):
        folder = out / f"{group}_{index:02d}"
        folder.mkdir()
        print(f"{group} {index:02d} generation started", flush=True)
        prompt = base + (feedback_prompt(previous, baseline_demo) if previous else "")
        code, generation, strategy = generate(prompt, folder, args.model, args.effort)
        demo = compare(code, split["demo"], folder / "demo") if code else {}
        validation = compare(code, split["validation"], folder / "validation") if code else {}
        demo = demo or {path.stem: {"status": generation} for path in split["demo"]}
        validation = validation or {path.stem: {"status": generation} for path in split["validation"]}
        summary = aggregation(validation, baseline_val)
        row = {"group": group, "round": index, "generation": generation, "strategy": strategy,
               "demo": demo, "validation": validation, "validation_summary": summary,
               "solver_path": str(code.relative_to(out)) if code else None, "sha256": digest(code) if code else None}
        write_json(folder / "receipt.json", row)
        print(f"{group} {index:02d} {generation} validation {summary}", flush=True)
        return {**row, "code": code}

    feedback = []
    with ThreadPoolExecutor(max_workers=args.parallel) as pool:
        independent_futures = [pool.submit(iteration, "independent", index) for index in range(1, args.rounds + 1)]
        for index in range(1, args.rounds + 1):
            feedback.append(iteration("feedback", index, feedback[-1] if feedback else None))
        independent = [future.result() for future in independent_futures]

    winners = {"independent": select(independent), "feedback": select(feedback)}
    chosen = {group: {key: winner[key] for key in ("round", "strategy", "sha256", "solver_path", "validation_summary")}
              for group, winner in winners.items()}
    write_json(out / "selected.json", chosen)
    # Holdout stays outside model prompts and the candidate selection function.
    baseline_test = compare(baseline_code, split["holdout"], out / "baseline_holdout", cpu)
    if any(row["status"] != "valid" for row in baseline_test.values()):
        raise ValueError(f"baseline holdout failed: {baseline_test}")
    expert = json.loads(EXPERT.read_text())
    scores = {"baseline": baseline_test}
    for group, winner in winners.items():
        if winner["code"]:
            scores[group] = compare(winner["code"], split["holdout"], out / f"{group}_holdout", cpu)
        else:
            scores[group] = {path.stem: {"status": "no_candidate_generated"} for path in split["holdout"]}
    results = {
        "selection": chosen, "holdout": scores,
        "holdout_summary": {name: aggregation(rows, baseline_test) for name, rows in scores.items()},
        "expert_reference": {path.stem: expert[path.stem] for path in split["holdout"]},
        "generation_counts": {name: dict(Counter(r["generation"] for r in rows))
                              for name, rows in (("independent", independent), ("feedback", feedback))},
        "validation_history": {name: [r["validation_summary"] for r in rows]
                               for name, rows in (("independent", independent), ("feedback", feedback))},
    }
    write_json(out / "results.json", results)
    print(json.dumps({"selected": chosen, "holdout": results["holdout_summary"],
                      "generation": results["generation_counts"]}, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    main()
