"""Compare fixed-budget Codex workflows on native Light-HLS scheduling graphs."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes
import json
import os
from pathlib import Path
import random
import re
import subprocess
import tempfile
from threading import Lock
import time

from flow_schedule import check, exact_single_port, list_schedule, port_delays
from run_round import HERE, ROOT, aggregation, digest, sandbox, write_json


DEV = ("lookup_xor", "mix_words")
HOLDOUT = ("weighted_sum", "pack_channels")
ARMS = ("independent", "feedback", "review")
COMPILER = ROOT / "Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top"
SOURCE = HERE / "flow_pilot.c"
SCHEDULER = ROOT / "Light_HLS/Implementations/HI_OperationScheduling/HI_OperationScheduling.cc"
FREQUENCY = "250MHz"
MODEL_TIMEOUT = 150
POLICY_TIMEOUT = 3
CALLS_PER_TRIAL = 3
EVAL_LOCK = Lock()


def read_json(path):
    return json.loads(path.read_text())


def run_logged(command, folder, label, *, env=None, timeout=60):
    path = folder / f"{label}.log"
    with path.open("wb") as log:
        result = subprocess.run(command, cwd=folder, env=env, stdout=log,
                                stderr=subprocess.STDOUT, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{label} exited {result.returncode}: {path.read_text(errors='replace')[-700:]}")
    return path


def compile_case(compiler, name, folder, starts=None, *, source=SOURCE):
    env = dict(os.environ, HI_HLS_LIB_PATH=str(ROOT / "Light_HLS/HLS_Data_Lib"),
               HI_EXPERIMENT_SCHEDULE_GRAPH=str(folder / "graph.json"))
    env.pop("HI_EXPERIMENT_SCHEDULE_REPLAY", None)
    if starts is not None:
        schedule = folder / "start_cycles.json"
        write_json(schedule, starts)
        env["HI_EXPERIMENT_SCHEDULE_REPLAY"] = str(schedule)
    run_logged([str(compiler), str(source), name, "--board", "u250", "--freq", FREQUENCY,
                "--synthesis", "disable", "--output", str(folder / "hls")], folder, "compile", env=env)
    graph = read_json(folder / "graph.json")
    native_starts = [node["native_start"] for node in graph["nodes"]]
    if (starts is not None and native_starts != starts) or check(graph, native_starts) != graph["native_cycles"]:
        raise RuntimeError("exported graph did not reproduce the compiler's schedule")
    return graph


def write_vectors(folder, *, source=SOURCE, cases=DEV + HOLDOUT):
    library = folder / "oracle.so"
    run_logged(["cc", "-O2", "-shared", "-fPIC", str(source), "-o", str(library)], folder, "oracle")
    oracle = ctypes.CDLL(str(library))
    rng = random.Random(731)
    vectors = [([0] * 8, 0), ([0xFFFFFFFF] * 8, 0xFFFFFFFF)]
    vectors += [([rng.getrandbits(32) for _ in range(8)], rng.getrandbits(32)) for _ in range(30)]
    for name in cases:
        ref = getattr(oracle, name)
        ref.argtypes = (ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32)
        ref.restype = ctypes.c_uint32
        rows = []
        for memory, key in vectors:
            result = ref((ctypes.c_uint32 * 8)(*memory), key)
            rows.append("".join(f"{x:08x}" for x in reversed(memory + [key, result])))
        (folder / f"{name}.hex").write_text("\n".join(rows) + "\n")


def simulate(name, folder, vectors):
    run_logged(["iverilog", "-g2012", "-s", "pilot_tb", f"-DCASE_TOP={name}", "-o", "sim",
                str(folder / "hls/reports/rtl/top.v"), str(HERE / "flow_pilot_tb.v")], folder, "iverilog")
    log = run_logged(["vvp", "sim", f"+vectors={vectors}"], folder, "rtl")
    match = re.search(r"RTL_CYCLES=(\d+) VECTORS=32", log.read_text())
    if not match:
        raise RuntimeError("missing RTL result")
    return int(match[1])


def replay(compiler, name, folder, vectors, starts, *, source=SOURCE):
    folder.mkdir(parents=True)
    try:
        graph = compile_case(compiler, name, folder, starts, source=source)
        cycles = simulate(name, folder, vectors)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        reason = str(error)
        diagnostic = re.search(r"Light-HLS backend error \[(EXPERIMENT_SCHEDULE_\w+)\]: ([^\r\n]+)", reason)
        return {"status": "rtl_replay_failed", "reason": reason,
                "diagnostic": ": ".join(diagnostic.groups()) if diagnostic else type(error).__name__}
    return {"status": "valid", "compiler_cycles": graph["native_cycles"], "rtl_cycles": cycles}


def prepare(compiler, output, *, source=SOURCE, cases=DEV + HOLDOUT):
    vectors = output / "vectors"
    vectors.mkdir()
    write_vectors(vectors, source=source, cases=cases)
    paths, reference = {}, {}
    for name in cases:
        folder = output / "native" / name
        folder.mkdir(parents=True)
        graph = compile_case(compiler, name, folder, source=source)
        vector_file = vectors / f"{name}.hex"
        native_cycles = simulate(name, folder, vector_file)
        optimum, exact_starts = exact_single_port(graph)
        tail_starts = list_schedule(graph, lambda op: op["tail"])
        reference[name] = {
            "status": "valid", "makespan": graph["native_cycles"], "rtl_cycles": native_cycles,
            "port_delays": port_delays(graph, [n["native_start"] for n in graph["nodes"]]),
            "exact_graph_cycles": optimum,
            "exact_replay": replay(compiler, name, output / "exact" / name, vector_file, exact_starts, source=source),
            "tail_graph_cycles": check(graph, tail_starts),
            "tail_replay": replay(compiler, name, output / "tail" / name, vector_file, tail_starts, source=source),
        }
        paths[name] = folder / "graph.json"
    write_json(output / "baseline.json", reference)
    return paths, reference


def model_call(prompt, fields, folder, model, effort):
    folder.mkdir()
    (folder / "prompt.txt").write_text(prompt)
    schema = {"type": "object", "properties": {key: {"type": "string"} for key in fields},
              "required": list(fields), "additionalProperties": False}
    started = time.monotonic()
    response, status = None, "generation_timeout"
    with tempfile.TemporaryDirectory(prefix="lighthls-flow-model-") as temporary:
        work = Path(temporary)
        write_json(work / "schema.json", schema)
        command = ["codex", "exec", "--ignore-user-config", "--ephemeral", "--skip-git-repo-check",
                   "--sandbox", "read-only", "--disable", "shell_tool", "--disable", "multi_agent_v2",
                   "-c", "agents.enabled=false", "-c", 'web_search="disabled"',
                   "-c", f'model_reasoning_effort="{effort}"', "--model", model, "--json",
                   "--output-schema", str(work / "schema.json"),
                   "--output-last-message", str(work / "reply.json"), "-"]
        events_path = folder / "events.jsonl"
        with events_path.open("w") as events, (folder / "stderr.log").open("w") as stderr:
            try:
                result = subprocess.run(command, cwd=work, text=True, input=prompt, stdout=events,
                                        stderr=stderr, timeout=MODEL_TIMEOUT)
            except subprocess.TimeoutExpired:
                result = None
        if result is not None:
            status = f"generation_failed_{result.returncode}"
            if result.returncode == 0:
                try:
                    reply = read_json(work / "reply.json")
                    if any(not isinstance(reply.get(k), str) or not reply[k].strip()
                           or len(reply[k]) > 4096 for k in fields):
                        raise ValueError("unexpected response fields")
                except (AttributeError, OSError, ValueError):
                    status = "invalid_model_response"
                else:
                    status, response = "generated", reply
                    write_json(folder / "response.json", response)
    usage = {}
    for line in events_path.read_text().splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("type") == "turn.completed":
            usage = event.get("usage", {})
    receipt = {"status": status, "seconds": time.monotonic() - started, "usage": usage}
    write_json(folder / "call.json", receipt)
    print(f"{folder.parent.name}/{folder.name} {status}", flush=True)
    return response, receipt


def evaluate(policy, graph_path, folder, cpu, *, trace=False):
    candidate = folder / "candidate"
    candidate.mkdir(parents=True)
    command = ["taskset", "-c", str(cpu)] + sandbox() + [
        "--ro-bind", str(policy), "/policy.py", "--ro-bind", str(graph_path), "/graph.json",
        "--ro-bind", str(HERE / "flow_schedule.py"), "/worker.py",
        "--bind", str(candidate), "/output", "/usr/bin/python3", "-I", "-B", "/worker.py",
    ]
    if trace:
        command.append("--trace")
    with EVAL_LOCK:
        started = time.monotonic()
        try:
            run_logged(command, folder, "policy", timeout=POLICY_TIMEOUT)
        except subprocess.TimeoutExpired:
            return {"status": "timeout", "seconds": time.monotonic() - started}
        except RuntimeError as error:
            reason = str(error)
            diagnostic = re.search(r"(?m)^(\w+Error: [^\n]{1,240})$", reason)
            return {"status": "execution_error", "seconds": time.monotonic() - started, "reason": reason,
                    "diagnostic": diagnostic[1] if diagnostic else "policy process failed"}
        seconds = time.monotonic() - started
    try:
        schedule = candidate / "schedule.json"
        if schedule.is_symlink() or not schedule.is_file() or schedule.stat().st_size > 1 << 20:
            raise ValueError("missing or invalid schedule file")
        starts = read_json(schedule)
        graph = read_json(graph_path)
        cycles = check(graph, starts)
    except (OSError, ValueError) as error:
        return {"status": "invalid", "seconds": seconds, "reason": str(error)}
    return {"status": "valid", "seconds": seconds, "makespan": cycles, "starts": starts,
            "port_delays": port_delays(graph, starts)}


def trial(arm, repeat, output, graphs, baseline, compiler, model, effort, cpu, prompt):
    directory = output / f"{arm}_{repeat:02d}"
    directory.mkdir()
    rows, calls = [], []
    previous, review_message = "", ""
    for call in range(1, CALLS_PER_TRIAL + 1):
        folder = directory / f"call_{call:02d}"
        if arm == "review" and call == 2:
            review_prompt = prompt + "\nAct as an independent reviewer. Return a short modification suggestion in review. " \
                "Reason from the recorded graph results and specific port contenders. Do not write the policy.\n" + previous
            response, receipt = model_call(review_prompt, ("review",), folder, model, effort)
            calls.append({"role": "reviewer", **receipt})
            if response:
                review_message = "\nIndependent review:\n" + response["review"]
            continue
        suffix = "" if arm == "independent" else previous + review_message
        response, receipt = model_call(prompt + suffix, ("code", "strategy"), folder, model, effort)
        calls.append({"role": "author", **receipt})
        if not response:
            continue
        code = folder / "policy.py"
        code.write_text(response["code"] + "\n")
        scores, rtl_scores = {}, {}
        for name in DEV:
            graph_score = evaluate(code, graphs[name], folder / name, cpu)
            if graph_score["status"] == "valid":
                rtl = replay(compiler, name, folder / name / "rtl_replay",
                             output / "vectors" / f"{name}.hex", graph_score["starts"])
                graph_score["rtl"] = rtl
                rtl_scores[name] = {"status": rtl["status"], "makespan": rtl.get("rtl_cycles"),
                                    "seconds": graph_score["seconds"]}
            else:
                rtl_scores[name] = graph_score
            scores[name] = graph_score
        native_rtl = {name: {"makespan": row["rtl_cycles"]} for name, row in baseline.items()}
        summary = aggregation(rtl_scores, native_rtl)
        row = {"call": call, "policy": str(code.relative_to(output)), "sha256": digest(code),
               "strategy": response["strategy"], "scores": scores, "summary": summary}
        rows.append(row)
        write_json(folder / "scores.json", row)
        # Process timing and local stderr paths are excluded from model feedback.
        evidence = {name: {k: v for k, v in result.items()
                           if k in {"status", "makespan", "starts", "port_delays", "diagnostic"}}
                    for name, result in scores.items()}
        for name, score in scores.items():
            if "rtl" in score:
                evidence[name]["rtl"] = {k: v for k, v in score["rtl"].items() if k != "reason"}
        previous = ("\nRefine this previous policy using the development evidence.\n"
                    f"Policy:\n{response['code']}\nScores and delayed requests:\n{json.dumps(evidence)}\n")
    ranked = [row for row in rows if row["summary"]["valid"] > 0]
    selected = min(ranked, key=lambda row: (-row["summary"]["valid"], row["summary"]["geomean_ratio"],
                                           row["call"])) if ranked else None
    result = {"arm": arm, "repeat": repeat, "calls": calls, "candidates": rows, "selected": selected}
    write_json(directory / "selection.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=COMPILER)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", default="gpt-6.1-sol")
    parser.add_argument("--effort", default="low", choices=("low", "medium"))
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--calibrate-only", action="store_true")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("repeats must be positive")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    compiler = args.compiler.resolve()
    cpu = min(os.sched_getaffinity(0))
    graphs, baseline = prepare(compiler, output)
    print("Native graph and RTL calibration:", {n: (r["makespan"], r["exact_graph_cycles"], r["rtl_cycles"])
                                              for n, r in baseline.items()}, flush=True)
    if args.calibrate_only:
        return
    subprocess.run(sandbox() + ["/usr/bin/true"], check=True, timeout=10)
    protocol = {"dev": list(DEV), "holdout": list(HOLDOUT), "model": args.model, "effort": args.effort,
                "repeats": args.repeats, "calls_per_trial": CALLS_PER_TRIAL, "cpu": cpu, "clock": FREQUENCY,
                "model_timeout": MODEL_TIMEOUT, "policy_timeout": POLICY_TIMEOUT,
                "selection": "maximize developmental RTL passes, minimize RTL cycle geomean, then earliest call",
                "source_sha256": {str(p.relative_to(ROOT)): digest(p)
                                  for p in (Path(__file__).resolve(), SOURCE, SCHEDULER,
                                            HERE / "flow_schedule.py", HERE / "flow_pilot_tb.v")},
                "graphs_sha256": {name: digest(path) for name, path in graphs.items()},
                "codex_version": subprocess.check_output(["codex", "--version"], text=True).strip(),
                "git_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()}
    write_json(output / "protocol.json", protocol)
    cases = {name: {"graph": read_json(graphs[name]), "reference_cycles": baseline[name]["exact_graph_cycles"]}
             for name in DEV}
    prompt = """Design a reusable priority rule for a Light-HLS static list scheduler.
Return JSON code and strategy. The code must define only a Python function priority(op) returning a finite number.
op is a plain Python dict. Read a feature with op["tail"] or op.get("tail", 0).
The scheduler chooses the highest score among ready operations that fit this cycle's ports and breaks
ties by native operation order. It reconsiders ready nodes after each choice, including zero-latency nodes.
Each memory request uses its listed ports during the issue cycle only. Dependencies require completion.
Features available in op: opcode, latency, tail (remaining weighted path including return overhead),
fanout, downstream_requests (max number of later port requests along a dependency path), uses_port,
cycle, competing_requests (ready nodes sharing a port). Use only these features, no node or case IDs.
Prefer fewer block cycles. Keep code under 35 lines, deterministic and standard library only.
The graphs show native start cycles, edges [producer, consumer] and an exact small-graph cycle-count reference.
Light-HLS replays the proposed starts and Icarus Verilog checks the RTL against native C.
Graph legality alone can miss load-response forwarding constraints that depend on the schedule.
Selection prioritizes passing RTL replay on both development cases, then minimizing RTL cycles.
Development graphs:\n""" + json.dumps(cases, separators=(",", ":")) + "\n"
    identifiers = [(arm, repeat) for arm in ARMS for repeat in range(1, args.repeats + 1)]
    with ThreadPoolExecutor(max_workers=min(6, len(identifiers))) as pool:
        futures = [pool.submit(trial, arm, repeat, output, graphs, baseline,
                               compiler, args.model, args.effort, cpu, prompt)
                   for arm, repeat in identifiers]
        trials = [future.result() for future in futures]
    write_json(output / "selected.json", [r["selected"] for r in trials])
    for record in trials:
        record["holdout"] = {}
        if record["selected"] is None:
            continue
        policy = output / record["selected"]["policy"]
        directory = output / f"{record['arm']}_{record['repeat']:02d}" / "holdout"
        for name in HOLDOUT:
            result = evaluate(policy, graphs[name], directory / name, cpu)
            if result["status"] == "valid":
                result["rtl"] = replay(compiler, name, directory / name / "rtl_replay",
                                       output / "vectors" / f"{name}.hex", result["starts"])
            record["holdout"][name] = result
    write_json(output / "results.json", {"baseline": baseline, "trials": trials})
    print(f"Completed {len(trials)} trajectories. Results in {output / 'results.json'}", flush=True)


if __name__ == "__main__":
    main()
