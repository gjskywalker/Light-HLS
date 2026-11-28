"""Keep the manual first probe; freeze the deployment winner using only development programs."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path

from flow_pilot import COMPILER, model_call, read_json, write_json
from paper_order_flow import BOARD_PATH, hashfile
from paper_profile_flow import FEATURES, measure, rank, summary

SELECTION = (
    "Keep the incumbent unless all valid development cases pass and the product of "
    "candidate cycles is strictly lower than the product of manual cycles on exactly those cases."
)


def allocation(loops, weights):
    first = rank(loops, None)[:1]
    # Find the two highest-ranked remaining components without suppressing a source-order tie.
    others = {name: data for name, data in loops.items() if name not in first}
    return first + rank(others, weights)[:2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--base", type=Path, required=True)
    p.add_argument("--first", type=Path, required=True)
    p.add_argument("--run", type=Path, required=True)
    p.add_argument("--compiler", type=Path, default=COMPILER)
    p.add_argument("--library", type=Path, default=BOARD_PATH)
    p.add_argument("--jobs", type=int, default=6)
    p.add_argument("--stage", choices=["learn", "apply"], required=True)
    p.add_argument("--summary", type=Path)
    a = p.parse_args()
    base, first, run, compiler, lib = [
        x.resolve() for x in (a.base, a.first, a.run, a.compiler, a.library)
    ]
    run.mkdir(parents=True, exist_ok=True)
    protocol = read_json(base / "protocol.json")
    dev, hold = protocol["dev"], protocol["holdout"]
    if hashfile(compiler) != protocol["compiler_sha256"] or any(
        hashfile(lib / f) != v for f, v in protocol["board_sha256"].items()
    ):
        raise ValueError("compiler or library differs")
    learn = a.stage == "learn"
    names = dev if learn else dev + hold
    profiles = {n: read_json(first / "profiles" / n / "profile.json") for n in names}
    manuals = read_json(first / ("human_learn.json" if learn else "manual_apply.json"))
    previous = read_json(first / "frozen.json")
    if learn:
        if (run / "frozen.json").exists():
            raise ValueError("already frozen")
        spec = {
            "dev": dev,
            "features": FEATURES,
            "compiler": protocol["compiler_sha256"],
            "selection": SELECTION,
            "budget": 3,
            "first_case_freeze": hashfile(first / "frozen.json"),
        }
        write_json(run / "protocol.json", spec)
        first_arm = read_json(first / "agent_apply.json")
        feedback = {}
        for n in dev:
            if profiles[n]["status"] != "valid":
                feedback[n] = {"status": "native_unavailable"}
                continue
            loops = profiles[n]["loops"]
            feedback[n] = {
                "native_cycles": profiles[n]["native_cycles"],
                "features": {k: v["features"] for k, v in loops.items()},
                "protected": rank(loops, None)[0],
                "manual": manuals[n],
                "v1": first_arm[n],
            }
        msg = (
            "Design one residual loop-priority rule for compiled full-program HLS. One live proposal. You will return a signed "
            "11-integer weight list in [-64,64] as a JSON string, plus a brief hypothesis. Do not use labels or case-specific overrides. "
            "The manually ranked highest-dynamic-slack loop is always probe #1. Rank remaining loops by dot product, take top 2, "
            "ties by source order. Fixed budget three, each single loop target II=1; final choice is the greatest strictly-positive "
            "legal single-loop estimate times its native invocation count, else native. Only a strictly lower valid dev geomean "
            "than manual promotes the learned rule, else manual remains incumbent. All legal probes have measured ABI results and "
            "native HLS feedback for the eight development programs provided. Feature order: "
            + json.dumps(FEATURES)
            + ". Three log values log2(1+x); slack=cycles minus visits. Multiplication, load and store are scheduled header-block counts. "
            "Consider solutions in the remaining slots, including those that earlier rankers missed; avoid spending both on redundant "
            "zeros, and leave the protected loop untouched. Previous unconstrained weights: "
            + json.dumps(previous["weights"])
            + ". Development feedback (a failed native workload has no RTL numbers): "
            + json.dumps(feedback)
        )
        response, receipt = model_call(
            msg, ("weights", "hypothesis"), run / "model", "gpt-6.1-sol", "medium"
        )
        if not response:
            raise RuntimeError(f"model retrieval failed: {receipt}")
        weights = json.loads(response["weights"])
        if (
            not isinstance(weights, list)
            or len(weights) != len(FEATURES)
            or any(type(x) != int or abs(x) > 64 for x in weights)
        ):
            raise ValueError("invalid model weights")
        write_json(
            run / "proposal.json",
            {
                "weights": weights,
                "response": response,
                "receipt": receipt,
                "prompt_sha256": hashfile(run / "model/prompt.txt"),
            },
        )
    else:
        frozen = read_json(run / "frozen.json")
        spec = frozen["protocol"]
        if (
            spec["dev"] != dev
            or spec["compiler"] != protocol["compiler_sha256"]
            or hashfile(first / "frozen.json") != spec["first_case_freeze"]
        ):
            raise ValueError("protocol changed")
        weights = frozen["proposal"]["weights"]

    def assess(n):
        labels = allocation(profiles[n].get("loops", {}), weights)
        return measure(
            n, base, run, compiler, lib, profiles, None, ordered_labels=labels, previous_run=first
        )

    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        raw = dict(zip(names, pool.map(assess, names), strict=True))
    if learn:
        from math import prod

        eligible = [n for n in dev if profiles[n]["status"] == "valid"]
        promote = (
            bool(eligible)
            and all(raw[n]["status"] == manuals[n]["status"] == "valid" for n in eligible)
            and (
                prod(raw[n]["cycles"] for n in eligible) < prod(manuals[n]["cycles"] for n in eligible)
            )
        )
        write_json(
            run / "frozen.json",
            {
                "protocol": spec,
                "proposal": read_json(run / "proposal.json"),
                "deploy": "model_residual" if promote else "manual_incumbent",
                "dev_score": summary(dev, profiles, manuals, raw),
                "dev_cases": raw,
            },
        )
        return
    deployed = raw if frozen["deploy"] == "model_residual" else manuals
    out = {"posthoc": True, "frozen": frozen, "splits": {}, "cases": {}}
    for split, grp in [("dev", dev), ("holdout", hold), ("all", names)]:
        out["splits"][split] = {
            "raw": summary(grp, profiles, manuals, raw),
            "deployed": summary(grp, profiles, manuals, deployed),
        }
    for n in names:
        out["cases"][n] = {
            "split": "dev" if n in dev else "holdout",
            "native": profiles[n].get("native_cycles"),
            "manual": manuals[n],
            "raw": raw[n],
            "deployed_cycles": deployed[n].get("cycles"),
        }
    write_json(a.summary or run / "results.json", out)


if __name__ == "__main__":
    main()
