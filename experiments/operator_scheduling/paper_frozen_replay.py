"""Apply previously frozen policies after correctness repairs, with no new model requests."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import csv
import os
from pathlib import Path
import tempfile

from flow_pilot import COMPILER, HERE, read_json, write_json
from paper_loop_flow import MANUAL, find_loops, policy
from paper_order_flow import BOARD, BOARD_PATH, COLLECTION, TAIL, hashfile, variant


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--input", type=Path, required=True)
    p.add_argument("--loops", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--compiler", type=Path, default=COMPILER)
    p.add_argument("--library", type=Path, default=BOARD_PATH)
    p.add_argument("--jobs", type=int, default=5)
    p.add_argument("--resume", action="store_true", help="reuse finished cases and retain interrupted attempts")
    args = p.parse_args()
    root, prior, discovery = args.output.resolve(), args.input.resolve(), args.loops.resolve()
    compiler, lib = args.compiler.resolve(), args.library.resolve()
    if args.resume:
        if (root/"input").resolve() != (prior/"input").resolve():
            raise ValueError("resume input differs")
    else:
        root.mkdir(parents=True, exist_ok=False)
        (root/"input").symlink_to(os.path.relpath(prior/"input",root),target_is_directory=True)
    old = read_json(prior/"protocol.json")
    metadata = f"boards/{BOARD}/board.json"
    board_hashes = {k:hashfile(lib/k) for k in old["board_sha256"]}
    board_info = read_json(lib/metadata)
    if (board_info["part"] != old["part"] or
            float(old["period_ns"]) not in map(float,board_info["clock_periods_ns"]) or
            any(v != old["board_sha256"][k] for k,v in board_hashes.items() if k != metadata)):
        raise ValueError("board target or characterized operator tables differ from frozen inputs")
    _, order = read_json(prior/"frozen.json")["agent"]
    _, loops = read_json(discovery/"frozen.json")["agent"]
    sources = {c["id"]:c for c in read_json(COLLECTION/"cases.json")}
    names = old["dev"]+old["holdout"]
    assert all(hashfile(root/"input"/n/"kernel.cpp")==old["inputs"][n]["adapter"] for n in names)
    protocol = {"compiler_sha256":hashfile(compiler),"board_sha256":board_hashes,"part":old["part"],
                "period_ns":old["period_ns"],"dev":old["dev"],"holdout":old["holdout"],
                "source_hashes":old["inputs"],"order":order,"loops":loops,"manual_order":TAIL,
                "manual_loops":MANUAL,"order_freeze":hashfile(prior/"frozen.json"),
                "loop_freeze":hashfile(discovery/"frozen.json"),
                "combined_rule":"use selected pipeline when compiler predicts positive savings and proves exact trip count; otherwise use gated ordinary-block policy",
                "source_files":{name:hashfile(HERE/name) for name in
                                ("paper_frozen_replay.py","paper_loop_flow.py","paper_order_flow.py")}}
    if args.resume:
        original = read_json(root/"protocol.json")
        # The orchestration script may gain recovery support. The compiler, policies,
        # feature extraction, candidate evaluation, inputs and split must match.
        if ({k:v for k,v in protocol.items() if k!="source_files"} !=
                {k:v for k,v in original.items() if k!="source_files"} or any(
                    original["source_files"][k] != protocol["source_files"][k]
                    for k in ("paper_order_flow.py","paper_loop_flow.py"))):
            raise ValueError("resume protocol differs")
        interrupted = Path(tempfile.mkdtemp(prefix="interrupted-", dir=root))
        write_json(interrupted/"resume.json", {"runner":protocol["source_files"]["paper_frozen_replay.py"]})
        protocol = original
    else:
        write_json(root/"protocol.json",protocol)
    with (HERE/"vericert_published_cycles_20261125.csv").open() as f:
        paper = {r["design"].replace("-","_"):r for r in csv.DictReader(f)}
    def measure(n):
        if args.resume:
            finished = root/"native"/n/"experiment.json"
            if finished.is_file():
                return n,read_json(finished)
            for stage in ("native","manual_order","agent_order","probes"):
                path = root/stage/n
                if path.is_dir():
                    target = interrupted/n/stage
                    target.parent.mkdir(parents=True,exist_ok=True)
                    path.rename(target)
        case = sources[n]
        base = variant(case,"native",None,root,compiler,lib)
        row={"split":"dev" if n in old["dev"] else "holdout","published":paper[n[11:]],"native":base}
        if base["status"]=="valid":
            source = root/"native"/n/"hls/reports/.lighthls/tmp_loopLabeled.cc"
            features={n:find_loops(source)}
            for label,op,pipe in [("manual",TAIL,MANUAL),("agent",order,loops)]:
                allocation=variant(case,label+"_order",op,root,compiler,lib)
                piped=policy(root,root,[n],{n:base},features,pipe,lib,compiler,1)[n]
                # This deployment choice only depends on the compiler's structural decision.
                # A subsequently failing RTL check is reported, not silently replaced.
                combined=allocation if piped.get("choice")=="native" else piped
                row[label]={"ordering":allocation,"pipeline":piped,"deployed":combined}
        write_json(root/"native"/n/"experiment.json",row)
        print(n,base["status"],base.get("cycles"),row.get("agent",{}).get("deployed",{}).get("cycles"),flush=True)
        return n,row
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        rows=dict(pool.map(measure,names))
    write_json(root/"results.json",{"protocol":protocol,"cases":rows})
    print(Counter(row["native"]["status"] for row in rows.values()))


if __name__=="__main__":
    main()
