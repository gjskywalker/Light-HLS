`timescale 1ns/1ps

module pragma_free_array_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done;
  wire ap_idle;
  wire ap_ready;
  wire [5:0] A_address0;
  wire A_ce0;
  wire A_we0;
  wire [31:0] A_d0;
  reg [31:0] A_q0 = 32'd0;

  reg [31:0] A_memory [0:57];
  reg [31:0] expected [0:57];
  integer index;
  integer outer;
  integer inner;
  integer cycles;

  f dut (
    .ap_clk(ap_clk),
    .ap_rst(ap_rst),
    .ap_start(ap_start),
    .ap_done(ap_done),
    .ap_idle(ap_idle),
    .ap_ready(ap_ready),
    .A_address0(A_address0),
    .A_ce0(A_ce0),
    .A_we0(A_we0),
    .A_d0(A_d0),
    .A_q0(A_q0)
  );

  always #5 ap_clk = ~ap_clk;

  // This model implements one-cycle synchronous ap_memory reads.
  always @(posedge ap_clk) begin
    if (A_ce0) begin
      if (A_address0 >= 58) begin
        $display("FAIL: A address out of range: %0d", A_address0);
        $fatal(1);
      end
      if (A_we0)
        A_memory[A_address0] <= A_d0;
      else
        A_q0 <= A_memory[A_address0];
    end
  end

  initial begin
    for (index = 0; index < 58; index = index + 1) begin
      A_memory[index] = (index * 5 + 3) % 29;
      expected[index] = (index * 5 + 3) % 29;
    end
    A_memory[4] = 8;
    expected[4] = 8;

    for (outer = 0; outer < 100; outer = outer + 1) begin
      expected[3] = 6;
      if (expected[4] == 8)
        expected[6] = expected[9] + 3;
      for (inner = 0; inner < 50; inner = inner + 1)
        expected[inner + 8] = expected[inner] + 1;
    end

    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    ap_start = 1'b1;
    @(negedge ap_clk);
    ap_start = 1'b0;

    cycles = 0;
    while (!ap_done && cycles < 100000) begin
      @(negedge ap_clk);
      cycles = cycles + 1;
    end
    if (!ap_done) begin
      $display("FAIL: timeout waiting for ap_done");
      $fatal(1);
    end

    for (index = 0; index < 58; index = index + 1) begin
      if (A_memory[index] !== expected[index]) begin
        $display("FAIL: A[%0d] expected=%0d actual=%0d", index,
                 expected[index], A_memory[index]);
        $fatal(1);
      end
      $display("RESULT A %0d %0d", index, A_memory[index]);
    end
    $display("ALL PRAGMA-FREE ARRAY TESTS PASSED cycles=%0d", cycles);
    $finish;
  end
endmodule
