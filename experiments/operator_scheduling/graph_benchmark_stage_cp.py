"""A compact CP-SAT comparison for the DiffSched discrete stage objective."""

import argparse
from concurrent.futures import ProcessPoolExecutor
import json
from pathlib import Path
import time

import ortools
from ortools.sat.python import cp_model

from graph_benchmark_diffsched import SOURCE, cost, official_check
from run_round import write_json


def solve_one(job):
    path, target, seconds = job
    weights, edges, seed, depth, ratio = json.loads(path.read_text())
    begin = time.perf_counter()
    model = cp_model.CpModel()
    stages = [model.new_int_var(0, depth-1, f"s{i}") for i in range(len(weights))]
    stage_loads = [[] for _ in range(depth)]
    for i, weight in enumerate(weights):
        model.add_hint(stages[i], seed[i])
        if weight == 0:
            continue
        flags = [model.new_bool_var(f"p{i}_{k}") for k in range(depth)]
        model.add_exactly_one(flags)
        model.add(stages[i] == sum(k * flags[k] for k in range(1, depth)))
        for k, flag in enumerate(flags):
            stage_loads[k].append(weight * flag)
            model.add_hint(flag, int(seed[i] == k))
    start = cost(weights, edges, seed)
    peak = model.new_int_var(max(max(weights), (sum(weights)+depth-1)//depth),
                             start["objective"]//ratio, "peak")
    model.add_hint(peak, start["peak"])
    for load in stage_loads:
        model.add(peak >= sum(load))
    coefficient = [0] * len(weights)
    for i, j, weight in edges:
        model.add(stages[i] <= stages[j])
        coefficient[i] -= weight
        coefficient[j] += weight
    objective = ratio * peak + sum(a*v for a, v in zip(coefficient, stages) if a)
    model.add(objective <= start["objective"])
    model.minimize(objective)
    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = seconds
    solver.parameters.num_search_workers = 1
    solver.parameters.random_seed = 0
    status = solver.solve(model)
    elapsed = time.perf_counter() - begin
    has_incumbent = status in (cp_model.OPTIMAL, cp_model.FEASIBLE)
    if status == cp_model.MODEL_INVALID or status == cp_model.INFEASIBLE:
        raise ValueError(f"{path}: {solver.status_name(status)}, seed is known feasible")
    result = [solver.value(v) for v in stages] if has_incumbent else seed
    checked = cost(weights, edges, result)
    if has_incumbent and round(solver.objective_value) != checked["objective"]:
        raise ValueError("CP-SAT objective mismatch")
    checker, _ = official_check()
    if checker(result, str(SOURCE / "graphgml" / path.with_suffix(".gml").name)) != checked["communication"]:
        raise ValueError("upstream checker disagreement")
    write_json(target / (path.stem + ".json"), result)
    return path.stem, {**checked, "solver_status": solver.status_name(status), "returned_seed_on_unknown": not has_incumbent,
                       "bound": solver.best_objective_bound, "solver_seconds": solver.wall_time,
                       "model_and_solver_seconds": elapsed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sweep", type=Path)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--parallel", type=int, default=3)
    args = parser.parse_args()
    directory = args.sweep / "stage_cp"
    directory.mkdir()
    rows = {}
    protocol = {"ortools": ortools.__version__, "seconds": args.seconds, "workers_per_graph": 1,
                "parallel_graphs": args.parallel, "seed": 0,
                "solver": "compact exact discrete model, balanced topological seed as a hint and incumbent"}
    jobs = [(path, directory, args.seconds) for path in sorted(args.sweep.glob("rand_graph*.json"))]
    with ProcessPoolExecutor(max_workers=args.parallel) as pool:
        for name, row in pool.map(solve_one, jobs):
            rows[name] = row
            write_json(args.sweep / "stage_cp.json", {"protocol": protocol, "graphs": rows})
            print(name, row["solver_status"], row["objective"], flush=True)


if __name__ == "__main__":
    main()
