"""Isolate A/B memory port counts on the previously frozen Jacobi-1D schedule."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import subprocess

from paper_final_arrays import output_arrays, reference, simulate
from paper_physical import HERE, ROOT, command, hash_file
from paper_physical_holdout_route import physical

CASE = "vericert24_jacobi_1d"
PRIOR = HERE / "paper_physical_final_20261125.json"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--single", type=Path, default=ROOT / "runs/collection_repaired_20261125")
    p.add_argument("--joint", type=Path, default=ROOT / "runs/collection_repaired_joint_20261125")
    p.add_argument("--compiler", type=Path, default=ROOT / "runs/paper_exception_checks_byte_gep_20261125/Light_HLS_Top")
    p.add_argument("--library", type=Path, default=ROOT / "runs/paper_z020_20261125/libraries")
    p.add_argument("--work", required=True, type=Path)
    p.add_argument("--out", required=True, type=Path)
    a = p.parse_args()
    work = a.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    inp = a.single.resolve() / "input" / CASE
    cfg = a.joint.resolve() / CASE / "manual" / "joint.cfg"
    protocol = json.loads((a.single / "protocol.json").read_text())
    old = json.loads(PRIOR.read_text())["cases"]["jacobi_1d"]
    compiler_sha = hash_file(a.compiler)
    if compiler_sha != protocol["compiler_sha256"]:
        raise ValueError("Wrong Light-HLS version for the frozen comparison")
    sequence = json.loads((a.joint / "results.json").read_text())["cases"][CASE]["agent"]["positive"]
    if sorted(re.findall(r"loop_pipeline label=(\S+) II=1", cfg.read_text())) != sorted(sequence):
        raise ValueError("Cannot reconstruct frozen loop selection")
    source = (inp/"oracle.c").read_text()
    arrays, pos = output_arrays(source)
    expected = old["agent"]["checksum_hex"]
    oracle = reference(source, work, arrays, pos, expected)
    env = {k:v for k,v in os.environ.items() if not k.startswith("HI_EXPERIMENT_")}
    env["HI_HLS_LIB_PATH"] = str(a.library.resolve())

    def trial(ports):
        folder = work / ("ports_" + "".join(map(str, ports)))
        folder.mkdir()
        directives = cfg.read_text()
        for mem, count in zip(("A", "B"), ports):
            if count != 1:
                directives += f"memory_interface variable={mem} scope=benchmark_main ports={count} read_latency=1\n"
        conf = folder/"jacobi.cfg"
        conf.write_text(directives)
        with (folder/"compile.log").open("w") as log:
            subprocess.run([a.compiler.resolve(), inp/"kernel.cpp", "benchmark_main", conf,
                            "--board","vericert_z020","--freq","100MHz","--synthesis","disable",
                            "--output",folder/"hls"], cwd=folder, env=env, stdout=log, stderr=log, check=True)
        rep = folder/"hls/reports"
        rtl = rep/"rtl/top.v"
        schedule = json.loads((rep/"scheduling/schedule.json").read_text())["pipelines"]
        if sorted(x["loop"] for x in schedule) != sorted(sequence):
            raise ValueError("Compiler changed selected loops")
        memory = {x["name"]:x for x in json.loads((rep/"binding/binding.json").read_text())["memory_objects"]}
        if any(memory[k]["ports"] != v for k,v in zip(("A","B"), ports)):
            raise ValueError("Memory port constraint not honored")
        command(["iverilog","-g2012","-s","tb","-o","sim",str(rtl),str(inp/"tb.v")],folder,"compile_sim.log")
        command(["vvp","sim"],folder,"simulation.log")
        match = re.search(r"PASS cycles=(\d+) checksum=([0-9a-f]+)", (folder/"simulation.log").read_text())
        if not match or match[2] != expected:
            raise ValueError("C/RTL return value mismatch")
        cycles = int(match[1])
        output = simulate(folder/"arrays",rtl,arrays,expected,cycles)
        if output != oracle:
            raise ValueError("Complete C/RTL array mismatch")
        save = {"cycles":cycles,"ii":{x["loop"]:x["achieved_ii"] for x in schedule},"checksum":expected,
                "checked_words":sum(len(x) for x in oracle.values()), "rtl_sha256":hash_file(rtl),
                "memory_ports":dict(zip(("A","B"),ports))}
        if hash_file(rtl) == old["agent"]["rtl_sha256"]:
            row = old["agent"]
            save["physical_source"] = "identical frozen routed RTL"
        else:
            (folder/"rtl.sha256").write_text(hash_file(rtl) + "\n")
            command(["vivado","-mode","batch","-notrace","-source",str(HERE/"paper_physical.tcl"),
                     "-tclargs",str(rtl),"benchmark_main","ap_clk"],folder,"route.log")
            row = physical(folder,rtl)
            save["physical_source"] = "new OOC route"
        keys = ("vivado","part","wns_ns","whs_ns","delay_ns","meets_100mhz","lut","ff","slice","dsp","bram18_equiv")
        save["physical"] = {k:row[k] for k in keys}
        save["physical"]["estimated_us"] = cycles*row["delay_ns"]/1000
        return save

    with ThreadPoolExecutor(max_workers=2) as pool:
        rows = list(pool.map(trial, [(1,1),(2,1),(1,2),(2,2)]))
    out = {"compiler_sha256": compiler_sha,"source_sha256":hash_file(inp/"kernel.cpp"),
           "frozen_directives":cfg.read_text().splitlines(),"target_clock_ns":10,
           "reference_bambu":old["bambu"],"trials":rows}
    a.out.write_text(json.dumps(out, indent=2) + "\n")


if __name__ == "__main__":
    main()
