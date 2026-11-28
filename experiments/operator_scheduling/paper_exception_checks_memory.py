"""Compare each primary output array in repaired four-state RTL with the complete C array."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess


CASES = {
    "lu": (40, "A", "int A[40][40]; init_array(40, A); kernel_lu(40, A);", "A_storage"),
    "adi": (20, "u", "int u[20][20], v[20][20], p[20][20], q[20][20]; "
            "init_array(20, u); kernel_adi(20, 20, u, v, p, q);", "u_storage"),
    "nussinov": (60, "table", "int table[60][60]; base seq[60]; "
                 "init_array(60, seq, table); kernel_nussinov(60, seq, table);", "table_v_storage"),
}


def compare(name, inputs, rtl_run, root):
    folder = root / name
    folder.mkdir()
    shape, output_array, initialization, rtl_memory = CASES[name]
    source = inputs / ("vericert24_" + name) / "oracle.c"
    (folder / "oracle.c").write_bytes(source.read_bytes())
    (folder / "dump.c").write_text(
        '#define main original_main\n#include "oracle.c"\n#undef main\n'
        f'int main(void) {{ {initialization}\n'
        f'for(int i=0;i<{shape};i++) for(int j=0;j<{shape};j++) '
        f'printf("%08x\\n", (unsigned){output_array}[i][j]); return 0; }}\n')
    subprocess.run(["cc", "-O0", "dump.c", "-o", "dump"], cwd=folder, check=True)
    reference = subprocess.check_output(["./dump"], cwd=folder)
    rtl = rtl_run / name / "hls/reports/rtl/top.v"
    (folder / "dump.sv").write_text(f"""module memory_dump;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  wire [31:0] ap_return;
  integer cycles, i, file;
  benchmark_main dut(.*);
  always #5 ap_clk = ~ap_clk;
  initial begin
    repeat (2) @(negedge ap_clk); ap_rst = 0; ap_start = 1;
    @(negedge ap_clk); ap_start = 0; cycles = 0;
    while (ap_done !== 1'b1 && cycles < 1000000) begin
      @(negedge ap_clk); cycles = cycles + 1;
    end
    if (ap_done !== 1'b1) $fatal(1, "No completed output");
    file = $fopen("rtl_memory.txt", "w");
    for (i=0; i < {shape*shape}; i=i+1) $fdisplay(file,"%08h",dut.{rtl_memory}[i]);
    $fclose(file); $display("cycles=%0d return=%h",cycles,ap_return); $finish;
  end
endmodule
""")
    subprocess.run(["iverilog", "-g2012", "-s", "memory_dump", "-o", "sim", str(rtl),
                    "dump.sv"], cwd=folder, check=True)
    simulation = subprocess.check_output(["vvp", "sim"], text=True, cwd=folder)
    actual = (folder / "rtl_memory.txt").read_bytes()
    (folder / "native_memory.txt").write_bytes(reference)
    c_words, rtl_words = reference.splitlines(), actual.splitlines()
    if len(c_words) != shape * shape or len(c_words) != len(rtl_words):
        raise ValueError(f"Unexpected memory dimensions in {name}")
    differences = [i for i, word in enumerate(c_words) if word != rtl_words[i]]
    return name, {"array": output_array, "words": len(c_words), "mismatches": len(differences),
                  "first_mismatches": [[i // shape, i % shape, rtl_words[i].decode(), c_words[i].decode()]
                                       for i in differences[:5]], "simulation": simulation.strip(),
                  "native_memory_sha256": hashlib.sha256(reference).hexdigest(),
                  "rtl_memory_sha256": hashlib.sha256(actual).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--existing-run", type=Path, required=True)
    parser.add_argument("--rtl-run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", choices=CASES, nargs="+", default=list(CASES))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True)
    with ThreadPoolExecutor(max_workers=3) as pool:
        jobs = [pool.submit(compare, name, args.existing_run.resolve() / "input",
                            args.rtl_run.resolve(), output) for name in args.cases]
        results = dict(job.result() for job in jobs)
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print({name: row["mismatches"] for name, row in results.items()})
    if any(row["mismatches"] for row in results.values()):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
