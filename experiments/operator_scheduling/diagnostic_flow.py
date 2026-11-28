"""Controlled result-only, fixed-diagnostic and agent-selected diagnostic workflows."""

import argparse
import ast
from concurrent.futures import ThreadPoolExecutor
import json
import math
import os
from pathlib import Path
import re
import subprocess

from flow_pilot import (COMPILER, FREQUENCY, HERE, MODEL_TIMEOUT, POLICY_TIMEOUT, ROOT, SCHEDULER,
                        SOURCE, digest, evaluate, model_call, prepare, read_json, replay, sandbox, write_json)


DEV = ("lookup_xor", "mix_words", "weighted_sum", "dot_product", "bit_extract")
HOLDOUT = ("crc_step", "wavelet_lift", "hamming_distance", "majority_vote", "linear_interp",
           "horner_eval", "table_blend", "sum_squares")
CASES = DEV + HOLDOUT
FIXTURE = HERE / "diagnostic_kernels.c"
ARMS = ("A", "B", "C")
ROUNDS = 2
SEED = "def priority(op):\n    return 0\n"
FEATURES = ("opcode", "latency", "tail", "fanout", "downstream_requests", "uses_port",
            "cycle", "competing_requests")
BUILTINS = {"int", "float", "min", "max", "abs", "bool"}
CONTRACT = """Develop a reusable priority(op) rule for a real Light-HLS static list scheduler.
The scheduler picks the highest score among ready operations fitting this cycle's ports, breaks ties by
native order and reconsiders after each issue (even after zero-latency ops). A port is occupied only on
issue. Load forwarding can change when starts move: compiler replay and RTL vs C decide acceptance.
op is a Python dict: opcode (LLVM string), latency, tail (weighted longest remaining dependency path
including return), fanout, downstream_requests (max future port requests along a path), uses_port
(bool), cycle, competing_requests (other ready nodes sharing a port).
Write only def priority(op), a deterministic finite numeric score. Use op["tail"] style literal reads,
local assignments, if/elif, arithmetic, comparisons, and min/max/int/float/abs/bool. No helpers,
imports, loops, mutation, annotations, IDs, design names or access outside op. First maximize RTL
passes on all development cases, then minimize geometric mean RTL cycles vs native. Aim to transfer
across unseen kernels. A tie retains the earlier candidate. A planned optimization should say what
observation would confirm or refute the hypothesis.
"""
MENU = ("Available query strings are CASE/overview (port delays, static longest dependency chain and "
        "replay), CASE/cycle/N (decision-by-decision ready, eligible and chosen operations in cycle N), "
        "CASE/diff (starts changed vs parent, or vs native for seed), CASE/replay (backend status). "
        "Use at most one query per budget slot, comma-separated. Cases must be development cases.")


def policy_error(code):
    """Validate model code before the usual no-network, isolated worker executes it."""
    try:
        tree = ast.parse(code)
    except (SyntaxError, ValueError) as exc:
        return str(exc).splitlines()[0]
    if len(tree.body) != 1 or not isinstance(tree.body[0], ast.FunctionDef):
        return "expected exactly one function"
    fn = tree.body[0]
    if (fn.name != "priority" or [arg.arg for arg in fn.args.args] != ["op"] or fn.args.posonlyargs
            or fn.args.kwonlyargs or fn.args.vararg or fn.args.kwarg or fn.args.defaults
            or fn.decorator_list or fn.returns or fn.args.args[0].annotation):
        return "expected def priority(op)"
    locals_ = {node.id for node in ast.walk(fn) if isinstance(node, ast.Name)
               and isinstance(node.ctx, ast.Store)}
    if locals_ & (BUILTINS | {"op"}):
        return "cannot overwrite op or a builtin"
    allowed = (ast.arguments, ast.arg, ast.Return, ast.If, ast.IfExp, ast.Assign, ast.AugAssign,
               ast.Expr, ast.Name, ast.Load, ast.Store, ast.Constant, ast.Subscript, ast.Call,
               ast.BinOp, ast.UnaryOp, ast.BoolOp, ast.Compare, ast.And, ast.Or, ast.Not,
               ast.UAdd, ast.USub, ast.Add, ast.Sub, ast.Mult, ast.Div, ast.FloorDiv, ast.Mod,
               ast.Gt, ast.GtE, ast.Lt, ast.LtE, ast.Eq, ast.NotEq)
    for node in ast.walk(fn):
        if node is fn:
            continue
        if not isinstance(node, allowed):
            return f"unsupported policy syntax: {type(node).__name__}"
        if isinstance(node, ast.Name) and node.id not in locals_ | BUILTINS | {"op"}:
            return f"unknown policy name: {node.id}"
        if isinstance(node, ast.Call) and not (isinstance(node.func, ast.Name) and node.func.id in BUILTINS):
            return "only basic numeric builtins may be called"
        if isinstance(node, ast.Subscript) and not (
                isinstance(node.ctx, ast.Load) and isinstance(node.value, ast.Name) and node.value.id == "op"
                and isinstance(node.slice, ast.Constant) and node.slice.value in FEATURES):
            return "use op with a literal supported feature"
    return None


def evaluate_cases(code, cases, directory, graphs, run, compiler, cpu):
    rows = {}
    error = policy_error(code.read_text())
    for name in cases:
        if error:
            rows[name] = {"status": "invalid_policy", "diagnostic": error}
            continue
        folder = directory / name
        row = evaluate(code, graphs[name], folder, cpu, trace=True)
        if row["status"] == "valid":
            row["rtl"] = replay(compiler, name, folder / "replay", run / "vectors" / f"{name}.hex",
                                row["starts"], source=FIXTURE)
        rows[name] = row
    return rows


def scalar(row):
    result = row.get("rtl", row)
    return {"status": result["status"], "cycles": result.get("rtl_cycles")}


def ranking(scores, baseline, index):
    passed = [scalar(row)["cycles"] / baseline[name]["rtl_cycles"] for name, row in scores.items()
              if scalar(row)["status"] == "valid"]
    return (-len(passed), math.exp(sum(map(math.log, passed)) / len(passed)) if passed else 1, index)


def longest_path(graph):
    parents = [[] for _ in graph["nodes"]]
    for src, dst in graph["edges"]:
        parents[dst].append(src)
    path, weights = [], []
    for i, node in enumerate(graph["nodes"]):
        prev = max(parents[i], key=weights.__getitem__, default=None)
        weights.append(node["latency"] + (weights[prev] if prev is not None else 0))
        path.append((path[prev] if prev is not None else []) + [i])
    return path[graph["return_node"]]


def diagnostic(request, record, ancestor, native_graphs, run):
    parts = request.split("/")
    if len(parts) < 2 or parts[0] not in DEV:
        return {"error": "unknown development case or query"}
    name = parts[0]
    row = record["scores"][name]
    result = row.get("rtl", row)
    replay_info = dict(scalar(row), diagnostic=result.get("diagnostic"))
    if parts[1:] == ["replay"] or not row.get("starts"):
        return replay_info
    graph = native_graphs[name]
    starts = row["starts"]

    def operation(i):
        return [i, graph["nodes"][i]["opcode"], starts[i]]

    if parts[1:] == ["overview"]:
        return {"replay": replay_info, "port_delays": row["port_delays"],
                "dependency_path_id_opcode_issue": [operation(i) for i in longest_path(graph)]}
    if parts[1:] == ["diff"]:
        prior = (ancestor["scores"][name].get("starts") if ancestor else
                 [node["native_start"] for node in graph["nodes"]])
        if prior is None:
            return {"error": "parent has no schedule"}
        return {"id_opcode_from_to": [[i, graph["nodes"][i]["opcode"], a, b]
                                      for i, (a, b) in enumerate(zip(prior, starts)) if a != b]}
    if len(parts) == 3 and parts[1] == "cycle" and re.fullmatch(r"[0-9]{1,3}", parts[2]):
        trace = read_json(run / record["folder"] / name / "candidate/trace.json")
        return {"steps": [step for step in trace if step["cycle"] == int(parts[2])],
                "id_opcode_issue": [operation(i) for i in range(len(starts))]}
    return {"error": "unknown query"}


def trajectory(arm, repeat, run, graphs, baseline, seed, compiler, cpu, model, effort):
    directory = run / f"{arm}_{repeat:02d}"
    directory.mkdir()
    dev_graphs = {name: read_json(graphs[name]) for name in DEV}
    archive = {0: seed}
    history, receipts = [], []
    for turn in range(1, ROUNDS + 1):
        step = directory / f"round_{turn}"
        step.mkdir()
        public = {i: {"code": (run / item["policy"]).read_text(),
                      "scores": {n: scalar(s) for n, s in item["scores"].items()}}
                  for i, item in archive.items()}
        context = (CONTRACT + "\nNative RTL cycles: " + json.dumps({n: baseline[n]["rtl_cycles"] for n in DEV})
                   + "\nCandidate archive: " + json.dumps(public) + "\nPast steps: " + json.dumps(history))
        request = ("\nSelect the parent by its integer archive key (string parent), and give one "
                   "short testable hypothesis to improve it.")
        fields = ["parent", "hypothesis"]
        if arm == "C":
            request += "\n" + MENU + f" Up to {len(DEV)} queries in the queries string."
            fields.append("queries")
        plan, receipt = model_call(context + request, fields, step / "plan", model, effort)
        receipts.append(receipt)
        try:
            parent = int(plan["parent"]) if plan else -1
        except ValueError:
            parent = -1
        if parent not in archive:
            parent = min(archive, key=lambda i: ranking(archive[i]["scores"], baseline, i))
        base = archive[parent]
        queries = []
        if arm == "B":
            queries = [f"{n}/overview" for n in DEV]
        elif arm == "C" and plan:
            queries = [s.strip() for s in plan["queries"].split(",") if s.strip()][:len(DEV)]
        reports = [{"query": q, "response": diagnostic(q, base, archive.get(base["parent"]), dev_graphs, run)}
                   for q in queries]
        step_data = {"round": turn, "parent": parent, "hypothesis": plan["hypothesis"] if plan else "",
                     "reports": reports}
        write_json(step / "diagnostics.json", step_data)
        response, receipt = model_call(context + "\nCurrent plan and evidence: " + json.dumps(step_data)
                                      + "\nReturn the complete priority function in code and a short strategy.",
                                      ["code", "strategy"], step / "author", model, effort)
        receipts.append(receipt)
        history.append(step_data)
        if response:
            code = step / "policy.py"
            code.write_text(response["code"] + "\n")
            scores = evaluate_cases(code, DEV, step, graphs, run, compiler, cpu)
            archive[turn] = {"index": turn, "parent": parent, "policy": str(code.relative_to(run)),
                             "folder": str(step.relative_to(run)), "sha256": digest(code),
                             "strategy": response["strategy"], "scores": scores}
            write_json(step / "scores.json", archive[turn])
    winner = min(archive, key=lambda i: ranking(archive[i]["scores"], baseline, i))
    result = {"arm": arm, "repeat": repeat, "calls": receipts, "steps": history, "selected": winner,
              "archive": list(archive.values())}
    write_json(directory / "selection.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, default=COMPILER)
    parser.add_argument("--model", default="gpt-6.1-sol")
    parser.add_argument("--effort", default="low")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--calibrate-only", action="store_true")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("repeats must be positive")
    compiler, run = args.compiler.resolve(), args.output.resolve()
    run.mkdir(parents=True, exist_ok=False)
    cpu = min(os.sched_getaffinity(0))
    protocol = {"dev": DEV, "holdout": HOLDOUT, "arms": ARMS, "rounds": ROUNDS, "repeats": args.repeats,
                "model": args.model, "effort": args.effort, "max_queries_per_round": len(DEV),
                "model_calls_per_round": 2, "max_development_evaluations_per_round": len(DEV),
                "model_timeout": MODEL_TIMEOUT, "policy_timeout": POLICY_TIMEOUT, "frequency": FREQUENCY,
                "selection": "maximize dev RTL coverage, minimize valid geomean ratio, earliest index including seed",
                "seed": SEED, "compiler_sha256": digest(compiler), "cpu": cpu,
                "codex": subprocess.check_output(["codex", "--version"], text=True).strip(),
                "git_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "source_sha256": {p.name: digest(p) for p in (Path(__file__), FIXTURE, SOURCE, SCHEDULER,
                                  HERE / "flow_pilot.py", HERE / "run_round.py", HERE / "flow_schedule.py",
                                  HERE / "flow_pilot_tb.v")}}
    write_json(run / "protocol.json", protocol)
    graphs, baseline = prepare(compiler, run, source=FIXTURE, cases=CASES)
    write_json(run / "graph_hashes.json", {n: digest(path) for n, path in graphs.items()})
    if args.calibrate_only:
        return
    subprocess.run(sandbox() + ["/usr/bin/true"], check=True, timeout=10)
    seed_dir = run / "seed"
    seed_dir.mkdir()
    code = seed_dir / "policy.py"
    code.write_text(SEED)
    seed = {"index": 0, "parent": None, "policy": "seed/policy.py", "folder": "seed", "sha256": digest(code),
            "scores": evaluate_cases(code, DEV, seed_dir, graphs, run, compiler, cpu)}
    if any(row["status"] != "valid" for row in seed["scores"].values()):
        raise RuntimeError("seed failed graph execution; inspect seed logs before using model budget")
    write_json(seed_dir / "scores.json", seed)
    jobs = [(arm, repeat) for repeat in range(1, args.repeats + 1) for arm in ARMS]
    with ThreadPoolExecutor(max_workers=min(3, len(jobs))) as pool:
        futures = [pool.submit(trajectory, arm, repeat, run, graphs, baseline, seed, compiler, cpu,
                               args.model, args.effort) for arm, repeat in jobs]
        trials = [future.result() for future in futures]
    selected = []
    for trial in trials:
        winner = next(a for a in trial["archive"] if a["index"] == trial["selected"])
        selected.append({k: winner[k] for k in ("policy", "sha256", "index")})
    write_json(run / "selected.json", selected)
    for trial, winner in zip(trials, selected):
        policy = run / winner["policy"]
        if digest(policy) != winner["sha256"]:
            raise RuntimeError("selected policy changed before holdout replay")
        directory = run / f"{trial['arm']}_{trial['repeat']:02d}" / "holdout"
        trial["holdout"] = evaluate_cases(policy, HOLDOUT, directory, graphs, run, compiler, cpu)
    write_json(run / "results.json", {"baseline": baseline, "trials": trials})
    print(f"Completed {len(trials)} trajectories in {run}", flush=True)


if __name__ == "__main__":
    main()
