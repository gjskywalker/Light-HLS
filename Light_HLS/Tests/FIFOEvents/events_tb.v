`timescale 1ns/1ps

module events_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] seed;
`ifdef PRIVATE_BUFFER
  wire [4:0] input_r_address0;
  localparam INPUTS = 32;
`else
  wire [3:0] input_r_address0;
  localparam INPUTS = 16;
`endif
  wire input_r_ce0;
  reg [31:0] input_r_q0 = 0;
  wire [3:0] output_r_address0;
  wire output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
`ifdef VITIS_MEMORY_PORTS
  // Vitis exposes unused directions and a second port in its native boundary.
  wire [$bits(input_r_address0)-1:0] input_r_address1;
  wire [3:0] output_r_address1;
  wire input_r_ce1, input_r_we0, input_r_we1, output_r_ce1, output_r_we1;
  wire [31:0] input_r_d0, input_r_d1, output_r_d1;
  reg [31:0] input_r_q1 = 0, output_r_q0 = 0, output_r_q1 = 0;
`endif
  reg [31:0] source [0:INPUTS-1], result [0:15], expected [0:15];
  reg [31:0] random_state = 32'h316849af, sampled_seed, a, b;
  reg [15:0] seen;
  integer trial, index, j, cycles, reads, writes;
  integer ticks = 0, start_tick;
  integer token_writes, token_reads, full_stalls, empty_stalls;
`ifdef PRIVATE_BUFFER
  integer loaded_writes, loaded_reads, local_writes, paused_local_writes, probe;
  reg [31:0] saved [0:31];
`endif
  `DUT_NAME dut (.*);
  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) ticks = ticks + 1;

  always @(posedge ap_clk) begin
`ifdef FIFO_INTERNAL_MONITOR
    if (!ap_rst) begin
`ifdef PRIVATE_BUFFER
      if (dut.channel_loaded_0_write && !dut.channel_loaded_0_full)
        loaded_writes = loaded_writes + 1;
      if (dut.channel_loaded_0_read && !dut.channel_loaded_0_empty)
        loaded_reads = loaded_reads + 1;
      if (dut.channel_computed_0_write && !dut.channel_computed_0_full)
        token_writes = token_writes + 1;
      if (dut.channel_computed_0_read && !dut.channel_computed_0_empty)
        token_reads = token_reads + 1;
      if (dut.private_buffer_1__computed_ce0 && dut.private_buffer_1__computed_we0 &&
          dut.channel_computed_0_full) full_stalls = full_stalls + 1;
      if (dut.private_buffer_1__loaded_ce0 && dut.channel_loaded_0_empty)
        empty_stalls = empty_stalls + 1;
      if (dut.private_buffer_1_instance.buffer_ce0 &&
          dut.private_buffer_1_instance.buffer_we0) begin
        if (dut.private_buffer_1_clock_enable) local_writes = local_writes + 1;
        else paused_local_writes = paused_local_writes + 1;
      end
`else
      if (dut.channel_tokens_0_write && !dut.channel_tokens_0_full)
        token_writes = token_writes + 1;
      if (dut.channel_tokens_0_read && !dut.channel_tokens_0_empty)
        token_reads = token_reads + 1;
      if (!dut.produce_0_clock_enable) full_stalls = full_stalls + 1;
      if (!dut.consume_1_clock_enable) empty_stalls = empty_stalls + 1;
`endif
    end
`endif
    if (!ap_rst && input_r_ce0) begin
      if ((^input_r_address0) === 1'bx) $fatal(1, "Unknown input address");
      input_r_q0 <= source[input_r_address0];
      reads = reads + 1;
    end
`ifdef VITIS_MEMORY_PORTS
    if (!ap_rst && ((input_r_ce0 && input_r_we0) || (input_r_ce1 && input_r_we1)))
      $fatal(1, "Unexpected write to input memory");
    if (!ap_rst && input_r_ce1) begin
      if ((^input_r_address1) === 1'bx) $fatal(1, "Unknown second-port input address");
      input_r_q1 <= source[input_r_address1];
      reads = reads + 1;
    end
    if (!ap_rst && output_r_ce0) output_r_q0 <= result[output_r_address0];
    if (!ap_rst && output_r_ce1) output_r_q1 <= result[output_r_address1];
    if (!ap_rst && output_r_ce1 && output_r_we1) begin
      if ((^output_r_address1) === 1'bx) $fatal(1, "Unknown second-port output address");
      if (seen[output_r_address1]) $fatal(1, "Duplicate second-port output");
      seen[output_r_address1] = 1;
      result[output_r_address1] <= output_r_d1;
      writes = writes + 1;
    end
`endif
    if (!ap_rst && output_r_ce0 && output_r_we0) begin
      if ((^output_r_address0) === 1'bx) $fatal(1, "Unknown output address");
      if (seen[output_r_address0]) $fatal(1, "Duplicate output");
      seen[output_r_address0] = 1;
      result[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
  end

`ifdef FIFO_INTERNAL_MONITOR
`ifdef PRIVATE_BUFFER
  // Natural FIFO backpressure must freeze private RAM together with the task.
  always @(posedge ap_clk) begin
    if (!ap_rst && !dut.private_buffer_1_clock_enable) begin
      for (probe = 0; probe < 32; probe = probe + 1)
        saved[probe] = dut.private_buffer_1_instance.buffer_storage[probe];
      #1;
      for (probe = 0; probe < 32; probe = probe + 1)
        if (saved[probe] !== dut.private_buffer_1_instance.buffer_storage[probe])
          $fatal(1, "Private RAM changed while the FIFO task was stalled");
    end
  end
`endif
`endif

  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 64; trial = trial + 1) begin
      for (index = 0; index < INPUTS; index = index + 1) begin
        random_state = random_state ^ (random_state << 13);
        random_state = random_state ^ (random_state >> 17);
        random_state = random_state ^ (random_state << 5);
        source[index] = random_state;
      end
      sampled_seed = random_state ^ trial;
      seed = sampled_seed;
      for (index = 0; index < 16; index = index + 1) begin
        a = source[index];
`ifdef PRIVATE_BUFFER
        b = source[index+16];
        for (j = 0; j < 4; j = j + 1) begin
          a = a ^ (a << 1);
          b = b ^ (b << 1);
        end
        a = a + b + 2*sampled_seed;
        for (j = 0; j < 8; j = j + 1)
          a = (a << 1) ^ (a >> 1) ^ 32'h9e3779b9;
        expected[index] = a;
`else
        a = a * sampled_seed + 17;
        expected[index] = (a + 32'h12345678) ^ (sampled_seed ^ index);
`endif
        result[index] = 32'hdeadbeef;
      end
      reads = 0;
      writes = 0;
      seen = 0;
      token_writes = 0;
      token_reads = 0;
      full_stalls = 0;
      empty_stalls = 0;
`ifdef PRIVATE_BUFFER
      loaded_writes = 0;
      loaded_reads = 0;
      local_writes = 0;
      paused_local_writes = 0;
`endif
      ap_start = 1;
      start_tick = ticks;
`ifdef FAIR_COMPARISON
      // Hold the request until ap_ready acknowledges it on a rising edge.
      // DATAFLOW tools may acknowledge later than they start their first task.
      @(posedge ap_clk);
      while (ap_ready !== 1'b1 && ticks - start_tick < 20000)
        @(posedge ap_clk);
      if (ap_ready !== 1'b1) $fatal(1, "Start handshake timeout");
`endif
      @(negedge ap_clk);
      ap_start = 0;
`ifndef FAIR_COMPARISON
      seed = ~sampled_seed;
`endif
      cycles = ticks - start_tick;
      while (!ap_done && cycles < 20000) begin
        @(negedge ap_clk);
        cycles = ticks - start_tick;
      end
      // Ordinary input RAM reads are idempotent; Vitis may prefetch/repeat one.
      // FIFO token counts remain exact in the independent internal monitor.
      if (!ap_done || writes != 16 || seen !== 16'hffff ||
`ifdef FAIR_COMPARISON
          reads < INPUTS)
`else
          reads != INPUTS)
`endif
        $fatal(1, "Timeout/request count trial=%0d reads=%0d writes=%0d", trial, reads, writes);
      for (index = 0; index < 16; index = index + 1)
        if (result[index] !== expected[index])
          $fatal(1, "Mismatch trial=%0d index=%0d expected=%h actual=%h", trial, index, expected[index], result[index]);
`ifdef FIFO_INTERNAL_MONITOR
`ifdef PRIVATE_BUFFER
      if (loaded_writes != 32 || loaded_reads != 32 ||
          token_writes != 16 || token_reads != 16 || local_writes != 32 ||
          full_stalls == 0 || empty_stalls == 0 || paused_local_writes == 0)
        $fatal(1, "Missing FIFO/stall coverage: loaded=%0d/%0d computed=%0d/%0d local=%0d paused=%0d full=%0d empty=%0d",
               loaded_writes, loaded_reads, token_writes, token_reads,
               local_writes, paused_local_writes, full_stalls, empty_stalls);
`else
      if (token_writes != 64 || token_reads != 64 || empty_stalls == 0)
        $fatal(1, "Token count/stall mismatch: write=%0d read=%0d", token_writes, token_reads);
`endif
`endif
`ifdef FAIR_COMPARISON
      for (index = 0; index < 16; index = index + 1)
        $display("RESULT output %0d %0d %0d", trial, index, result[index]);
      $display("TRANSACTION trial=%0d cycles=%0d", trial, cycles);
`endif
      #1;
      repeat (3) @(negedge ap_clk);
    end
    $display("FIFO EVENT TESTS PASSED: 64 calls, 1024 outputs");
    $finish;
  end
endmodule
