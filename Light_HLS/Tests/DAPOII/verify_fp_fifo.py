#!/usr/bin/env python3
"""Check floating FIFO task stalls, repeated transactions, and reset abort."""
import argparse
import ctypes
import json
from pathlib import Path
import re
import struct
from verify_floating import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    template = (Path(__file__).resolve().parents[2] / "App/backend_floating/backend_fp_fifo.cpp").read_text()
    libm = ctypes.CDLL("libm.so.6")
    libm.sqrtf.argtypes = [ctypes.c_float]
    libm.sqrtf.restype = ctypes.c_float
    libm.sqrt.argtypes = [ctypes.c_double]
    libm.sqrt.restype = ctypes.c_double
    results = []
    for width, kind, fmt in [(16, "_Float16", "e"), (32, "float", "f"), (64, "double", "d")]:
        directory = output / str(width)
        directory.mkdir()
        source = directory / "fifo.cpp"
        text = template.replace("float", kind).replace("bitwidth=32", f"bitwidth={width}")
        if width == 64: text = text.replace("__builtin_sqrtf", "__builtin_sqrt")
        source.write_text(text)
        run([args.binary.resolve(), source, "dut", "--board", "u250", "--freq", "100MHz", "--output", directory / "generated"], directory, "generate")
        rtl = directory / "generated/reports/rtl/top.v"
        cores = re.findall(r"divSqrtRecFN_small #\([^)]*\) (\w+) ", rtl.read_text())
        if not cores: raise RuntimeError("No iterative operators in FIFO stress design")
        busy = " || ".join(f"(dut_i.consumer_1_instance.{core}.divSqrtRecFNToRaw.cycleNum > 1)" for core in cores)
        fraction = {16: 10, 32: 23, 64: 52}[width]
        exponent = ((1 << (width-fraction-1))-1) << fraction
        values = [0, 1, (1 << fraction)-1, 1 << fraction, exponent-1, exponent,
                  exponent+1, 1 << (width-1), (1 << (width-1)) | exponent]
        values += [int.from_bytes(struct.pack("<"+fmt, x), "little") for x in (1, 2, 3, 4, 9, -1, 0.25)]
        initializers = []
        for index, bits in enumerate(values):
            value = struct.unpack("<"+fmt, bits.to_bytes(width//8, "little"))[0]
            root = libm.sqrt(value) if width == 64 else libm.sqrtf(value)
            answer = root / 3.0
            if width != 64: answer = ctypes.c_float(answer).value
            answer += 0.25
            if width != 64: answer = ctypes.c_float(answer).value
            expected = int.from_bytes(struct.pack("<"+fmt, answer), "little")
            initializers += [f"input_memory[{index}]={width}'h{bits:x};", f"expected[{index}]={width}'h{expected:x};"]
        tb = directory / "tb.v"
        tb.write_text(f'''module tb;
reg ap_clk=0, ap_rst=1, ap_start=0;
always #5 ap_clk=~ap_clk;
wire ap_done,ap_idle,ap_ready,input_stream_read,output_stream_write;
wire [{width-1}:0] input_stream_dout,output_stream_din;
wire input_stream_empty_n;
reg output_stream_full_n=0;
reg [{width-1}:0] input_memory[0:15],expected[0:15];
integer input_index=0,output_index=0,ticks=0,trial=0,cycles=0,stalls=0,arithmetic_stalls=0;
reg checking=0;
assign input_stream_empty_n=!ap_rst && input_index<16 && ((ticks%23)>7);
assign input_stream_dout=input_index<16 ? input_memory[(input_index+trial*5)%16] : 0;
dut dut_i(.ap_clk(ap_clk),.ap_rst(ap_rst),.ap_start(ap_start),.ap_done(ap_done),
 .ap_idle(ap_idle),.ap_ready(ap_ready),.input_stream_dout(input_stream_dout),
 .input_stream_empty_n(input_stream_empty_n),.input_stream_read(input_stream_read),
 .output_stream_din(output_stream_din),.output_stream_full_n(output_stream_full_n),
 .output_stream_write(output_stream_write));
function equal_fp;
input [{width-1}:0] a,b;
begin
 equal_fp=(a===b) || ((^a!==1'bx) && (^b!==1'bx) &&
  ((a&{width}'h{exponent:x})=={width}'h{exponent:x}) &&
  ((b&{width}'h{exponent:x})=={width}'h{exponent:x}) &&
  (|(a&{width}'h{(1<<fraction)-1:x})) && (|(b&{width}'h{(1<<fraction)-1:x})));
end
endfunction
always @(negedge ap_clk) output_stream_full_n = !ap_rst && (((ticks+trial*13)%101)>75);
always @(posedge ap_clk) begin
 ticks=ticks+1;
 if (!ap_rst) begin
  if (!output_stream_full_n) stalls=stalls+1;
  if (!dut_i.consumer_1_clock_enable && ({busy})) arithmetic_stalls=arithmetic_stalls+1;
  if (input_stream_read && input_stream_empty_n) input_index<=input_index+1;
  if (output_stream_write && output_stream_full_n) begin
   if (checking && (output_index>=16 || !equal_fp(output_stream_din,expected[(output_index+trial*5)%16])))
    $fatal(1,"MISMATCH trial=%0d output=%0d actual=%h expected=%h",trial,output_index,output_stream_din,expected[(output_index+trial*5)%16]);
   output_index=output_index+1;
  end
 end
end
task start_transaction;
begin
 @(negedge ap_clk); ap_start=1;
 @(negedge ap_clk); ap_start=0;
end
endtask
initial begin
{chr(10).join(initializers)}
 repeat(3) @(negedge ap_clk); ap_rst=0;
 start_transaction;
 repeat(45) @(negedge ap_clk);
 ap_rst=1;
 repeat(3) @(negedge ap_clk);
 input_index=0; output_index=0; ap_rst=0; checking=1;
 for (trial=0;trial<4;trial=trial+1) begin
  input_index=0; output_index=0; cycles=0;
  start_transaction;
  while(!ap_done && cycles<20000) begin @(negedge ap_clk); cycles=cycles+1; end
  if (!ap_done || input_index!=16 || output_index!=16)
   $fatal(1,"COUNTS input=%0d output=%0d",input_index,output_index);
  $display("PASS trial=%0d cycles=%0d",trial,cycles);
  @(negedge ap_clk);
 end
 if(stalls==0) $fatal(1,"NO BACKPRESSURE");
 if(arithmetic_stalls==0) $fatal(1,"NO ACTIVE ARITHMETIC STALL");
 $display("FIFO VERIFICATION PASSED stalls=%0d arithmetic_stalls=%0d",stalls,arithmetic_stalls);
 $finish;
end
endmodule
''')
        run(["iverilog", "-g2012", "-s", "tb", "-o", directory / "sim", tb, directory / "generated/reports/rtl/top.v"], directory, "iverilog")
        log = run(["vvp", directory / "sim"], directory, "simulation")
        passed = "FIFO VERIFICATION PASSED" in log and log.count("PASS trial=") == 4
        results.append(dict(width=width, passed=passed, trials=4, reset_abort=True, output=log))
        (output / "results.json").write_text(json.dumps(results, indent=2)+"\n")
        print(width, "passed" if passed else "FAILED", flush=True)
        if not passed: raise SystemExit(1)


if __name__ == "__main__": main()
