`timescale 1ns/1ps
module pipo_bank_checker #(parameter DEPTH = 3, ADDRESS_WIDTH = 2)(output reg done = 0);
  reg clk = 0, write_bank = 0, read_bank = 1, write_enable = 0, read_enable = 0;
  reg [ADDRESS_WIDTH-1:0] write_address = 0, read_address = 0;
  reg [31:0] data_in = 0;
  wire [31:0] data_out, peek_data;
  reg [31:0] model [0:2*DEPTH-1];
  reg [31:0] expected_q, random_state = 32'hfe1357bd;
  reg response_valid = 0;
  integer step, address;
  light_hls_pipo_bank #(.WIDTH(32), .DEPTH(DEPTH), .ADDRESS_WIDTH(ADDRESS_WIDTH), .PEEK_ENABLED(DEPTH == 1)) dut (.*);
  always #5 clk = ~clk;
  initial begin
    // Initialize both ownership banks without relying on a payload reset.
    for (address = 0; address < 2*DEPTH; address = address + 1) begin
      @(negedge clk);
      write_enable = 1;
      write_bank = address >= DEPTH;
      write_address = address % DEPTH;
      data_in = address ^ 32'h12345678;
      model[address] = data_in;
      @(posedge clk);
      #1;
    end
    for (step = 0; step < 2048; step = step + 1) begin
      @(negedge clk);
      random_state = random_state * 32'd1664525 + 32'd1013904223;
      write_bank = random_state[28];
      read_bank = random_state[29];
      write_address = (random_state >> 3) % DEPTH;
      read_address = (random_state >> 15) % DEPTH;
      write_enable = random_state[31] | random_state[25];
      read_enable = random_state[30];
      data_in = random_state ^ (step * 32'h32856971);
      if (read_enable) begin
        expected_q = model[(read_bank ? DEPTH : 0) + read_address];
        response_valid = 1;
      end
      @(posedge clk);
      if (write_enable) model[(write_bank ? DEPTH : 0) + write_address] = data_in;
      #1;
      if (response_valid && data_out !== expected_q)
        $fatal(1, "PIPO response mismatch depth=%0d step=%0d actual=%h expected=%h", DEPTH, step, data_out, expected_q);
      if (DEPTH == 1 && peek_data !== model[read_bank ? DEPTH : 0])
        $fatal(1, "PIPO scalar peek mismatch");
    end
    done = 1;
  end
endmodule

module pipo_banks_tb;
  wire a, b, c, d;
  pipo_bank_checker #(1, 1) depth1(a);
  pipo_bank_checker #(3, 2) depth3(b);
  pipo_bank_checker #(16, 4) depth16(c);
  pipo_bank_checker #(64, 6) depth64(d);
  initial begin
    wait (a && b && c && d);
    $display("PIPO BANK TESTS PASSED: 8192 request/hold cycles");
    $finish;
  end
  initial begin
    #1000000;
    $fatal(1, "PIPO bank test timeout");
  end
endmodule
