`timescale 1ns/1ps

module backend_local_memory_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  reg [31:0] seed = 32'd0;
  reg [31:0] selector = 32'd0;
  wire ap_done;
  wire ap_idle;
  wire ap_ready;
  wire [31:0] ap_return;

  backend_local_memory dut (
    .ap_clk(ap_clk),
    .ap_rst(ap_rst),
    .ap_start(ap_start),
    .ap_done(ap_done),
    .ap_idle(ap_idle),
    .ap_ready(ap_ready),
    .seed(seed),
    .selector(selector),
    .ap_return(ap_return)
  );

  always #5 ap_clk = ~ap_clk;

  function automatic [31:0] reference_result;
    input [31:0] input_seed;
    input [31:0] input_selector;
    reg [31:0] first_index;
    reg [31:0] second_index;
    begin
      first_index = input_selector & 7;
      second_index = (input_selector + 3) & 7;
      reference_result = input_seed + first_index * 3 +
                         input_seed + second_index * 3;
    end
  endfunction

  task automatic run_case;
    input [31:0] input_seed;
    input [31:0] input_selector;
    reg [31:0] expected;
    integer cycles;
    begin
      expected = reference_result(input_seed, input_selector);
      @(negedge ap_clk);
      seed = input_seed;
      selector = input_selector;
      ap_start = 1'b1;
      @(negedge ap_clk);
      ap_start = 1'b0;

      cycles = 0;
      while (!ap_done && cycles < 300) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (!ap_done) begin
        $display("FAIL: timeout waiting for ap_done");
        $fatal(1);
      end
      if (ap_return !== expected) begin
        $display("FAIL: seed=%0d selector=%0d expected=%0d actual=%0d",
                 input_seed, input_selector, expected, ap_return);
        $fatal(1);
      end
      $display("PASS: local RAM seed=%0d selector=%0d result=%0d cycles=%0d",
               input_seed, input_selector, ap_return, cycles);
    end
  endtask

  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    run_case(32'd10, 32'd1);
    run_case(32'd25, 32'd6);
    $display("ALL LOCAL RAM TESTS PASSED");
    $finish;
  end
endmodule
