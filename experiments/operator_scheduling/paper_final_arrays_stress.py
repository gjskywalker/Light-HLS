"""Inject bounded signed operands into the frozen, previously verified four benchmark RTLs."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
from pathlib import Path
import random
import re
import struct
import subprocess

from paper_final_arrays import digest, output_arrays
from paper_physical_final_route import final_rtl, hash_file, read

# First kernel preheader following completion of the original initializer.
CASES = {
    "gemm": ("for_cond1_preheader_i12", ["A", "B", "C"]),
    "2mm": ("for_cond1_preheader_i16", ["A", "B", "C", "D"]),
    "3mm": ("for_cond1_preheader_i19", ["A", "B", "C", "D"]),
    "doitgen": ("for_cond1_preheader_i12", ["A", "C4"]),
}
SEEDS = (20261125, 20261126, 20261127)


def case(name, single, joint, archive, deployed, output):
    key = "vericert24_" + name
    folder = output / name
    folder.mkdir()
    rtl, choice = final_rtl(single, joint, key, "agent", deployed["cases"][key])
    auth = archive["cases"][key]["agent"]
    if hash_file(rtl) != auth["rtl_sha256"]:
        raise ValueError(f"Actual proposed RTL differs from the audited official RTL for {name}")
    if auth["rtl_sha256"] != archive["cases"][key]["manual"]["rtl_sha256"]:
        raise ValueError("Manual and agent deploy different RTL")
    first, operands = CASES[name]
    schedule = read(rtl.parents[1] / "scheduling/schedule.json")
    state, = [block["first_state"] for block in schedule["blocks"] if block["name"] == first]
    ram = {n:int(last)+1 for n,last in re.findall(r"reg \[31:0\] (\w+)_storage \[0:(\d+)\];", rtl.read_text())}
    source = (single / "input" / key / "oracle.c").read_text()
    result_arrays, ret = output_arrays(source)
    result_name, = result_arrays
    depth = math.prod(result_arrays[result_name])
    if ram[result_name] != depth:
        raise ValueError("Unexpected output depth")
    entry = source.index("int benchmark_main()")
    call = source.index("kernel_" + name, entry)
    observe = (f'FILE *dump=fopen("c_output.bin","wb"); if (!dump || fwrite(&{result_name},'
               f'sizeof({result_name}),1,dump)!=1) abort(); fclose(dump);\n')
    load = "\n".join(f'read_input("{n}.bin",&{n},sizeof({n}));' for n in operands)
    adapter = ('#include <stdio.h>\n#include <stdlib.h>\n_Static_assert(sizeof(int)==4,"4-byte int");\n'
               'static void read_input(const char *path, void *buffer, size_t bytes) {'
               'FILE *f=fopen(path,"rb"); if (!f || fread(buffer,bytes,1,f)!=1 || fgetc(f)!=EOF) abort();'
               'fclose(f);}\n' + source[:call] + load + "\n" + source[call:ret] + observe + source[ret:])
    (folder / "oracle.c").write_text(adapter)
    subprocess.run(["clang", "-std=c11", "-O1", "-fsanitize=undefined", "-fno-sanitize-recover=all",
                    "oracle.c", "-o", "oracle"], cwd=folder, check=True)
    injection = []
    for n in operands:
        injection.extend([f'for (i=0; i<{ram[n]}; i=i+1) if (dut.{n}_storage[i] !== 32\'d0) '
                          f'$fatal(1, "Original initializer mismatch {n}[%d]",i);',
                          f'$readmemh("{n}.hex",dut.{n}_storage);'])
    (folder / "tb.sv").write_text(f"""module tb;
  reg ap_clk=0, ap_rst=1, ap_start=0;
  wire ap_done, ap_idle, ap_ready; wire [31:0] ap_return;
  integer cycles=0, injected=0, injection_cycle=-1, i, file, states;
  benchmark_main dut(.*);
  always #5 ap_clk=~ap_clk;
  initial begin
    states=$fopen("fsm.bin","wb");
    repeat (2) @(negedge ap_clk); ap_rst=0; ap_start=1;
    @(negedge ap_clk); ap_start=0;
    while (ap_done !== 1'b1 && cycles < 2000000) begin
      $fwrite(states,"%c",dut.state);
      if (!$test$plusargs("official") && !injected && dut.state == {state}) begin
        {chr(10).join(injection)}
        injected=1; injection_cycle=cycles;
      end
      @(negedge ap_clk); cycles=cycles+1;
    end
    $fclose(states);
    if ((!$test$plusargs("official") && !injected) || ap_done !== 1'b1)
      $fatal(1, "missing injection or completion");
    file=$fopen("rtl_output.hex","w");
    for (i=0; i<{depth}; i=i+1) $fdisplay(file,"%08h",dut.{result_name}_storage[i]);
    $fclose(file);
    $display("injected=%0d cycles=%0d checksum=%08h",injection_cycle,cycles,ap_return); $finish;
  end
endmodule
""")
    subprocess.run(["iverilog","-g2012","-s","tb","-o","sim",str(rtl),"tb.sv"], cwd=folder, check=True)
    official = folder / "official"
    official.mkdir()
    original = subprocess.check_output(["vvp", str(folder/"sim"), "+official"], cwd=official, text=True)
    expected_trace = (official / "fsm.bin").read_bytes()
    if (len(expected_trace) != auth["cycles"] or
            f'cycles={auth["cycles"]} checksum={archive["cases"][key]["checksum"]}' not in original or
            digest((official/"rtl_output.hex").read_text().splitlines()) != auth["arrays"][result_name]["native_sha256"]):
        raise ValueError("Unperturbed RTL replay disagrees with the official array result")
    result = {"rtl_sha256": hash_file(rtl), "choice": choice, "state": state, "first_kernel_block": first,
              "original_cycles": auth["cycles"], "original_fsm_sha256": hash_file(official/"fsm.bin"),
              "output": result_name, "words": depth, "vectors":[]}
    for seed in SEEDS:
        vector = folder / str(seed)
        vector.mkdir()
        rng = random.Random(seed)
        inputs = {}
        for n in operands:
            vals = [rng.randint(-3,3) for _ in range(ram[n])]
            words = [f"{v & 0xffffffff:08x}" for v in vals]
            (vector / (n + ".hex")).write_text("".join(w + "\n" for w in words))
            (vector / (n + ".bin")).write_bytes(struct.pack(f"={len(vals)}i", *vals))
            inputs[n] = digest(words)
        expected = subprocess.check_output([str(folder/"oracle")], cwd=vector, text=True).strip()
        ref = [f"{v[0]:08x}" for v in struct.iter_unpack("=I", (vector/"c_output.bin").read_bytes())]
        trace = subprocess.check_output(["vvp",str(folder/"sim")],cwd=vector,text=True,timeout=180)
        (vector/"simulation.log").write_text(trace)
        inj, last, checksum = re.search(r"injected=(\d+) cycles=(\d+) checksum=(\S+)",trace).groups()
        words = (vector/"rtl_output.hex").read_text().splitlines()
        if not (len(ref)==len(words)==depth) or len(set(ref))<2 or not any(int(w,16) for w in ref):
            raise ValueError(f"Missing or degenerate output for {key}/{seed}")
        bad = [i for i,(x,y) in enumerate(zip(words,ref)) if x!=y]
        result["vectors"].append({"seed":seed,"inputs_sha256":inputs,"injection_cycle":int(inj),
                                  "cycles":int(last),"rtl_checksum":checksum,"native_checksum":expected,
                                  "fsm_trace_equal":(vector/"fsm.bin").read_bytes()==expected_trace,
                                  "fsm_sha256":hash_file(vector/"fsm.bin"),
                                  "native_distinct":len(set(ref)),"native_nonzero":sum(int(w,16)!=0 for w in ref),
                                  "mismatches":len(bad),"first_flat_index_rtl_c":[[i,words[i],ref[i]] for i in bad[:5]],
                                  "native_sha256":digest(ref),"rtl_sha256":digest(words)})
    return name,result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("single","joint","official-arrays","output"):
        p.add_argument("--"+name,type=Path,required=True)
    a=p.parse_args()
    single,joint,out=(x.resolve() for x in (a.single,a.joint,a.output))
    archive=read(a.official_arrays)
    if archive["joint_results_sha256"]!=hash_file(joint/"results.json") or archive["single_results_sha256"]!=hash_file(single/"results.json"):
        raise ValueError("Official measurements belong to different runs")
    out.mkdir(parents=True)
    deployment=read(joint/"results.json")
    with ThreadPoolExecutor(max_workers=4) as pool:
        rows=dict(pool.map(lambda n:case(n,single,joint,archive,deployment,out),CASES))
    (out/"results.json").write_text(json.dumps(rows,indent=2)+"\n")
    for n,r in rows.items():
        print(n, [(v["mismatches"],v["native_nonzero"],v["cycles"]) for v in r["vectors"]])
    if any(v["mismatches"] or v["rtl_checksum"]!=v["native_checksum"] for r in rows.values() for v in r["vectors"]):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
