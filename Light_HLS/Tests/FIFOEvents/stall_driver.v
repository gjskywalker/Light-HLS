`timescale 1ns/1ps

// Add deterministic clock-enable pauses without changing the common QoR oracle.
module fifo_stall_driver;
  events_tb bench();
  always @(negedge bench.ap_clk) begin
`ifdef PRIVATE_BUFFER
    if (!bench.ap_rst && !bench.dut.store_slow_2_idle && bench.ticks % 13 < 4)
      force bench.dut.store_slow_2_clock_enable = 1'b0;
    else
      release bench.dut.store_slow_2_clock_enable;
`else
    if (!bench.ap_rst && !bench.dut.consume_1_idle && bench.ticks % 13 < 4)
      force bench.dut.consume_1_clock_enable = 1'b0;
    else
      release bench.dut.consume_1_clock_enable;
`endif
  end
endmodule
