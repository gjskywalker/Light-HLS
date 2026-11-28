"""Run and audit compiler-allocated policies on the complete Vericert PolyBench programs."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess

from flow_pilot import COMPILER, HERE, ROOT, model_call, read_json, write_json

COLLECTION = ROOT / "benchmarks/hls_scheduling_2024_2026"
# Includes all programs used during runner and hardware preflight.
DEV = {"gemm", "bicg", "jacobi_1d", "ludcmp", "nussinov", "mvt", "gesummv", "syrk"}
TAIL = [1, 0, 0, 0]
BOARD = "vericert_z020"
BOARD_PATH = ROOT / "runs/paper_z020_20261125/libraries"
TB = """module tb;
reg ap_clk=0, ap_rst=1, ap_start=0;
wire ap_done,ap_idle,ap_ready; wire [31:0] ap_return;
benchmark_main dut(.*);
always #5 ap_clk=~ap_clk;
integer cycles=0;
initial begin
 repeat(2) @(negedge ap_clk); ap_rst=0; ap_start=1;
 @(negedge ap_clk); ap_start=0;
 while (ap_done!==1'b1 && cycles<20000000) begin @(negedge ap_clk); cycles=cycles+1; end
 if(ap_done!==1'b1 || ap_return!==32'hEXPECTED)
   $fatal(1,"RTL expected EXPECTED, actual %h after %d cycles",ap_return,cycles);
 $display("PASS cycles=%0d checksum=%h",cycles,ap_return); $finish;
end
endmodule
"""


def hashfile(path):
    sha = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1 << 20), b""):
            sha.update(chunk)
    return sha.hexdigest()


def launch(command, folder, log, timeout=120, env=None):
    with (folder / log).open("w") as output:
        try:
            return subprocess.run(command, cwd=folder, stdout=output, stderr=subprocess.STDOUT,
                                  env=env, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            return 124


def adapter(case, run):
    name = case["id"]
    folder = run / "input" / name
    folder.mkdir(parents=True)
    src = COLLECTION / case["source_path"]
    if hashfile(src) != case["sha256"]:
        raise ValueError(f"source integrity failure for {name}")
    preprocessed = subprocess.check_output(["clang", "-E", "-P", "-DSYNTHESIS", "-Dmain=benchmark_main",
                                            str(src)], text=True)
    # Disable optimizer-created libc intrinsics. Preserve loops, initialization, sizes and return checksum.
    source = folder / "kernel.cpp"
    source.write_text('#pragma clang attribute push(__attribute__((no_builtin("memcpy","memset"))), '
                      'apply_to=function)\n' + preprocessed + '\n#pragma clang attribute pop\n')
    oracle = folder / "oracle.c"
    oracle.write_text(preprocessed + '\n#include <stdio.h>\nint main(void){printf("%08x\\n",'
                      '(unsigned)benchmark_main());return 0;}\n')
    subprocess.run(["clang", "-std=c99", "-O1", str(oracle), "-lm", "-o", str(folder/"oracle")], check=True)
    expected = subprocess.check_output([str(folder/"oracle")], text=True, timeout=120).strip()
    if not re.fullmatch("[0-9a-f]{8}", expected):
        raise ValueError(f"unexpected native checksum {expected}")
    (folder / "tb.v").write_text(TB.replace("EXPECTED", expected))
    return {"original": case["sha256"], "adapter": hashfile(source), "checksum": expected}


def compile_variant(compiler, lib, source, folder, weights, blocks=None):
    folder.mkdir(parents=True)
    env = dict(os.environ, HI_HLS_LIB_PATH=str(lib))
    for key in ("HI_EXPERIMENT_SCHEDULE_GRAPH", "HI_EXPERIMENT_SCHEDULE_REPLAY",
                "HI_EXPERIMENT_ORDER_WEIGHTS", "HI_EXPERIMENT_ORDER_BLOCKS"):
        env.pop(key, None)
    if weights is not None:
        env["HI_EXPERIMENT_ORDER_WEIGHTS"] = json.dumps(weights)
    if blocks is not None:
        env["HI_EXPERIMENT_ORDER_BLOCKS"] = json.dumps(blocks)
    status = launch([str(compiler), str(source), "benchmark_main", "--board", BOARD, "--freq", "100MHz",
                     "--synthesis", "disable", "--output", str(folder / "hls")], folder, "compile.log", env=env)
    if status:
        log = (folder / "compile.log").read_text(errors="replace")
        errors = re.findall(r"backend error \[([^\]]+)\]|(Assertion .*? failed)", log)
        return {"status": "compile_failed", "exit": status, "diagnostic":
                " ".join(filter(None, errors[0]))[:220] if errors else "see compile.log"}
    report = read_json(folder / "hls/reports/scheduling/schedule.json")
    return {"status": "compiled", "states": {b["name"]: b["state_count"] for b in report["blocks"]},
            "port_conflicts": report["memory_port_conflicts"],
            "order_cycles": report["memory_ordering_cycles"]}


def simulate(source_dir, folder, stats):
    if stats["status"] != "compiled":
        return stats
    status = launch(["iverilog", "-g2012", "-s", "tb", "-o", "sim", "hls/reports/rtl/top.v",
                     str(source_dir / "tb.v")], folder, "iverilog.log")
    if status:
        return dict(stats, status="rtl_elaboration_failed")
    status = launch(["vvp", "sim"], folder, "rtl.log", timeout=240)
    log = (folder / "rtl.log").read_text(errors="replace")
    hit = re.search(r"PASS cycles=(\d+) checksum=([0-9a-f]{8})", log)
    if status or not hit:
        return dict(stats, status="rtl_timeout" if status == 124 else "rtl_failed", diagnostic=log[-320:])
    return dict(stats, status="valid", cycles=int(hit[1]), checksum=hit[2])


def choose_blocks(native, candidate):
    """Select before simulation, using only compiler per-block costs under the same CFG."""
    if candidate["status"] != "compiled" or native["states"].keys() != candidate["states"].keys():
        return []
    return [b for b in native["states"] if candidate["states"][b] < native["states"][b]]


def variant(case, label, weights, run, compiler, lib):
    name = case["id"]
    folder = run / label / name
    inp = run / "input" / name
    native = read_json(run / "native" / name / "result.json") if weights is not None else None
    if native and native["status"] != "valid":
        folder.mkdir(parents=True)
        result = {"status": "native_unavailable", "native_status": native["status"]}
    else:
        raw = compile_variant(compiler, lib, inp / "kernel.cpp", folder, weights)
        if native is None:
            result = simulate(inp, folder, raw)
        else:
            blocks = choose_blocks(native, raw)
            if not blocks:
                result = dict(status="valid", cycles=native["cycles"], checksum=native["checksum"],
                              choice="native", selected_blocks=[], candidate_status=raw["status"])
            else:
                selected = raw
                active = folder
                if any(raw["states"][b] > native["states"][b] for b in native["states"]):
                    active = folder / "mixed"
                    selected = compile_variant(compiler, lib, inp/"kernel.cpp", active, weights, blocks)
                if (selected["status"] != "compiled" or selected["states"].keys() != native["states"].keys()
                        or any(v > native["states"][b] for b, v in selected["states"].items())):
                    result = dict(status="valid", cycles=native["cycles"], checksum=native["checksum"],
                                  choice="native_after_cost_check", selected_blocks=blocks)
                else:
                    result = dict(simulate(inp, active, selected), choice="mixed" if active != folder else "policy",
                                  selected_blocks=blocks)
    write_json(folder / "result.json", result)
    return result


def summarize(rows, native):
    valid = [n for n, row in rows.items() if row["status"] == "valid"]
    return {"valid": len(valid), "wins": sum(rows[n]["cycles"] < native[n]["cycles"] for n in valid),
            "geomean": statistics.geometric_mean(rows[n]["cycles"]/native[n]["cycles"] for n in valid)
            if valid else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--library", type=Path, default=BOARD_PATH)
    parser.add_argument("--compiler", type=Path, default=COMPILER)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--stage", choices=("native", "learn", "test"), required=True)
    args = parser.parse_args()
    run, lib, compiler = args.output.resolve(), args.library.resolve(), args.compiler.resolve()
    cases = [c for c in read_json(COLLECTION/"cases.json") if c["id"].startswith("vericert24_")]
    dev = [c for c in cases if c["id"][11:] in DEV]
    test = [c for c in cases if c not in dev]
    assert len(dev) == len(DEV) and len(cases) == 27

    def batch(group, label, weights):
        def task(c):
            row = variant(c, label, weights, run, compiler, lib)
            print(label, c["id"], row["status"], row.get("cycles", ""), flush=True)
            return c["id"], row
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            return dict(pool.map(task, group))

    if args.stage == "native":
        run.mkdir(parents=True, exist_ok=False)
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            input_rows = dict(zip((c["id"] for c in cases), pool.map(lambda c: adapter(c, run), cases)))
        write_json(run/"protocol.json", {"dev": [c["id"] for c in dev], "holdout": [c["id"] for c in test],
                   "inputs": input_rows, "model": "gpt-6.1-sol", "effort": "low", "calls": 3,
                   "board": BOARD, "part": "xc7z020clg484-1", "period_ns": 10,
                   "compiler_sha256": hashfile(compiler), "runner_sha256": hashfile(Path(__file__)),
                   "board_sha256": {str(p.relative_to(lib)): hashfile(p) for p in sorted(lib.rglob("*")) if p.is_file()},
                   "selection": "dev coverage, cycle ratio, prefer strongest prespecified baseline on ties (tail or native)"})
        dev_rows = batch(dev, "native", None)
        write_json(run/"native_dev.json", dev_rows)
        write_json(run/"native.json", {**dev_rows, **batch(test, "native", None)})
        return
    protocol = read_json(run/"protocol.json")
    if protocol["compiler_sha256"] != hashfile(compiler):
        raise ValueError("compiler changed after freeze")
    native = read_json(run/("native_dev.json" if args.stage == "learn" else "native.json"))
    if args.stage == "learn":
        tail = batch(dev, "tail", TAIL)
        history = []
        available = [("tail", TAIL, tail), ("native", None, {c["id"]: native[c["id"]] for c in dev})]
        prompt = ("Improve the order in which a real HLS backend allocates ordinary basic-block operations. "
                  "Propose 4 integer weights in [-64,64] for the score dot(weights,[latency-weighted remaining "
                  "critical path, max remaining memory requests on a dependence path, fanout, intrinsic latency]). "
                  "The highest score ready instruction is allocated first, ties use original IR order. Dependencies, "
                  "writes and FIFO ordering are enforced, actual start cycles, forwarding and controls stay with the compiler. "
                  "On deployment, only blocks strictly reducing compiler state count vs native are selected. "
                  "Propose an alternative to [1,0,0,0], reasoning about serial ports and load sampling. "
                  "Return weights as a JSON array inside the weights string and your concise hypothesis. "
                  "Do not use kernel IDs or modify kernels. Only development RTL results follow.\n")
        def evidence(rows):
            return {n: {"status": v["status"], "cycles": v.get("cycles"), "selection":v.get("choice"),
                        "port_conflicts":v.get("port_conflicts")} for n,v in rows.items()}
        for label, w, rows in available:
            history.append({"label": label, "weights": w, "dev": evidence(rows)})
        for i in range(1, 4):
            reply, receipt = model_call(prompt + json.dumps(history), ("weights","hypothesis"),
                                        run/f"model_{i}", "gpt-6.1-sol", "low")
            try:
                weights = json.loads(reply["weights"]) if reply else None
            except ValueError:
                weights = None
            if not isinstance(weights, list) or len(weights)!=4 or any(type(v)!=int or abs(v)>64 for v in weights):
                history.append({"call":i,"error":"invalid weights","receipt":receipt})
                continue
            label = f"agent_{i}"
            rows = batch(dev, label, weights)
            available.append((label, weights, rows))
            history.append({"call":i, "label":label,"weights":weights,"hypothesis":reply["hypothesis"],
                            "dev":evidence(rows),"receipt":receipt})
        def score(item):
            s = summarize(item[2],native)
            return (-s["valid"],s["geomean"] if s["geomean"] is not None else 10**9)
        best = min(available,key=score)
        agents = [item for item in available if item[0].startswith("agent_")]
        if not agents:
            raise RuntimeError("no live model produced a candidate")
        best_agent = min(agents,key=score)
        write_json(run/"frozen.json", {"winner":best[:2],"agent":best_agent[:2],"history":history,
                                       "dev_summary":{x[0]:summarize(x[2],native) for x in available}})
        return
    frozen = read_json(run/"frozen.json")
    agent_label, w = frozen["agent"]
    groups = {"tail":batch(test,"tail",TAIL),agent_label:batch(test,agent_label,w)}
    for label in groups:
        groups[label].update({c["id"]:read_json(run/label/c["id"]/"result.json") for c in dev})
    with (HERE/"vericert_published_cycles_20261125.csv").open() as f:
        paper = {row["design"].replace("-","_"):row for row in csv.DictReader(f)}
    rows = {}
    for c in cases:
        n = c["id"]
        rows[n] = {"split":"dev" if c in dev else "holdout", "native":native[n],
                   "tail":groups["tail"][n],"agent":groups[agent_label][n],"published":paper[n[11:]]}
    write_json(run/"results.json", {"protocol":protocol,"frozen":frozen,"cases":rows})


if __name__ == "__main__":
    main()
