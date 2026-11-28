"""Explore a frozen, budgeted outer loop for choosing a real compiler pipeline."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
from concurrent.futures import ThreadPoolExecutor

from flow_pilot import COMPILER, model_call, read_json, write_json
from paper_order_flow import BOARD, BOARD_PATH, hashfile, launch, simulate, summarize

MANUAL = [1, 0, 0, 0, 0]
FEATURES = ("depth", "array_refs", "multiplies", "conditional_ops", "compound_updates")
BUDGET = 3


def find_loops(source):
    tree = json.loads(subprocess.check_output(["clang++", "-w", "-Xclang", "-ast-dump=json",
                                               "-fsyntax-only", str(source)]))
    loops = {}
    def walk(node, depth):
        inner = node.get("inner", [])
        kind = node.get("kind")
        counted = kind in ("ForStmt", "WhileStmt", "DoStmt")
        counts = [0, int(kind == "ArraySubscriptExpr"), int(node.get("opcode") == "*"),
                  int(kind in ("IfStmt","ConditionalOperator")), int(kind == "CompoundAssignOperator")]
        for child in inner:
            sub = walk(child, depth + counted)
            counts = [a+b for a,b in zip(counts,sub)]
        if kind == "LabelStmt" and node.get("name","").startswith("Loop_kernel_") and inner:
            # Locations provide the deterministic source-order tie break across loops.
            loops[node["name"]] = {"features": [depth + 1, *counts[1:]],
                                   "offset": node["range"]["begin"].get("offset",0)}
        return counts
    for function in tree.get("inner",[]):
        if function.get("kind") == "FunctionDecl" and function.get("name","").startswith("kernel_"):
            walk(function,0)
    return loops


def probe(run, inputs, name, label, native, lib, compiler):
    folder = run / "probes" / name / label
    result_path = folder / "compile.json"
    if result_path.exists():
        return read_json(result_path)
    folder.mkdir(parents=True)
    cfg = folder/"pipeline.cfg"
    cfg.write_text(f"loop_pipeline label={label} II=1\n")
    env = {k:v for k,v in os.environ.items() if not k.startswith("HI_EXPERIMENT_")}
    env["HI_HLS_LIB_PATH"] = str(lib)
    status = launch([str(compiler), str(inputs/"input"/name/"kernel.cpp"), "benchmark_main", str(cfg),
                     "--board", BOARD, "--freq","100MHz","--synthesis","disable","--output",str(folder/"hls")],
                    folder,"compile.log",env=env)
    result = {"status":"rejected","gain_estimate":0}
    if status == 0:
        report = read_json(folder/"hls/reports/scheduling/schedule.json")
        pipes = report["pipelines"]
        if len(pipes) == 1 and pipes[0]["loop"] == label:
            pipe = pipes[0]
            name_in_ir = pipe["ir_loop"].split("-",1)[-1]
            block = re.sub(r"[^a-zA-Z0-9_]","_",name_in_ir)
            blocks = native["states"]
            # For a simple, nonflattened loop use the same iteration boundary.
            if block in blocks and pipe["flattened_loop_count"] == 1:
                trip, ii = pipe["trip_count"], pipe["achieved_ii"]
                native_cost = blocks[block] * trip
                piped_cost = (trip-1)*ii + pipe["pipeline_latency"] + 2
                result = {"status":"compiled","gain_estimate":native_cost-piped_cost,"trip":trip,
                          "ii":ii,"bottleneck":pipe["limiting_factor"],"block":block}
            else:
                result["status"]="unmatched_loop_boundary"
        else:
            result["status"]="directive_not_realized"
    write_json(result_path,result)
    return result


def policy(run, inputs, names, native, features, weights, lib, compiler, jobs):
    def evaluate(n):
        ref = native[n]
        if ref["status"] != "valid":
            return n, {"status":"native_unavailable"}
        loops = features[n]
        def key(label):
            ops = loops[label]
            return -sum(a*b for a,b in zip(ops["features"],weights)), ops["offset"]
        labels = sorted(loops,key=key)[:BUDGET]
        attempts = {l:probe(run,inputs,n,l,ref,lib,compiler) for l in labels}
        good = [l for l in labels if attempts[l]["gain_estimate"] > 0]
        if not good:
            return n, {"status":"valid","choice":"native","cycles":ref["cycles"],"attempts":attempts}
        label = max(good,key=lambda l: attempts[l]["gain_estimate"])
        folder = run/"probes"/n/label
        cache = folder/"rtl.json"
        if not cache.exists():
            write_json(cache,simulate(inputs/"input"/n,folder,{"status":"compiled"}))
        return n, dict(read_json(cache),choice=label,attempts=attempts)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        return dict(pool.map(evaluate,names))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--library",type=Path,default=BOARD_PATH)
    parser.add_argument("--compiler",type=Path,default=COMPILER)
    parser.add_argument("--jobs",type=int,default=5)
    parser.add_argument("--stage",choices=("learn","test"),required=True)
    args=parser.parse_args()
    inputs,run,compiler,lib=(p.resolve() for p in (args.inputs,args.output,args.compiler,args.library))
    protocol=read_json(inputs/"protocol.json")
    if hashfile(compiler)!=protocol["compiler_sha256"]:
        raise ValueError("compiler changed")
    learn = args.stage=="learn"
    names = protocol["dev" if learn else "holdout"]
    native = read_json(inputs/("native_dev.json" if learn else "native.json"))
    features = {n:find_loops(inputs/"native"/n/"hls/reports/.lighthls/tmp_loopLabeled.cc")
                for n in names if native[n]["status"]=="valid"}
    if learn:
        run.mkdir(parents=True,exist_ok=False)
        write_json(run/"protocol.json", {"source":hashfile(Path(__file__)), "native_inputs":protocol,
                   "max_unique_probes_per_program_per_policy":BUDGET,"features":FEATURES,"model_calls":2,
                   "selection":"dev pass count then cycle ratio; prefer earlier policy in ties"})
    summary = []
    policies = [("innermost",MANUAL)]
    if not learn:
        policies.append(tuple(read_json(run/"frozen.json")["agent"]))
    measured = {}
    for i in range(3 if learn else len(policies)):
        if learn and i:
            msg = ("Design loop selection for a real HLS pipeline. There are at most 3 hardware compiler probes "
                   "per complete C program. It tries top-ranked source computation loops, pragmas target II=1. "
                   "The backend keeps the legal simple loop with greatest positive predicted cycle saving "
                   "(or native). Propose a different linear ranking as a weights JSON array of 5 integer values "
                   "in [-64,64], returned as a string, and a diagnostic hypothesis. Features in order: nesting depth, "
                   "array references including nested subscripts, multiply AST operations, conditional statements, "
                   "compound assignments. Counts include nested bodies. Higher dot score wins and ties follow source order. "
                   "Only the development cases and previous results follow.\n" + json.dumps({"features":features,"history":summary}))
            reply,receipt=model_call(msg,("weights","hypothesis"),run/f"model_{i}","gpt-6.1-sol","low")
            weights=None
            if reply:
                try: weights=json.loads(reply["weights"])
                except ValueError: pass
            if not isinstance(weights,list) or len(weights)!=5 or any(type(v)!=int or abs(v)>64 for v in weights):
                summary.append({"call":i,"status":"invalid","receipt":receipt})
                continue
            label=f"agent_{i}"
            write_json(run/f"reply_{i}.json",{"reply":reply,"receipt":receipt})
        else:
            label,weights=policies[i]
        rows=policy(run,inputs,names,native,features,weights,lib,compiler,args.jobs)
        measured[label]={"weights":weights,"rows":rows,"score":summarize(rows,native)}
        summary.append({"label":label,"weights":weights,"geomean":measured[label]["score"],
                        "feedback":{n:{"status":r["status"],"chosen":r.get("choice"),"cycles":r.get("cycles"),
                                       "native":native[n].get("cycles"),"attempts":r.get("attempts")} for n,r in rows.items()}})
        print(label,measured[label]["score"],flush=True)
    write_json(run/("dev.json" if learn else "test.json"),measured)
    if learn:
        agents=[l for l in measured if l.startswith("agent_")]
        if not agents:
            raise RuntimeError("no live model candidate")
        def rank(label):
            s=measured[label]["score"]
            return -s["valid"], s["geomean"] or 1e9
        best=min(agents,key=rank)
        winner=min(measured,key=rank)
        write_json(run/"frozen.json",{"agent":[best,measured[best]["weights"]],
                                      "winner":[winner,measured[winner]["weights"]]})


if __name__=="__main__":
    main()
