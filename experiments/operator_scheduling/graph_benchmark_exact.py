"""Check unresolved HeuriGym horizons with an interval CP model and official feasibility tests."""

import argparse
import json
from pathlib import Path
import sys

from ortools import __version__
from ortools.sat.python import cp_model

from run_round import DATA, SOURCES, write_json
sys.path.insert(0, str(SOURCES / "heurigym_tools/operator_scheduling/program"))
from evaluator import evaluate
from verifier import verify


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sweep", type=Path)
    parser.add_argument("--seconds", type=float, default=90)
    args = parser.parse_args()
    data = json.loads((args.sweep / "results.json").read_text())
    results = {}
    for name, row in data["graphs"].items():
        if row["optimality"]["verified"]:
            continue
        path = DATA / row["subset"] / (name + ".json")
        graph = json.loads(path.read_text())
        horizon = row["expert_cycles"] - 1
        model = cp_model.CpModel()
        starts = {}
        intervals = {k: [] for k in graph["resource"]}
        for node, kind in graph["nodes"]:
            d = graph["delay"][kind]
            starts[node] = model.new_int_var(0, horizon - d, node)
            intervals[kind].append(model.new_fixed_size_interval_var(starts[node], d, "i" + node))
        kinds = dict(graph["nodes"])
        for u, v, _ in graph["edges"]:
            model.add(starts[u] + graph["delay"][kinds[u]] <= starts[v])
        for kind, group in intervals.items():
            model.add_cumulative(group, [1] * len(group), graph["resource"][kind])
        solver = cp_model.CpSolver()
        solver.parameters.max_time_in_seconds = args.seconds
        solver.parameters.num_search_workers = 4
        solver.parameters.random_seed = 0
        status = solver.solve(model)
        result = {"status": solver.status_name(status), "tested_horizon": horizon, "seconds": solver.wall_time}
        if status in (cp_model.OPTIMAL, cp_model.FEASIBLE):
            schedule = args.sweep / name / "cp_schedule.txt"
            schedule.write_text("".join(f"{k}:{solver.value(v)}\n" for k,v in starts.items()))
            ok, error = verify(str(path), str(schedule))
            if not ok:
                raise ValueError(error)
            result["cycles"] = evaluate(str(path), str(schedule))
        results[name] = result
    write_json(args.sweep / "interval_cp.json",
               {"ortools": __version__, "threads": 4, "seconds_limit": args.seconds, "results": results})
    print(results)


if __name__ == "__main__":
    main()
