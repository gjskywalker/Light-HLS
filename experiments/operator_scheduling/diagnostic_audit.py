"""Audit a diagnostic experiment and export the evidence needed to review each candidate."""

import argparse
import ctypes
from pathlib import Path
import random
import statistics
from collections import Counter

from diagnostic_flow import (ARMS, DEV, HOLDOUT, ROUNDS, COMPILER, FIXTURE, diagnostic, digest,
                             ranking, read_json, replay, scalar, write_json)
from flow_schedule import check


def verify_schedules(run, frozen, audit_dir, compiler):
    """Recompile each distinct proposed holdout schedule with a fresh set of test vectors."""
    audit_dir.mkdir(parents=True, exist_ok=False)
    rng = random.Random(20261125)
    inputs = [([i * 0x11111111 for i in range(8)], 7),
              ([1 << (i * 4) for i in range(8)], 0x80000001)]
    inputs.extend(([rng.getrandbits(32) for _ in range(8)], rng.getrandbits(32)) for _ in range(30))
    lib = ctypes.CDLL(str(run / "vectors/oracle.so"))
    checks = {}
    for name in HOLDOUT:
        function = getattr(lib, name)
        function.argtypes = (ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32)
        function.restype = ctypes.c_uint32
        vectors = []
        for memory, key in inputs:
            expected = function((ctypes.c_uint32 * 8)(*memory), key)
            vectors.append("".join(f"{word:08x}" for word in reversed(memory + [key, expected])))
        path = audit_dir / f"{name}.hex"
        path.write_text("\n".join(vectors) + "\n")
        graph = read_json(run / "native" / name / "graph.json")
        for record in frozen:
            row = record["holdout"][name]
            if "starts" not in row:
                continue
            assert check(graph, row["starts"]) == row["makespan"]
            key = (name, tuple(row["starts"]))
            if key not in checks:
                folder = audit_dir / name / str(len(checks))
                checks[key] = replay(compiler, name, folder, path, row["starts"], source=FIXTURE)
            expected, actual = row["rtl"], checks[key]
            if (expected["status"], expected.get("rtl_cycles"), expected.get("diagnostic")) != (
                    actual["status"], actual.get("rtl_cycles"), actual.get("diagnostic")):
                raise ValueError(f"independent replay mismatch at {name}")
    return len(checks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-dir", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, default=COMPILER)
    args = parser.parse_args()
    run = args.run.resolve()
    result, protocol = read_json(run / "results.json"), read_json(run / "protocol.json")
    baseline, trials = result["baseline"], result["trials"]
    assert digest(args.compiler) == protocol["compiler_sha256"]
    graphs = {n: read_json(run / "native" / n / "graph.json") for n in DEV}
    selected = read_json(run / "selected.json")
    assert len(trials) == len(selected) == len(ARMS) * protocol["repeats"]
    policies, records = {}, []
    for trial, choice in zip(trials, selected):
        arm, rep = trial["arm"], trial["repeat"]
        directory = run / f"{arm}_{rep:02d}"
        archive = {a["index"]: a for a in trial["archive"]}
        index = min(archive, key=lambda i: ranking(archive[i]["scores"], baseline, i))
        assert trial["selected"] == index == choice["index"]
        assert choice["sha256"] == archive[index]["sha256"]
        calls = trial["calls"]
        assert len(calls) == ROUNDS * 2
        for file in directory.glob("round_*/*/prompt.txt"):
            prompt = file.read_text()
            assert all(n not in prompt for n in HOLDOUT)
        candidates = []
        for i, item in archive.items():
            sha = item["sha256"]
            assert digest(run / item["policy"]) == sha
            policies[sha] = (run / item["policy"]).read_text()
            candidates.append({"index": i, "parent": item["parent"], "sha256": sha,
                               "dev": {n: scalar(s) for n, s in item["scores"].items()}})
        for step in trial["steps"]:
            base = archive[step["parent"]]
            queries = [r["query"] for r in step["reports"]]
            if arm == "A":
                assert not queries
            if arm == "B":
                assert queries == [f"{n}/overview" for n in DEV]
            assert len(queries) <= len(DEV)
            for report in step["reports"]:
                assert report["response"] == diagnostic(report["query"], base, archive.get(base["parent"]),
                                                        graphs, run)
        counts = {"development_replays": sum("rtl" in row for item in trial["archive"] if item["index"]
                                              for row in item["scores"].values()),
                  "holdout_replays": sum("rtl" in row for row in trial["holdout"].values()),
                  "development_compiles": len(list(directory.glob("round_*/*/replay/compile.log"))),
                  "holdout_compiles": len(list(directory.glob("holdout/*/replay/compile.log"))),
                  "tokens": dict(sum((Counter(c["usage"]) for c in calls), Counter()))}
        records.append({"arm": arm, "repeat": rep, "selected": index, "candidates": candidates,
                        "queries": [[r["query"] for r in s["reports"]] for s in trial["steps"]],
                        "hypotheses": [s["hypothesis"] for s in trial["steps"]],
                        "calls": [c["status"] for c in calls], **counts,
                        "holdout": {n: scalar(s) for n, s in trial["holdout"].items()}})
    n_checks = verify_schedules(run, trials, args.verify_dir.resolve(), args.compiler.resolve())
    compact = {"protocol": protocol, "graph_hashes": read_json(run / "graph_hashes.json"),
               "baseline": {n: {"native": b["rtl_cycles"], "graph_opt": b["exact_graph_cycles"],
                                "exact_replay": b["exact_replay"].get("rtl_cycles"),
                                "critical_path_replay": b["tail_replay"].get("rtl_cycles")}
                            for n, b in baseline.items()},
               "extra_vector_seed": 20261125, "extra_vectors": 32, "distinct_schedules_replayed": n_checks,
               "policies": policies, "trials": records}
    write_json(args.output, compact)
    print("verified distinct holdout schedules", n_checks)
    for arm in ARMS:
        group = [r for r in records if r["arm"] == arm]
        ratios = [s["cycles"] / baseline[n]["rtl_cycles"] for r in group for n, s in r["holdout"].items()
                  if s["status"] == "valid"]
        print(arm, [r["selected"] for r in group], "valid", len(ratios),
              "win/tie/loss", (sum(x < 1 for x in ratios), ratios.count(1), sum(x > 1 for x in ratios)),
              "valid geomean ratio", statistics.geometric_mean(ratios) if ratios else None)


if __name__ == "__main__":
    main()
