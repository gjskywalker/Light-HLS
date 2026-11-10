`timescale 1ns/1ps

module recurrence_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] seed, count;
  wire [3:0] input_r_address0, output_r_address0;
  wire input_r_ce0, output_r_ce0, output_r_we0;
  reg [31:0] input_r_q0 = 0, delayed_read = 0;
  wire [31:0] output_r_d0;
  reg [31:0] source [0:15], result [0:15], expected [0:15];
  reg [31:0] random_state = 32'h34a087cd, sampled_seed, x, y, next_x, previous;
  reg [15:0] seen;
  integer trial, index, j, trips, reads, writes, cycles, total_cycles = 0;
  recurrence dut (.*);
  always #5 ap_clk = ~ap_clk;

  always @(posedge ap_clk) begin
    if (!ap_rst && input_r_ce0) begin
      if ((^input_r_address0) === 1'bx) $fatal(1, "Unknown read address");
      if (`READ_LATENCY == 2) delayed_read <= source[input_r_address0];
      else input_r_q0 <= source[input_r_address0];
      reads = reads + 1;
    end
    if (`READ_LATENCY == 2) input_r_q0 <= delayed_read;
    if (!ap_rst && output_r_ce0 && output_r_we0) begin
      if ((^output_r_address0) === 1'bx || seen[output_r_address0])
        $fatal(1, "Unknown or duplicate write address");
      seen[output_r_address0] = 1;
      result[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
  end

  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 64; trial = trial + 1) begin
      for (index = 0; index < 16; index = index + 1) begin
        random_state = random_state ^ (random_state << 13);
        random_state = random_state ^ (random_state >> 17);
        random_state = random_state ^ (random_state << 5);
        source[index] = random_state;
      end
      sampled_seed = random_state;
      trips = `CASE_KIND == 1 ? trial % 8 + 1 : 8;
      for (index = 0; index < 16; index = index + 1) begin
        x = source[index] ^ sampled_seed;
        previous = x;
        y = sampled_seed + index;
        for (j = 0; j < trips; j = j + 1) begin
          case (`CASE_KIND)
            2: begin
              next_x = (x << 1) ^ y;
              y = (y >> 1) + x;
              x = next_x;
            end
            3: x = (x << 1) ^ source[j];
            4: begin
              x = (x << 1) ^ (x >> 1) ^ 32'h9e3779b9;
              y = y + source[j];
            end
            6: x = ((x << 1) ^ (x >> 1) ^ 32'h9e3779b9) & 255;
            7: x = x / (sampled_seed | 1);
            8: begin
              previous = x;
              x = (x << 1) ^ (x >> 1) ^ 32'h9e3779b9;
            end
            default: x = (x << 1) ^ (x >> 1) ^ 32'h9e3779b9;
          endcase
        end
        expected[index] = x ^ (`CASE_KIND == 8 ? previous : y);
        result[index] = 32'hdeadbeef;
      end
      seed = sampled_seed;
      count = trial;
      reads = 0;
      writes = 0;
      seen = 0;
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      seed = ~sampled_seed;
      count = 0;
      cycles = 0;
      while (!ap_done && cycles < 10000) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (!ap_done || writes != 16 || reads != ((`CASE_KIND == 3 || `CASE_KIND == 4) ? 144 : 16))
        $fatal(1, "Control mismatch trial=%0d reads=%0d writes=%0d cycles=%0d", trial, reads, writes, cycles);
      for (index = 0; index < 16; index = index + 1)
        if (result[index] !== expected[index])
          $fatal(1, "Output mismatch trial=%0d index=%0d expected=%h actual=%h", trial, index, expected[index], result[index]);
      total_cycles = total_cycles + cycles;
      repeat (3) @(negedge ap_clk);
    end
    $display("BACKEDGE TESTS PASSED: 64 calls, 1024 outputs, total_cycles=%0d", total_cycles);
    $finish;
  end
endmodule
