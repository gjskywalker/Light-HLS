`timescale 1ns/1ps
`ifndef CASE_KIND
`define CASE_KIND 0
`endif

module pressure_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] coefficient = 0;
  wire [7:0] input_r_address0, output_r_address0;
  wire input_r_ce0, output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] input_r_q0 = 0;
`ifdef PRESSURE_MEMORY_DIRECTIONS
  wire input_r_we0;
  wire [31:0] input_r_d0;
  reg [31:0] output_r_q0 = 0;
`endif
`ifdef PRESSURE_DUAL_PORT
  wire [7:0] input_r_address1, output_r_address1;
  wire input_r_ce1, output_r_ce1, output_r_we1;
  wire [31:0] output_r_d1;
  reg [31:0] input_r_q1 = 0;
`endif
  reg [31:0] source [0:255], result [0:255];
  reg [255:0] seen = 0;
  reg [255:0] seen_reads = 0;
  reg [31:0] expected;
  integer trial, index, writes = 0, reads = 0, cycles, tick = 0;
  integer accepted, first_read, first_write, last_write, observed_done;
  integer start_acknowledged = 0, completed_reads;
  qor_pressure dut (.*);
  always #5 ap_clk = ~ap_clk;

  task commit(input [7:0] address, input [31:0] data);
    begin
      if (seen[address]) $fatal(1, "Duplicate write at %0d", address);
      seen[address] = 1;
      result[address] = data;
      writes = writes + 1;
      if (first_write < 0) first_write = tick;
      last_write = tick;
    end
  endtask

  always @(posedge ap_clk) begin
    tick = tick + 1;
    if (!ap_rst) begin
      if (ap_start && ap_ready) start_acknowledged = 1;
      if (input_r_ce0) begin
        if ((^input_r_address0) === 1'bx) $fatal(1, "Unknown input address");
        seen_reads[input_r_address0] = 1;
        input_r_q0 <= source[input_r_address0];
        reads = reads + 1;
        if (first_read < 0) first_read = tick;
      end
      if (output_r_ce0 && output_r_we0) commit(output_r_address0, output_r_d0);
`ifdef PRESSURE_MEMORY_DIRECTIONS
      if (input_r_ce0 && input_r_we0) $fatal(1, "Unexpected input memory write");
      if (output_r_ce0) output_r_q0 <= result[output_r_address0];
`endif
`ifdef PRESSURE_DUAL_PORT
      if (input_r_ce1) begin
        if ((^input_r_address1) === 1'bx) $fatal(1, "Unknown second input address");
        seen_reads[input_r_address1] = 1;
        input_r_q1 <= source[input_r_address1];
        reads = reads + 1;
        if (first_read < 0) first_read = tick;
      end
      if (output_r_ce1 && output_r_we1) commit(output_r_address1, output_r_d1);
`endif
    end
  end

  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 8; trial = trial + 1) begin
      for (index = 0; index < 256; index = index + 1) begin
        source[index] = (index * 131 + trial * 977) ^ 32'ha531e297;
        result[index] = 32'hdeadbeef;
      end
      seen = 0; seen_reads = 0; writes = 0; reads = 0;
      first_read = -1; first_write = -1; last_write = -1; observed_done = -1;
      start_acknowledged = 0;
      coefficient = trial * 29 + 7;
      accepted = tick + 1;
      ap_start = 1;
      @(negedge ap_clk);
`ifndef PRESSURE_HOLD_START
      ap_start = 0;
`endif
      cycles = 0;
      while ((observed_done < 0 || writes < 256) && cycles < 10000) begin
        // Vitis loop pipelines require start until the ready sampling edge.
        // Completion may follow several cycles later; do not launch again.
        if (start_acknowledged) ap_start = 0;
        if (ap_done && observed_done < 0) observed_done = tick;
        if (observed_done >= 0 && writes == 256) ap_start = 0;
        else begin
          @(negedge ap_clk);
          cycles = cycles + 1;
        end
      end
      ap_start = 0;
      if (writes != 256 || observed_done < 0 || seen !== {256{1'b1}} || seen_reads !== {256{1'b1}} ||
`ifdef PRESSURE_ALLOW_PREFETCH
          // These are ordinary, immutable RAM reads, not FIFO/volatile events.
          // Vitis may repeat one read while the loop exit drains.
          reads < 256 || reads > 257)
`else
          reads != 256)
`endif
        $fatal(1, "Transaction %0d reads=%0d writes=%0d done=%0d", trial, reads, writes, observed_done);
      for (index = 0; index < 256; index = index + 1) begin
        if (`CASE_KIND == 1)
          expected = (source[(index * 73) & 255] ^ coefficient) + index;
        else if (`CASE_KIND == 2)
          expected = (source[index] ^ coefficient) + (source[index ^ 1] ^ coefficient);
        else
          expected = (source[index] & 32'h1ffff) * (coefficient & 32'h1ffff) + index;
        if (result[index] !== expected)
          $fatal(1, "Mismatch trial=%0d index=%0d actual=%h expected=%h", trial, index, result[index], expected);
        $display("RESULT %0d %0d %0d", trial, index, result[index]);
      end
      $display("EVENT trial=%0d accepted=%0d first_read=%0d first_write=%0d last_commit=%0d done=%0d",
               trial, accepted, first_read, first_write, last_write, observed_done);
      $display("TESTS PASSED: transaction=%0d cycles=%0d", trial, observed_done - accepted);
      completed_reads = reads;
      repeat (3) @(negedge ap_clk);
      if (writes != 256 || reads != completed_reads) $fatal(1, "Access after transaction completion");
    end
    $finish;
  end
endmodule
