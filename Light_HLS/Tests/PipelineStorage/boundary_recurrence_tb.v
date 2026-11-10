`timescale 1ns/1ps

module boundary_recurrence_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] bias = 0, seed = 0;
  wire [31:0] ap_return;
  wire [4:0] input_r_address0;
  wire input_r_ce0;
  reg [31:0] input_r_q0 = 0, source [0:31];
  reg [31:0] expected, saved_bias, saved_seed, previous_return = 0;
  reg [31:0] random_state = 32'hd172e912;
  integer trial, index, cycles, reads;
`ifdef ADJACENT_SEED
  localparam EXPECTED_READS = 34;
`elsif MEMORY_SEED
  localparam EXPECTED_READS = 18;
`else
  localparam EXPECTED_READS = 17;
`endif
  `DUT_NAME dut (.*);
  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (input_r_ce0) begin
      input_r_q0 <= source[input_r_address0];
      reads = reads + 1;
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
      saved_bias = random_state;
`ifdef CONSTANT_SEED
      saved_seed = 7;
`elsif MEMORY_SEED
      saved_seed = source[31];
`else
      saved_seed = random_state ^ 32'hfed314a2;
`endif
      expected = saved_seed;
      for (index = 0; index < 17; index = index + 1)
        expected = expected + source[index] + saved_bias;
`ifdef ADJACENT_SEED
      for (index = 0; index < 17; index = index + 1)
        expected = expected + (source[index] ^ saved_bias);
`endif
      bias = saved_bias;
      seed = saved_seed;
      reads = 0;
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      // Arguments are sampled at acceptance, not on a later pipeline edge.
      bias = ~saved_bias;
      seed = ~saved_seed;
      cycles = 0;
      while (!ap_done && cycles < 256) begin
        if (ap_return !== previous_return)
          $fatal(1, "Return changed before done at trial=%0d", trial);
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (!ap_done || reads != EXPECTED_READS || ap_return !== expected)
        $fatal(1, "Boundary trial=%0d reads=%0d expected=%h actual=%h cycles=%0d",
               trial, reads, expected, ap_return, cycles);
      previous_return = ap_return;
      @(negedge ap_clk);
    end
    $display("ALL BOUNDARY RECURRENCE TESTS PASSED (64 returns, %0d reads)",
             64 * EXPECTED_READS);
    $finish;
  end
endmodule
