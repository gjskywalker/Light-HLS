"""Post-hoc policy search using state coverage from one native full-program execution."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from collections import Counter
import json
import math
from pathlib import Path
import re
import statistics

from flow_pilot import COMPILER, model_call, read_json, write_json
from paper_loop_flow import find_loops, probe
from paper_order_flow import BOARD_PATH, hashfile, launch, simulate

FEATURES = [
    "log2_header_cycles",
    "log2_slack_cycles",
    "log2_visits",
    "state_count",
    "leaf",
    "depth",
    "loads",
    "stores",
    "multiplies",
    "body_conditions",
    "mapped",
]
MAP = re.compile(r"Done evaluation Loop Latency for Loop ([^ (]+)\((Loop_kernel_[^)]+)\)")
PROFILE_FORMAT = 1


def profile(n, base, run):
    dest = run / "profiles" / n
    out = dest / "profile.json"
    if out.is_file():
        return read_json(out)
    dest.mkdir(parents=True, exist_ok=True)
    folder = base / "native" / n
    reference = read_json(folder / "result.json")
    if reference["status"] != "valid":
        result = {"status": "native_unavailable", "diagnostic": reference["status"]}
        write_json(out, result)
        return result
    rep = folder / "hls/reports"
    blocks = read_json(rep / "scheduling/schedule.json")["blocks"]
    count = max(b["first_state"] + b["state_count"] for b in blocks) + 1
    states = range(count)
    tb = (base / "input" / n / "tb.v").read_text()
    hook = f"""integer i;
integer hits[0:{count-1}];
initial for (i=0;i<{count};i=i+1) hits[i]=0;
always @(posedge ap_clk) if (!ap_rst && dut.state < {count}) hits[dut.state] = hits[dut.state]+1;
final for (i=0;i<{count};i=i+1) $display("STATE %0d %0d",i,hits[i]);
"""
    if tb.count("endmodule") != 1:
        raise ValueError(f"unexpected testbench structure for {n}")
    (dest / "profile_tb.v").write_text(tb.replace("endmodule", hook + "endmodule"))
    rtl = rep / "rtl/top.v"
    if launch(
        ["iverilog", "-g2012", "-s", "tb", "-o", "sim", str(rtl), "profile_tb.v"], dest, "elab.log"
    ):
        raise ValueError(f"profile elaboration failed for {n}")
    rc = launch(["vvp", "sim"], dest, "trace.log", timeout=360)
    log = (dest / "trace.log").read_text()
    passline = f'PASS cycles={reference["cycles"]} checksum={reference["checksum"]}'
    if rc or passline not in log:
        raise ValueError(f"profile failed to match native cycles and checksum: {n}")
    hits = {int(a): int(b) for a, b in re.findall(r"^STATE (\d+) (\d+)$", log, re.M)}
    if hits.keys() != set(states):
        raise ValueError("incomplete FSM counts")
    block_by = {b["name"]: b for b in blocks}
    scratch = rep / ".lighthls"
    mapping = {
        label: re.sub(r"[^a-zA-Z0-9_]", "_", ir)
        for ir, label in MAP.findall(
            (scratch / "HI_WithDirectiveTimingResourceEvaluation__forCheck_0").read_text()
        )
    }
    source_loops = find_loops(scratch / "tmp_loopLabeled.cc")
    ordered = sorted(source_loops, key=lambda l: source_loops[l]["offset"])
    loops = {}
    for i, label in enumerate(ordered):
        static = source_loops[label]["features"]
        block = block_by.get(mapping.get(label))
        size = block["state_count"] if block else 0
        visits = hits[block["first_state"]] if size else 0
        cycles = (
            sum(hits[k] for k in range(block["first_state"], block["first_state"] + size))
            if size
            else 0
        )
        slack = max(0, cycles - visits)
        ops = Counter(inst["opcode"] for inst in block["instructions"]) if block else {}
        leaf = int(i + 1 == len(ordered) or source_loops[ordered[i + 1]]["features"][0] <= static[0])
        features = [round(math.log2(1 + x), 4) for x in (cycles, slack, visits)] + [
            size,
            leaf,
            static[0],
            ops.get("load", 0),
            ops.get("store", 0),
            ops.get("mul", 0),
            static[3],
            int(bool(size)),
        ]
        loops[label] = {
            "features": features,
            "block": mapping.get(label),
            "visits": visits,
            "cycles": cycles,
            "removable_upper_bound": slack,
            "offset": source_loops[label]["offset"],
        }
    result = {
        "status": "valid",
        "native_cycles": reference["cycles"],
        "checksum": reference["checksum"],
        "rtl_sha256": hashfile(rtl),
        "loops": loops,
    }
    write_json(out, result)
    return result


def rank(loops, weights):
    def key(label):
        row = loops[label]
        if weights is None:
            # A pragmatic II=1 upper bound, with preference for inner loops in equal-cost cases.
            return (
                -row["removable_upper_bound"],
                -row["features"][4],
                -row["features"][5],
                row["offset"],
            )
        return (-sum(a * b for a, b in zip(weights, row["features"], strict=True)), row["offset"])

    return sorted(loops, key=key)[:3]


def measure(
    n, base, run, compiler, library, profiles, weights, *, ordered_labels=None, previous_run=None, remeasure=False
):
    stats = profiles[n]
    if stats["status"] != "valid":
        return {"status": "native_unavailable", "reason": stats["diagnostic"]}
    native = read_json(base / "native" / n / "result.json")
    labels = rank(stats["loops"], weights) if ordered_labels is None else ordered_labels
    if len(labels) != len(set(labels)) or len(labels) > 3 or not set(labels) <= stats["loops"].keys():
        raise ValueError("invalid probe allocation")
    folders = {}
    attempts = {}
    for label in labels:
        cached = base / "probes" / n / label
        if (cached / "compile.json").is_file():
            cp = read_json(cached / "compile.json")
            folder, origin = cached, "original"
        elif previous_run and (previous_run / "probes" / n / label / "compile.json").is_file():
            folder, origin = previous_run / "probes" / n / label, "previous_profile"
            cp = read_json(folder / "compile.json")
        else:
            cp = probe(run, base, n, label, native, library, compiler)
            folder, origin = run / "probes" / n / label, "profile_run"
        folders[label] = folder
        gain = 0.0
        if cp["status"] == "compiled" and cp["gain_estimate"] > 0:
            loop = stats["loops"][label]
            if loop["block"] == cp["block"] and loop["visits"] > 0:
                gain = cp["gain_estimate"] * (loop["visits"] / cp["trip"])
        attempts[label] = {
            "source": origin,
            "probe": cp,
            "predicted_saving": gain,
        }
    choice = max(labels, key=lambda l: attempts[l]["predicted_saving"]) if labels else None
    if choice is None or attempts[choice]["predicted_saving"] <= 0:
        return {
            "status": "valid",
            "choice": "native",
            "cycles": native["cycles"],
            "attempts": attempts,
            "checksum": native["checksum"],
        }
    folder = folders[choice]
    sha = hashfile(folder / "hls/reports/rtl/top.v")
    # Cross-policy reuse is keyed by both the original expected-output testbench and every byte of RTL.
    tb = base / "input" / n / "tb.v"
    result_folder = run / "validated" / n / (hashfile(tb)[:12] + "-" + sha)
    result_folder.mkdir(parents=True, exist_ok=True)
    cache = result_folder / "simulation.json"
    if not cache.exists():
        prior_sim = previous_run / cache.relative_to(run) if previous_run else None
        known = prior_sim if prior_sim and prior_sim.is_file() else folder / "rtl.json"
        if not remeasure and known.is_file() and read_json(known)["status"] == "valid":
            sim = read_json(known)
        else:
            hls = result_folder / "hls"
            if not hls.exists():
                hls.symlink_to(folder / "hls", target_is_directory=True)
            sim = simulate(base / "input" / n, result_folder, {"status": "compiled"})
        write_json(cache, sim)
    result = read_json(cache)
    return dict(result, choice=choice, attempts=attempts, rtl_sha256=sha)


def summary(names, profiles, manual, agent):
    rows = [n for n in names if manual[n]["status"] == agent[n]["status"] == "valid"]
    summ = {
        "scheduled": len(names),
        "native_valid": sum(profiles[n]["status"] == "valid" for n in names),
        "paired": len(rows),
    }
    for title, a, b in [
        ("manual/native", manual, None),
        ("agent/native", agent, None),
        ("agent/manual", agent, manual),
    ]:
        ratios = [
            a[n]["cycles"] / (b[n]["cycles"] if b else profiles[n]["native_cycles"]) for n in rows
        ]
        summ[title] = {
            "wins": sum(x < 1 for x in ratios),
            "ties": sum(x == 1 for x in ratios),
            "losses": sum(x > 1 for x in ratios),
            "geomean": statistics.geometric_mean(ratios) if ratios else None,
        }
    return summ


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base", type=Path, required=True)
    ap.add_argument("--run", type=Path, required=True)
    ap.add_argument("--compiler", type=Path, default=COMPILER)
    ap.add_argument("--library", type=Path, default=BOARD_PATH)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--stage", choices=["prepare", "learn", "apply"], required=True)
    ap.add_argument("--summary", type=Path)
    a = ap.parse_args()
    base, run, compiler, lib = [x.resolve() for x in (a.base, a.run, a.compiler, a.library)]
    run.mkdir(parents=True, exist_ok=True)
    cfg = read_json(base / "protocol.json")
    dev, hold = cfg["dev"], cfg["holdout"]
    if hashfile(compiler) != cfg["compiler_sha256"] or any(
        hashfile(lib / relative) != digest for relative, digest in cfg["board_sha256"].items()
    ):
        raise ValueError("replay requires reference compiler and board")
    names = dev if a.stage in ("prepare", "learn") else dev + hold
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        profiles = dict(zip(names, pool.map(lambda n: profile(n, base, run), names), strict=True))
    if a.stage == "prepare":
        return

    def batch(label, weights):
        with ThreadPoolExecutor(max_workers=a.jobs) as pool:
            result = dict(
                zip(
                    names,
                    pool.map(lambda n: measure(n, base, run, compiler, lib, profiles, weights), names),
                    strict=True,
                )
            )
        write_json(run / (label + "_" + a.stage + ".json"), result)
        return result

    if a.stage == "learn":
        manual = batch("human", None)
        feedback = {}
        for n in names:
            row = profiles[n]
            if row["status"] != "valid":
                feedback[n] = {"status": row["status"]}
                continue
            previous = read_json(base / "native" / n / "experiment.json")
            feedback[n] = {
                "native": row["native_cycles"],
                "prior_scheduler_cycles": previous["agent"]["deployed"].get("cycles"),
                "manual_cycles": manual[n].get("cycles"),
                "manual_probes": manual[n]["attempts"],
                "preprobe_features": {l: r["features"] for l, r in row["loops"].items()},
            }
        prompt = (
            "You propose a reusable ranking for a budget of three single-loop II=1 compiler probes on each full C program. "
            "Use exactly 11 integer weights in [-64,64], in JSON list encoded as a string, with larger dot products first. "
            "Features in order: "
            + json.dumps(FEATURES)
            + ". Header cycles and visits are counted in validated unmodified native RTL. "
            "Slack = header cycles minus visits (optimistic saved cycles if II=1). First three coordinates log2(1+x). "
            "Loads/stores/multiplications are in native scheduled header block. Body conditions include nested AST. "
            "Unmapped headers use 0. The hand-designed comparator ranks the highest dynamic slack then innermost. "
            "After three compiler probes, both deploy the legal candidate with greatest positive local estimated saving times native "
            "header visits divided by exact trip count; otherwise native. Hypothesize lawful loop-friendly traffic, control and opportunity. "
            "Do not encode program names, labels or previous candidate IDs. Output weights and explanation. "
            "The following covers the complete 8 development programs; these summaries are all the evidence available.\n"
            + json.dumps(feedback)
        )
        if (run / "frozen.json").exists():
            raise ValueError("policy already frozen")
        model_dir = run / "model"
        if model_dir.exists():
            prior = read_json(model_dir / "call.json")
            if prior["status"] == "generated":
                raise ValueError("a prior model proposal already exists")
            model_dir = run / "model_retry"
        response, receipt = model_call(
            prompt, ("weights", "hypothesis"), model_dir, "gpt-6.1-sol", "medium"
        )
        if not response:
            raise RuntimeError(f"live model failed: {receipt}")
        weights = json.loads(response["weights"])
        if (
            not isinstance(weights, list)
            or len(weights) != len(FEATURES)
            or any(type(v) != int or abs(v) > 64 for v in weights)
        ):
            raise ValueError("invalid weights")
        write_json(
            run / "frozen.json",
            {
                "weights": weights,
                "response": response,
                "receipt": receipt,
                "features": FEATURES,
                "dev": dev,
                "profile_version": PROFILE_FORMAT,
                "compiler": cfg["compiler_sha256"],
                "prompt_sha256": hashfile(model_dir / "prompt.txt"),
            },
        )
        return
    frozen = read_json(run / "frozen.json")
    if frozen["dev"] != dev or frozen["compiler"] != cfg["compiler_sha256"]:
        raise ValueError("freeze and reference differ")
    manual = batch("manual", None)
    agent = batch("agent", frozen["weights"])
    if a.summary:
        write_json(
            a.summary,
            {
                "exploratory_posthoc": True,
                "frozen": frozen,
                "part": cfg["part"],
                "period_ns": cfg["period_ns"],
                "rule": "same budget of three single-loop II=1 compiler probes per arm, plus one native profiling simulation",
                "summaries": {
                    s: summary(ns, profiles, manual, agent)
                    for s, ns in [("dev", dev), ("holdout", hold), ("all", names)]
                },
                "cases": {
                    n: {
                        "split": "dev" if n in dev else "holdout",
                        "native": profiles[n].get("native_cycles"),
                        "profile": profiles[n],
                        "manual": manual[n],
                        "agent": agent[n],
                    }
                    for n in names
                },
            },
        )


if __name__ == "__main__":
    main()
