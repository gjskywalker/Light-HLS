`timescale 1ns/1ps

module extremeLoop_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done;
  wire ap_idle;
  wire ap_ready;

  wire [2:0] A_address0;
  wire A_ce0;
  wire A_we0;
  wire [31:0] A_d0;
  reg [31:0] A_q0 = 32'd0;

  wire [2:0] B_address0;
  wire B_ce0;
  wire B_we0;
  wire [31:0] B_d0;
  reg [31:0] B_q0 = 32'd0;

  reg [31:0] A_memory [0:5];
  reg [31:0] B_memory [0:7];
  integer index;
  integer cycles;

  extremeLoop dut (
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
    .A_q0(A_q0),
    .B_address0(B_address0),
    .B_ce0(B_ce0),
    .B_we0(B_we0),
    .B_d0(B_d0),
    .B_q0(B_q0)
  );

  always #5 ap_clk = ~ap_clk;

  // Each model implements a synchronous one-cycle ap_memory read port.
  always @(posedge ap_clk) begin
    if (A_ce0) begin
      if (A_address0 >= 6) begin
        $display("FAIL: A address out of range: %0d", A_address0);
        $fatal(1);
      end
      if (A_we0)
        A_memory[A_address0] <= A_d0;
      else
        A_q0 <= A_memory[A_address0];
    end

    if (B_ce0) begin
      if (B_address0 >= 8) begin
        $display("FAIL: B address out of range: %0d", B_address0);
        $fatal(1);
      end
      if (B_we0)
        B_memory[B_address0] <= B_d0;
      else
        B_q0 <= B_memory[B_address0];
    end
  end

  initial begin
    A_memory[0] = 32'd10;
    B_memory[0] = 32'd2;
    for (index = 1; index < 6; index = index + 1)
      A_memory[index] = 32'd0;
    for (index = 1; index < 8; index = index + 1)
      B_memory[index] = 32'd0;

    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    ap_start = 1'b1;
    @(negedge ap_clk);
    ap_start = 1'b0;

    cycles = 0;
    while (!ap_done && cycles < 200) begin
      @(negedge ap_clk);
      cycles = cycles + 1;
    end
    if (!ap_done) begin
      $display("FAIL: timeout waiting for ap_done");
      $fatal(1);
    end

    for (index = 0; index < 6; index = index + 1) begin
      if (A_memory[index] !== 10 + index) begin
        $display("FAIL: A[%0d] expected=%0d actual=%0d", index,
                 10 + index, A_memory[index]);
        $fatal(1);
      end
    end
    for (index = 1; index < 8; index = index + 1) begin
      if (B_memory[index] !== B_memory[index - 1] * 13) begin
        $display("FAIL: B[%0d] expected=%0d actual=%0d", index,
                 B_memory[index - 1] * 13, B_memory[index]);
        $fatal(1);
      end
    end

    $display("ALL AP_MEMORY TESTS PASSED cycles=%0d", cycles);
    $finish;
  end
endmodule
