`timescale 1ns/1ps

module local_write_data_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] seed;
  wire [4:0] input_r_address0;
  wire input_r_ce0;
  reg [31:0] input_r_q0 = 0;
  wire [3:0] output_r_address0;
  wire output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] source [0:31], result [0:15], expected [0:15];
  reg [31:0] random_state = 32'h7fb93736, sampled_seed, a, b;
  reg [15:0] seen;
  integer trial, index, j, cycles, reads, writes;
  `DUT_NAME dut (.*);
  always #5 ap_clk = ~ap_clk;
`ifdef DATAFLOW_PRIVATE
  integer pause_ticks = 0, paused_writes, active_writes, probe_index;
  reg paused = 0;
  reg [31:0] saved_memory [0:31];
  // Inject legal task-clock stalls at the wrapper's shared enable so that
  // external requests, FIFO/PIPO events, registers, and local RAM all freeze.
  always @(negedge ap_clk) begin
    pause_ticks = pause_ticks + 1;
    if (!ap_rst && !dut.private_buffer_0_idle &&
        pause_ticks % 11 >= 3 && pause_ticks % 11 <= 5) begin
      force dut.private_buffer_0_clock_enable = 1'b0;
      paused = 1;
    end else begin
      release dut.private_buffer_0_clock_enable;
      paused = 0;
    end
  end
  always @(posedge ap_clk) begin
    if (!ap_rst && dut.private_buffer_0_instance.buffer_ce0 &&
        dut.private_buffer_0_instance.buffer_we0) begin
      if (paused) paused_writes = paused_writes + 1;
      else if (dut.private_buffer_0_clock_enable) active_writes = active_writes + 1;
    end
    if (paused) begin
      for (probe_index = 0; probe_index < 32; probe_index = probe_index + 1)
        saved_memory[probe_index] = dut.private_buffer_0_instance.buffer_storage[probe_index];
      #1;
      for (probe_index = 0; probe_index < 32; probe_index = probe_index + 1)
        if (saved_memory[probe_index] !== dut.private_buffer_0_instance.buffer_storage[probe_index])
          $fatal(1, "Local RAM changed while the task clock was stalled");
    end
  end
`endif
  always @(posedge ap_clk) begin
    if (!ap_rst && input_r_ce0) begin
      if ((^input_r_address0) === 1'bx) $fatal(1, "Unknown read address");
      input_r_q0 <= source[input_r_address0];
      reads = reads + 1;
    end
    if (!ap_rst && output_r_ce0 && output_r_we0) begin
      if ((^output_r_address0) === 1'bx) $fatal(1, "Unknown write address");
      if (seen[output_r_address0]) $fatal(1, "Duplicate output write");
      seen[output_r_address0] = 1;
      result[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
  end
  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 64; trial = trial + 1) begin
      for (index = 0; index < 32; index = index + 1) begin
        random_state = random_state ^ (random_state << 13);
        random_state = random_state ^ (random_state >> 17);
        random_state = random_state ^ (random_state << 5);
        source[index] = random_state;
      end
      sampled_seed = random_state ^ trial;
      seed = sampled_seed;
      for (index = 0; index < 16; index = index + 1) begin
        result[index] = 32'hdeadbeef;
        a = source[index];
        b = source[index+16];
        case (`CASE_KIND)
          0: expected[index] = a + b + 2*sampled_seed;
          1: expected[index] = (a + sampled_seed) + (a ^ sampled_seed);
          2: begin
            a = a + sampled_seed;
            if (index % 4 == 0) a = a + 7;
            expected[index] = a ^ (sampled_seed + 1);
          end
          3: expected[index] = a + sampled_seed;
          4: begin
            expected[index] = a + b + 2*sampled_seed;
          end
        endcase
      end
      reads = 0;
      writes = 0;
      seen = 0;
`ifdef DATAFLOW_PRIVATE
      paused_writes = 0;
      active_writes = 0;
`endif
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      seed = ~sampled_seed;
      cycles = 0;
      while (!ap_done && cycles < 10000) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (!ap_done || writes != 16 || reads != ((`CASE_KIND == 0 || `CASE_KIND == 4) ? 32 : 16))
        $fatal(1, "Timeout or request mismatch trial=%0d reads=%0d writes=%0d", trial, reads, writes);
`ifdef DATAFLOW_PRIVATE
      if (paused_writes == 0 || active_writes != 32)
        $fatal(1, "Missing stalled-write coverage or incorrect committed local writes");
`endif
      for (index = 0; index < 16; index = index + 1)
        if (result[index] !== expected[index])
          $fatal(1, "Mismatch trial=%0d index=%0d expected=%h actual=%h", trial, index, expected[index], result[index]);
      @(negedge ap_clk);
    end
    $display("LOCAL WRITE DATA TESTS PASSED: 64 calls, 1024 outputs");
    $finish;
  end
endmodule
