`timescale 1ns/1ps

module spam_filter_tb;
  localparam integer NUM_FEATURES = 800;

  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done;
  wire ap_idle;
  wire ap_ready;
  reg [31:0] prob = 32'd3;
  reg [31:0] step = 32'd2;

  wire [9:0] theta_local_address0;
  wire theta_local_ce0;
  wire theta_local_we0;
  wire [31:0] theta_local_d0;
  reg [31:0] theta_local_q0 = 32'd0;
  wire [9:0] training_instance_address0;
  wire training_instance_ce0;
  reg [31:0] training_instance_q0 = 32'd0;
  wire [9:0] gradient_address0;
  wire gradient_ce0;
  wire gradient_we0;
  wire [31:0] gradient_d0;
  reg [31:0] gradient_q0 = 32'd0;

  reg [31:0] theta_local_memory [0:NUM_FEATURES-1];
  reg [31:0] training_instance_memory [0:NUM_FEATURES-1];
  reg [31:0] gradient_memory [0:NUM_FEATURES-1];
  reg [31:0] expected_theta [0:NUM_FEATURES-1];
  reg [31:0] expected_gradient [0:NUM_FEATURES-1];
  integer index;
  integer cycles;

  compute dut (
    .ap_clk(ap_clk),
    .ap_rst(ap_rst),
    .ap_start(ap_start),
    .ap_done(ap_done),
    .ap_idle(ap_idle),
    .ap_ready(ap_ready),
    .theta_local_address0(theta_local_address0),
    .theta_local_ce0(theta_local_ce0),
    .theta_local_we0(theta_local_we0),
    .theta_local_d0(theta_local_d0),
    .theta_local_q0(theta_local_q0),
    .training_instance_address0(training_instance_address0),
    .training_instance_ce0(training_instance_ce0),
    .training_instance_q0(training_instance_q0),
    .gradient_address0(gradient_address0),
    .gradient_ce0(gradient_ce0),
    .gradient_we0(gradient_we0),
    .gradient_d0(gradient_d0),
    .gradient_q0(gradient_q0),
    .prob(prob),
    .step(step)
  );

  always #5 ap_clk = ~ap_clk;

  // Each model implements one-cycle synchronous ap_memory reads.
  always @(posedge ap_clk) begin
    if (theta_local_ce0) begin
      if (theta_local_address0 >= NUM_FEATURES) begin
        $display("FAIL: theta_local address out of range: %0d",
                 theta_local_address0);
        $fatal(1);
      end
      if (theta_local_we0)
        theta_local_memory[theta_local_address0] <= theta_local_d0;
      else
        theta_local_q0 <= theta_local_memory[theta_local_address0];
    end

    if (training_instance_ce0) begin
      if (training_instance_address0 >= NUM_FEATURES) begin
        $display("FAIL: training_instance address out of range: %0d",
                 training_instance_address0);
        $fatal(1);
      end
      training_instance_q0 <=
          training_instance_memory[training_instance_address0];
    end

    if (gradient_ce0) begin
      if (gradient_address0 >= NUM_FEATURES) begin
        $display("FAIL: gradient address out of range: %0d",
                 gradient_address0);
        $fatal(1);
      end
      if (gradient_we0)
        gradient_memory[gradient_address0] <= gradient_d0;
      else
        gradient_q0 <= gradient_memory[gradient_address0];
    end
  end

  initial begin
    for (index = 0; index < NUM_FEATURES; index = index + 1) begin
      theta_local_memory[index] = index % 23;
      training_instance_memory[index] = (index * 3 + 1) % 11;
      gradient_memory[index] = 0;
      expected_gradient[index] = 3 * training_instance_memory[index];
      expected_theta[index] = theta_local_memory[index] +
                              2 * expected_gradient[index];
    end

    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    ap_start = 1'b1;
    @(negedge ap_clk);
    ap_start = 1'b0;

    cycles = 0;
    while (!ap_done && cycles < 20000) begin
      @(negedge ap_clk);
      cycles = cycles + 1;
    end
    if (!ap_done) begin
      $display("FAIL: timeout waiting for ap_done");
      $fatal(1);
    end

    for (index = 0; index < NUM_FEATURES; index = index + 1) begin
      if (gradient_memory[index] !== expected_gradient[index]) begin
        $display("FAIL: gradient[%0d] expected=%0d actual=%0d", index,
                 expected_gradient[index], gradient_memory[index]);
        $fatal(1);
      end
      $display("RESULT gradient %0d %0d", index, gradient_memory[index]);
    end
    for (index = 0; index < NUM_FEATURES; index = index + 1) begin
      if (theta_local_memory[index] !== expected_theta[index]) begin
        $display("FAIL: theta_local[%0d] expected=%0d actual=%0d", index,
                 expected_theta[index], theta_local_memory[index]);
        $fatal(1);
      end
      $display("RESULT theta_local %0d %0d", index,
               theta_local_memory[index]);
    end
    $display("ALL SPAM FILTER TESTS PASSED cycles=%0d", cycles);
    $finish;
  end
endmodule
