`timescale 1ns/1ps
module registered_pressure_tb;
  reg ap_clk = 0;
  integer tick = 0, completed = 0, writes = 0, reads = 0;
  registered_pressure_shell dut(.*);
  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    tick = tick + 1;
    if (!dut.ap_rst) begin
      if (dut.input_r_ce0) begin
        if ((^dut.input_r_address0) === 1'bx) $fatal(1, "Unknown input address");
        reads = reads + 1;
      end
      if (dut.output_r_ce0 && dut.output_r_we0) begin
        if (writes >= 8192 || dut.output_r_address0 !== writes % 256)
          $fatal(1, "Duplicate or reordered output");
        $display("RESULT %0d %0d %0d", writes / 256, writes % 256, dut.output_r_d0);
        writes = writes + 1;
      end
      if (dut.ap_done) begin
        completed = completed + 1;
        if (completed > 32) $fatal(1, "Extra completion");
      end
    end
  end
  initial begin
    while ((completed < 32 || writes < 8192) && tick < 40000) @(negedge ap_clk);
    if (completed != 32 || writes != 8192 || dut.offers != 32)
      $fatal(1, "Shell timed out");
    repeat (8) @(negedge ap_clk);
    if (!dut.ap_idle || dut.ap_done || completed != 32 || writes != 8192 ||
        reads < 8192 || reads > 8224)
      $fatal(1, "Shell drain failed");
    $display("REGISTERED SHELL TESTS PASSED: 32 calls, 8192 outputs, reads=%0d", reads);
    $finish;
  end
endmodule
