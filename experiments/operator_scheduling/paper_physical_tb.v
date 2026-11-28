`timescale 1ns/1ps
module physical_tb;
  reg clk = 0, reset = 0, start = 0;
  wire finish;
  wire [31:0] value;
  integer cycles;
  reg [31:0] expected;

`ifdef LIGHT_HLS
  benchmark_main dut(.ap_clk(clk), .ap_rst(reset), .ap_start(start),
                     .ap_done(finish), .ap_return(value), .ap_idle(), .ap_ready());
`else
  main dut(.clk(clk), .reset(reset), .start(start), .finish(finish), .return_val(value));
`endif
  task tick;
    begin
      #5 clk = 1; #5 clk = 0;
    end
  endtask
  initial begin
    if (!$value$plusargs("expected=%h", expected))
      $fatal(1,"expected checksum is required");
    tick;
    reset = 1; tick; reset = 0;
`ifdef LIGHT_HLS
    start = 1; tick; start = 0;
    cycles = 0; // elapsed edges after the accepted ap_start edge
`else
    tick;
    cycles = 1; // author's verilator_main.cpp counts the first released edge
`endif
    while (finish !== 1'b1 && cycles < 5000000) begin
      tick; cycles = cycles + 1;
    end
    if (finish !== 1'b1 || value !== expected)
      $fatal(1,"cycles=%0d done=%b value=%h expected=%h", cycles, finish, value, expected);
    $display("cycles=%0d checksum=%h", cycles, value);
    $finish;
  end
endmodule
