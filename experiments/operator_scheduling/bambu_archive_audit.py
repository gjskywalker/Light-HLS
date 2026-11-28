"""Check the archived Bambu command, cycles, datapaths and actual memory-port use."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import csv
import hashlib
import io
import json
from pathlib import Path
import re
import statistics
import subprocess
import tarfile

from paper_physical import ARCHIVE, command, hash_file
from paper_physical_final_route import final_rtl

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PROBES = {"bicg", "jacobi-1d", "trisolv"}
FROZEN = HERE / "paper_repaired_results_20261125.json"


def sha(s):
    return hashlib.sha256(s.encode()).hexdigest()


def describe(rtl):
    version = re.search(r"Version: (.*?) - Revision (\S+)", rtl)
    argv = re.search(r"executed with: \S+/bambu (.*?)\s*\n", rtl)[1].split()
    dp = rtl.split("module datapath_main_top(", 1)[1].split("endmodule", 1)[0]
    ctl = rtl.split("module controller_main_top(", 1)[1].split("endmodule", 1)[0]
    arrays = {}
    for m in re.finditer(r"^\s*(ARRAY_1D_\w+) #\((.*?)\) (\w+) \(", dp, re.M | re.S):
        kind, pars, name = m.groups()
        arrays[name] = {"kind": kind, "words": int(re.search(r"\.n_elements\((\d+)\)", pars)[1]),
                        "access_lanes": int(re.search(r"\.PORTSIZE_sel_LOAD\((\d+)\)", pars)[1])}
    return {"version": version[1], "revision": version[2], "args": argv, "sha256": sha(rtl),
            "top_fsm_states": len(set(re.findall(r"\b(S_\d+)\s*=", ctl))), "memories": arrays,
            "top_mult_expr_fu_instances": len(re.findall(r"^\s*mult_expr_FU #", dp, re.M)),
            "additional_controllers": [n for n in re.findall(r"^module controller_(\w+)\(", rtl, re.M)
                                       if n != "main_top"],
            "modulo_instances": re.findall(r"^\s*modulo\s+(\w+)\s*\(", dp, re.M)}


def memory_observer(variant, arrays, modulo, jacobi=False):
    if variant == "light":
        clk, start, done, dp = "tb.ap_clk", "tb.ap_start", "tb.ap_done", "tb.dut"
    else:
        tb = "main_top_tb_top.DUT"
        clk, start, done = [f"{tb}.{sig}" for sig in ("clock", "start_port", "done_port")]
        dp = tb + ".main_top._main_top_i0.Datapath_i"
    decls, add, out = ["integer ticks=0;"], ["ticks++;"], []
    for i, (name, props) in enumerate(arrays.items()):
        instance = f"{dp}.{name}"
        ports = props["access_lanes"]
        if variant == "light":
            reads = [f"({dp}.{name}_ce{p} && !{dp}.{name}_we{p})" for p in range(ports)]
            writes = [f"({dp}.{name}_ce{p} && {dp}.{name}_we{p})" for p in range(ports)]
        else:
            reads, writes = [], []
            for sel, result in (("LOAD", reads), ("STORE", writes)):
                for p in range(ports):
                    result.append(f"(({instance}.in4[{p}] & {instance}.sel_{sel}[{p}]) | "
                                  f"{instance}.proxy_sel_{sel}[{p}])")
        decls.append(f"integer r{i}=0,w{i}=0,dual{i}=0,multi{i}=0,unknown{i}=0,nr{i},nw{i};")
        decls.append(f"integer first_r{i}=0, last_w{i}=0;")
        for kind, exprs in [("nr", reads), ("nw", writes)]:
            add.append(f"{kind}{i}=0;")
            for expr in exprs:
                add.append(f"if ({expr} === 1'b1) {kind}{i}={kind}{i}+1; "
                           f"else if ({expr} !== 1'b0) unknown{i}=unknown{i}+1;")
        add.append(f"r{i}+=nr{i}; w{i}+=nw{i}; if(nr{i}>1) dual{i}++; if(nr{i}+nw{i}>1) multi{i}++;")
        add.append(f"if(nr{i}>0 && first_r{i}==0) first_r{i}=ticks; if(nw{i}>0) last_w{i}=ticks;")
        out.append(f'$display("MEM {name} %0d %0d %0d %0d %0d %0d %0d",r{i},w{i},dual{i},'
                   f'multi{i},unknown{i},first_r{i},last_w{i});')
    if jacobi:
        # There are exactly 30*2 initialization stores, with no intervening RAM loads.
        # The 20 time steps perform two sweeps of 28 stores each. Track store edges
        # across BOTH memories: their identity changes at every sweep boundary.
        decls.append("integer wr_total=0, first_kernel=0, last_kernel=0, same_only=0;")
        add += ["wr_total+=nw0+nw1;",
                "if (nw0+nw1>0 && wr_total>=61 && first_kernel==0) first_kernel=ticks;",
                "if (nw0+nw1>0 && wr_total>=61) last_kernel=ticks;",
                "if(nw0+nw1>1 && wr_total>=61) same_only++;"]
        out.append('$display("STORES %0d %0d %0d %0d",wr_total,first_kernel,last_kernel,same_only);')
    if modulo:
        (name,) = modulo
        sig = f"{dp}.{name}"
        decls += ["integer calls=0,ret=0;"]
        add += [f"if({sig}.start_port === 1'b1) calls++;", f"if({sig}.done_port === 1'b1) ret++;"]
        out += ['$display("MODULO %0d %0d",calls,ret);']
    return "\n".join(["module observer;", "bit active=0;", *decls, f"always @(posedge {clk}) begin",
                      f"if({start} === 1'b1) active=1;", "if(active) begin", *add, "end end",
                      f"always @(negedge {clk}) if(active && {done} === 1'b1) active=0;",
                      "final begin", *out, "end endmodule"])


def simulate(job):
    mode, design, work, rtl, tb, checksum, cycles, arrays, modulo = job
    work.mkdir(parents=True)
    (work / "rtl.v").write_text(rtl)
    if mode == "light":
        top = "tb"
    else:
        top = "main_top_tb_top"
        tb, n = re.subn(r'\$fopen\("[^"]*/(values.txt|results.txt)"', r'$fopen("\1"', tb)
        if n != 2:
            raise ValueError("Unexpected paths in author testbench")
        (work / "values.txt").write_text(f"o{int(checksum,16):032b}\ne\n")
    (work / "tb.v").write_text(tb)
    probes = design in PROBES
    cmd = ["iverilog", "-g2012", "-s", top]
    if probes:
        (work / "observer.sv").write_text(memory_observer(mode, arrays, modulo, design == "jacobi-1d"))
        cmd += ["-s", "observer", "observer.sv"]
    command(cmd + ["-o", "sim", "rtl.v", "tb.v"], work, "iverilog.log")
    with (work / "simulation.log").open("w") as log:
        subprocess.run(["vvp", "sim"], cwd=work, stdout=log, stderr=subprocess.STDOUT, timeout=480, check=True)
    log = (work / "simulation.log").read_text()
    if mode == "light":
        m = re.search(r"PASS cycles=(\d+) checksum=([0-9a-f]+)", log)
        if not m or int(m[1]) != cycles or int(m[2], 16) != int(checksum, 16):
            raise ValueError(f"Light-HLS check failed in {work}")
    else:
        if [int(x) for x in (work / "results.txt").read_text().split()] != [1, cycles]:
            raise ValueError(f"Archive simulation differs: {work}")
    mem = {}
    for m in re.finditer(r"^MEM (\w+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)$", log, re.M):
        mem[m[1]] = dict(zip(("read_requests", "write_requests", "two_or_more_reads_cycles",
                              "two_or_more_requests_cycles", "unknown_enables", "first_read_tick", "last_write_tick"), map(int, m.groups()[1:])))
    if probes and (mem.keys() != arrays.keys() or any(v["unknown_enables"] for v in mem.values())):
        raise ValueError(f"Memory interface coverage incomplete in {work}: {mem}")
    result = {"verified_cycles": cycles, "c_checksum": checksum, "rtl_sha256": sha(rtl)}
    if probes:
        result["memory_interface_counts"] = mem
        if design == "jacobi-1d":
            stores = re.search(r"^STORES (\d+) (\d+) (\d+) (\d+)$", log, re.M)
            count, first, last, multiple = map(int, stores.groups())
            result["jacobi_store_interval"] = {"all_stores": count, "first_store_after_60": first,
                                                "last_store": last, "kernel_multi_write_cycles": multiple}
        if modulo:
            m = re.search(r"^MODULO (\d+) (\d+)$", log, re.M)
            result["modulo_start_done_pulses"] = list(map(int, m.groups()))
    return mode, design, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=ARCHIVE)
    parser.add_argument("--single", type=Path, default=ROOT / "runs/collection_repaired_20261125")
    parser.add_argument("--joint", type=Path, default=ROOT / "runs/collection_repaired_joint_20261125")
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=5)
    parser.add_argument("--all-rtl", action="store_true", help="add checksum/cycle replays for all archived pairs")
    a = parser.parse_args()
    if hash_file(a.archive) != "f43b0201b64b23ac071ae54af317984a40fb067943f1e09ccd22bd17c0e100f5":
        raise ValueError("The input archive does not match the published artifact.")
    work = a.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    cases = json.loads(FROZEN.read_text())["cases"]
    joint = json.loads((a.joint / "results.json").read_text())["cases"]
    rows, jobs = {}, []
    # The original tar contains a second bambu-opt/output/ tree. Use only the
    # canonical directories alongside the archived run-specific exec.csv.
    with tarfile.open(a.archive, "r:xz") as tar:
        files = {m.name: tar.extractfile(m).read().decode() for m in tar if m.isfile()
                 and "/output/" not in m.name and "/bambu-" in m.name
                 and m.name.endswith(("/main_top.v", "/testbench_main_top.v", "/exec.csv"))}
    for mode in ("opt", "noopt"):
        prefix = "synthesis-results/bambu-" + mode + "/"
        for design, raw_cycles in csv.reader(io.StringIO(files[prefix + "exec.csv"])):
            case = "vericert24_" + design.replace("-", "_")
            record = cases[case]
            cycles = int(raw_cycles)
            if cycles != record["published"]["bambu-" + mode]:
                raise ValueError("Reference cycle mismatch")
            path, = [n for n in files if n.startswith(prefix) and n.endswith(f"/{design}/main_top.v")]
            rtl = files[path]
            d = describe(rtl)
            row = rows.setdefault(design, {"agent_cycles": record["agent_joint"]})
            row[mode] = {"archive_path": path, "cycles": cycles, **d}
            if a.all_rtl or design in PROBES:
                tb = files[path.replace("main_top.v", "HLS_output/simulation/testbench_main_top.v")]
                jobs.append((mode, design, work/mode/design, rtl, tb, record["observed_checksum"], cycles,
                             d["memories"], d["modulo_instances"]))
            if mode == "opt" and design in PROBES:
                light, _ = final_rtl(a.single, a.joint, case, "agent", joint[case])
                light_text = light.read_text()
                fields = re.findall(r"^\s*reg (\w+)_ce(\d+);$", light_text, re.M)
                ports = Counter(name for name, _ in fields)
                arrays = {name: {"access_lanes": count} for name,count in ports.items()}
                if not arrays: raise ValueError("No memories found in Light-HLS output")
                jobs.append(("light", design, work/"light"/design, light_text,
                             (a.single/"input"/case/"tb.v").read_text(), record["observed_checksum"],
                             record["agent_joint"], arrays, []))
    with ThreadPoolExecutor(a.jobs) as pool:
        for mode, design, result in pool.map(simulate, jobs):
            rows[design].setdefault("rtl_replay", {})[mode] = result
    data = list(rows.values())
    def compare(left, right):
        return {"less": sum(x < y for x,y in zip(left,right)), "equal": sum(x == y for x,y in zip(left,right)),
                "more": sum(x > y for x,y in zip(left,right)), "geomean_ratio":
                statistics.geometric_mean(x/y for x,y in zip(left,right))}
    default = [r["opt"]["cycles"] for r in data]
    noopt = [r["noopt"]["cycles"] for r in data]
    agent = [r["agent_cycles"] for r in data]
    aggregate = {"default_over_noopt": compare(default,noopt), "agent_over_default": compare(agent,default),
                 "agent_over_noopt": compare(agent,noopt), "default_beats_agent_only_when_optimized":
                 sum(d < a < n for d,a,n in zip(default,agent,noopt))}
    a.out.write_text(json.dumps({"archive_sha256": hash_file(a.archive), "frozen_agent_data_sha256":
                                 hash_file(FROZEN), "aggregate": aggregate, "cases": rows}, indent=2) + "\n")


if __name__ == "__main__":
    main()
