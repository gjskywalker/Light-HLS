"""Exploratory extension: compose profitable pipeline choices from the frozen three probes."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path

from flow_pilot import COMPILER, read_json, write_json
from paper_order_flow import BOARD, BOARD_PATH, hashfile, launch, simulate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, default=COMPILER)
    parser.add_argument("--library", type=Path, default=BOARD_PATH)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    run, dest, compiler = args.input.resolve(), args.output.resolve(), args.compiler.resolve()
    if hashfile(compiler) != read_json(run/"protocol.json")["compiler_sha256"]:
        raise ValueError("compiler changed since single-pipeline evaluation")
    rows = read_json(run/"results.json")["cases"]
    dest.mkdir(parents=True, exist_ok=False)
    def task(item):
        n,row=item
        out = {}
        if row["native"]["status"] != "valid":
            return n,{"status":"native_unavailable"}
        cache = {}
        for group in ("manual","agent"):
            base = row[group]["deployed"]
            attempts = row[group]["pipeline"]["attempts"]
            labels = tuple(sorted(k for k,v in attempts.items() if v["gain_estimate"] > 0))
            out[group] = {"status":base["status"],"cycles":base.get("cycles"),"source":"single",
                          "positive":labels}
            if len(labels) < 2:
                continue
            if labels not in cache:
                folder=dest/n/group
                folder.mkdir(parents=True)
                cfg=folder/"joint.cfg"
                cfg.write_text("".join(f"loop_pipeline label={l} II=1\n" for l in labels))
                env={k:v for k,v in os.environ.items() if not k.startswith("HI_EXPERIMENT_")}
                env["HI_HLS_LIB_PATH"]=str(args.library.resolve())
                exitcode=launch([str(compiler),str(run/"input"/n/"kernel.cpp"),"benchmark_main",str(cfg),
                                 "--board",BOARD,"--freq","100MHz","--synthesis","disable","--output",str(folder/"hls")],
                                folder,"compile.log",env=env)
                candidate = {"status":"compiler_rejected"}
                if exitcode==0:
                    pipes=read_json(folder/"hls/reports/scheduling/schedule.json")["pipelines"]
                    if sorted(p["loop"] for p in pipes)==list(labels) and all(
                            p["trip_count"]==attempts[p["loop"]]["trip"] and
                            p["achieved_ii"]<=attempts[p["loop"]]["ii"] for p in pipes):
                        candidate=simulate(run/"input"/n,folder,{"status":"compiled"})
                cache[labels] = candidate
            joint = cache[labels]
            out[group]["joint_status"] = joint["status"]
            if joint["status"] != "compiler_rejected":
                out[group].update(status=joint["status"],cycles=joint.get("cycles"),source="joint")
        return n,out
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results=dict(pool.map(task, rows.items()))
    write_json(dest/"results.json", {"parent_run":hashfile(run/"results.json"),"cases":results})
    print(json.dumps({n:v for n,v in results.items() if v.get("agent",{}).get("source")=="joint"}))


if __name__=="__main__":
    main()
