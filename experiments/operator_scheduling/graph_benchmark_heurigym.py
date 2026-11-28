"""Run frozen HeuriGym solvers, the official checker, and an independent optimality check."""

import argparse
from collections import Counter
from graphlib import TopologicalSorter
import json
import os
from pathlib import Path
import subprocess
import sys

from run_round import DATA, EXPERT, HERE, SOURCES, RUN_TIMEOUT, digest, run_solver, sandbox, write_json

PROGRAM = SOURCES / "heurigym_tools/operator_scheduling/program"
sys.path.insert(0, str(PROGRAM))
from evaluator import evaluate
from verifier import verify


def certify(graph, target, folder):
    """Prove no makespan below target by bounds, or exact finite-horizon SAT."""
    kinds = [kind for _, kind in graph["nodes"]]
    index = {name: i for i, (name, _) in enumerate(graph["nodes"])}
    durations = [graph["delay"][kind] for kind in kinds]
    parents = {i: set() for i in range(len(kinds))}
    children = {i: set() for i in parents}
    for u, v, _ in graph["edges"]:
        i, j = index[u], index[v]
        parents[j].add(i)
        children[i].add(j)
    order = list(TopologicalSorter(parents).static_order())
    earliest, tail = {}, {}
    for i in order:
        earliest[i] = max((earliest[j] + durations[j] for j in parents[i]), default=0)
    for i in reversed(order):
        tail[i] = durations[i] + max((tail[j] for j in children[i]), default=0)
    capacity = graph["resource"]
    counts = Counter(kinds)
    lower = max(max(tail.values()), max(graph["delay"][k] * ((n + capacity[k] - 1) // capacity[k])
                                         for k, n in counts.items()))
    horizon = target - 1
    latest = {i: horizon - tail[i] for i in order}
    if lower > horizon:
        return {"method": "critical_path_or_resource", "lower_bound": lower, "verified": True}
    groups = {k: [i for i in order if kinds[i] == k] for k in counts}
    for a in range(horizon):
        for b in range(a + 1, horizon + 1):
            for k, group in groups.items():
                demand = sum(max(0, min(durations[i], b-a, earliest[i]+durations[i]-a, b-latest[i]))
                             for i in group)
                if demand > capacity[k] * (b-a):
                    return {"method": "interval_resource", "resource": k, "interval": [a, b],
                            "required": demand, "available": capacity[k] * (b-a),
                            "tested_horizon": horizon, "verified": True}

    lines = ["(set-logic QF_FD)"]
    # One Boolean per feasible start time. Cover constraints are checked on every integer cycle.
    for i in order:
        variables = [f"x_{i}_{s}" for s in range(earliest[i], latest[i]+1)]
        lines.extend(f"(declare-const {var} Bool)" for var in variables)
        names = " ".join(variables)
        lines.append(f"(assert (or {names}))")
        if len(variables) > 1:
            lines.append(f"(assert ((_ at-most 1) {names}))")
    for i in order:
        for j in children[i]:
            for s in range(earliest[i], latest[i]+1):
                forbidden = [f"x_{j}_{t}" for t in range(earliest[j], min(latest[j]+1, s+durations[i]))]
                if forbidden:
                    lines.append(f"(assert (=> x_{i}_{s} (not (or {' '.join(forbidden)}))))")
    for t in range(horizon):
        for k, group in groups.items():
            active = [f"x_{i}_{s}" for i in group for s in
                      range(max(earliest[i], t-durations[i]+1), min(latest[i], t)+1)]
            if len(active) > capacity[k]:
                lines.append(f"(assert ((_ at-most {capacity[k]}) {' '.join(active)}))")
    path = folder / "better.smt2"
    path.write_text("\n".join(lines + ["(check-sat)"]) + "\n")
    result = subprocess.run(["z3", "-T:45", str(path)], capture_output=True, text=True, timeout=50, check=True)
    verdict = result.stdout.strip()
    if verdict not in ("unsat", "sat", "timeout", "unknown"):
        raise ValueError(verdict[:500])
    return {"method": "z3_finite_horizon", "tested_horizon": horizon, "result": verdict,
            "input_sha256": digest(path), "verified": verdict == "unsat"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True)
    subprocess.run(sandbox() + ["/usr/bin/true"], check=True)
    cpu = min(os.sched_getaffinity(0))
    codes = {"critical_path": HERE / "baseline.py",
             "independent": HERE / "round1_20261120/independent_02.py",
             "feedback": HERE / "round1_20261120/feedback_01.py"}
    refs = json.loads(EXPERT.read_text())
    data = {"cpu": cpu, "z3": subprocess.check_output(["z3", "-version"], text=True).strip(),
            "wall_seconds_per_candidate": RUN_TIMEOUT, "sha256": {k: digest(p) for k, p in codes.items()},
            "official_reference_sha256": digest(EXPERT), "graphs": {}}
    for path in sorted(DATA.glob("*/*.json")):
        name = path.stem
        folder = out / name
        folder.mkdir()
        graph = json.loads(path.read_text())
        row = {"input_sha256": digest(path), "subset": path.parent.name, "expert_cycles": refs[name]}
        for k, code in codes.items():
            row[k] = run_solver(code, path, folder / k, cpu)
            if row[k]["status"] == "valid":
                sched = folder / k / "candidate/schedule.txt"
                ok, error = verify(str(path), str(sched))
                if not ok or evaluate(str(path), str(sched)) != row[k]["makespan"]:
                    raise ValueError(f"{name}: official verification failed: {error}")
                row[k]["schedule_sha256"] = digest(sched)
        row["optimality"] = certify(graph, refs[name], folder)
        data["graphs"][name] = row
        write_json(out / "results.json", data)
        print(name, row["optimality"], flush=True)


if __name__ == "__main__":
    main()
