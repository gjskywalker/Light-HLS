"""Re-run frozen Light-HLS RTLs in Verilator and Icarus using read-only probes."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DRIVER = r'''
#include <cstdio>
#include <cstdlib>
#include <cinttypes>
#include <array>
#include <string>
#include "Vobserve.h"
#include "verilated_save.h"

void tick(Vobserve& dut) {
  dut.ap_clk = 1; dut.eval();
  dut.ap_clk = 0; dut.eval();
}
void snapshot(Vobserve& dut, uint64_t cycle) {
  VerilatedSave save;
  std::string name = "state_" + std::to_string(cycle) + ".bin";
  save.open(name.c_str()); save << dut; save.close();
}
int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  Vobserve dut;
  dut.dbg_addr = 0;
  dut.ap_rst = 1; dut.ap_start = 0; dut.ap_clk = 0; dut.eval();
  tick(dut); tick(dut);
  dut.ap_rst = 0; dut.ap_start = 1; dut.eval(); tick(dut);
  dut.ap_start = 0; dut.eval();
  const uint64_t limit = strtoull(argv[1], nullptr, 10);
  const int words = atoi(argv[2]);
  uint64_t cycle = 0;
  std::array<uint64_t, 256> counts{};
  while (!dut.ap_done && cycle < limit) {
    ++counts[dut.fsm]; tick(dut); ++cycle;
    if (cycle >= 20000000 && cycle < 20000016) snapshot(dut, cycle);
    if (cycle == 20000000 || cycle == limit || (cycle >= 20000000 && cycle < 20000016))
      printf("POINT %" PRIu64 " fsm=%u done=%u return=%08x addend=%08x limit=%08x acc=%08x updated=%08x\n",
             cycle,dut.fsm,dut.ap_done,dut.ap_return,dut.dbg_a,dut.dbg_b,dut.dbg_acc,dut.dbg_updated);
  }
  printf("RESULT cycles=%" PRIu64 " done=%u return=%08x\n",cycle,dut.ap_done,dut.ap_return);
  for (unsigned i=0;i<counts.size();i++)
    if (counts[i]) printf("STATE %u %" PRIu64 "\n",i,counts[i]);
  if (dut.ap_done && words) {
    FILE* mem = fopen("final_memory.txt", "w");
    for (int i=0;i<words;i++) {
      dut.dbg_addr=i; dut.eval(); fprintf(mem,"%08x %08x\n",dut.dbg_a,dut.dbg_b);
    }
    fclose(mem);
  }
  dut.final(); return 0;
}
'''
ORACLE_DUMP = '''
#define main original_main
#include "oracle.c"
#undef main
int main(void) {
    int table[60][60]; base seq[60];
    init_array(60, seq, table);
    kernel_nussinov(60, seq, table);
    for(int i=0;i<3600;i++) printf("%08x %08x\\n", (unsigned)table[i/60][i%60], (unsigned)seq[i%60]);
    return 0;
}
'''


def run(cmd, folder, logfile, env=None, seconds=180):
    with (folder / logfile).open("w") as f:
        subprocess.run(cmd, cwd=folder, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=seconds, check=True)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def case(name, inputs, verilator_root, target):
    folder = target / name
    folder.mkdir()
    case_id = "vericert24_" + name
    src = inputs / "input" / case_id / "oracle.c"
    native = inputs / "native" / case_id
    rtl = native / "hls/reports/rtl/top.v"
    nussinov = name == "nussinov"
    mem = ("assign dbg_a = impl.table_v_storage[dbg_addr]; "
           "assign dbg_b = impl.seq_storage[dbg_addr % 60]; assign dbg_acc=0; assign dbg_updated=0;" if nussinov else
           "assign dbg_a=impl.v_v182; assign dbg_b=impl.v_v174; assign dbg_acc=impl.v_acc_06_i44_i_i13; "
        "assign dbg_updated=impl.v_add_i47_i_i16;")
    if name == "adi":
        mem = "assign dbg_a=0; assign dbg_b=0; assign dbg_acc=0; assign dbg_updated=0;"
    (folder / "observe.v").write_text(
        "`timescale 1ns/1ps\nmodule observe(input ap_clk, ap_rst, ap_start, input [11:0] dbg_addr, "
        "output ap_done, output [31:0] ap_return, dbg_a, dbg_b, dbg_acc, dbg_updated, output [7:0] fsm);\n"
        "wire ap_idle,ap_ready; benchmark_main impl (.ap_clk(ap_clk),.ap_rst(ap_rst),.ap_start(ap_start),"
        ".ap_done(ap_done),.ap_return(ap_return),.ap_idle(ap_idle),.ap_ready(ap_ready));\n"
        "assign fsm=impl.state;\n" + mem + "\nendmodule\n")
    (folder / "driver.cpp").write_text(DRIVER)
    tools = verilator_root / "usr"
    ver = tools / "bin/verilator_bin"
    env = dict(os.environ, VERILATOR_ROOT=str(tools / "share/verilator"))
    run(["cc", "-O0", str(src), "-o", "oracle"], folder, "build_oracle.log")
    run(["./oracle"], folder, "native.log")
    run([str(ver), "--cc", "--exe", "--savable", "--top-module", "observe",
         "-Wno-fatal", "-Wno-WIDTH", str(folder/"observe.v"), str(rtl), str(folder/"driver.cpp")],
        folder, "translate.log", env)
    run(["make", "-C", "obj_dir", "-j3", "-f", "Vobserve.mk"], folder, "build.log", env)
    limit = 20_000_000 if nussinov else 100_000_000
    run(["./obj_dir/Vobserve", str(limit), "3600" if nussinov else "0"], folder, "verilator.log", env)
    log = (folder / "verilator.log").read_text()
    cycles, done, actual = re.search(r"RESULT cycles=(\d+) done=([01]) return=([0-9a-f]{8})",log).groups()
    previous = native / "result.json"
    original = json.loads(previous.read_text()) if previous.is_file() else {}
    row = {"rtl_sha256": digest(rtl), "oracle_c_sha256": digest(src), "native_hex":(folder/"native.log").read_text().strip(),
           "cycle_limit":limit, "observed_cycles":int(cycles), "ap_done":bool(int(done)), "return_hex":actual,
           "original_icarus": {k:v for k,v in original.items()
                               if k in ("status","diagnostic")}, "state_histogram": dict(re.findall(r"STATE (\d+) (\d+)",log)),
           "snapshots": {p.stem:digest(p) for p in sorted(folder.glob("state_*.bin"))}}
    if name == "lu":
        row["probe_cycles"] = [line for line in log.splitlines() if line.startswith("POINT ")]
    if nussinov and row["ap_done"]:
        (folder/"oracle.c").write_bytes(src.read_bytes())
        (folder/"dump.c").write_text(ORACLE_DUMP)
        run(["cc","-O0","dump.c","-o","dump"],folder,"build_oracle_mem.log")
        run(["./dump"],folder,"native_memory.txt")
        def load(path):
            return [tuple(int(x,16) for x in line.split()) for line in path.read_text().splitlines()]
        dut, ref = load(folder/"final_memory.txt"), load(folder/"native_memory.txt")
        assert len(dut) == len(ref) == 3600
        different = [i for i in range(3600) if dut[i][0] != ref[i][0]]
        row["table_mismatches"]=len(different)
        row["examples_ij_dut_ref"]=[[i//60,i%60,dut[i][0],ref[i][0]] for i in different[:8]]
        row["seq_mismatches"]=sum(dut[i][1]!=ref[i][1] for i in range(60))
        row["table_nonzero_dut_ref"]=[sum(bool(x[0]) for x in data) for data in (dut,ref)]
    probe = {"nussinov": "nussinov_writes", "adi": "adi_completion"}.get(name)
    if probe:
        run(["iverilog", "-g2012", "-s", probe, "-o", "icarus_probe", str(rtl),
             str(HERE / "paper_exception_checks_tb.sv")], folder, "build_icarus_probe.log")
        run(["vvp", "icarus_probe"], folder, "icarus_probe.log")
        row["icarus_probe"] = (folder / "icarus_probe.log").read_text().splitlines()
    return name,row


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--existing-run",type=Path,required=True)
    p.add_argument("--verilator-root",type=Path,required=True)
    p.add_argument("--output",type=Path,required=True)
    p.add_argument("--cases", nargs="+", choices=["lu", "nussinov", "adi"], default=["lu", "nussinov"])
    a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True)
    result={}
    with ThreadPoolExecutor(max_workers=2) as pool:
        futures=[pool.submit(case,n,a.existing_run.resolve(),a.verilator_root.resolve(),out) for n in a.cases]
        for fut in futures:
            name,row=fut.result();result[name]=row
    (out/"results.json").write_text(json.dumps(result,indent=2)+"\n")
    print({n:(r["ap_done"],r["observed_cycles"],r["return_hex"],r["native_hex"]) for n,r in result.items()})


if __name__ == "__main__":
    main()
