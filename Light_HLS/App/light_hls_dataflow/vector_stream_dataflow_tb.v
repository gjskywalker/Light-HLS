`timescale 1ns/1ps

`ifndef DATAFLOW_DUT
`define DATAFLOW_DUT vector_stream_dataflow
`endif

module vector_stream_dataflow_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done, ap_idle, ap_ready;
  wire [5:0] input_r_address0;
  wire input_r_ce0;
  reg [31:0] input_r_q0 = 0;
  wire [5:0] output_r_address0;
  wire output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
`ifdef DATAFLOW_VITIS_DUAL_PORT
  wire [5:0] input_r_address1, output_r_address1;
  wire input_r_ce1, output_r_ce1, output_r_we1;
  wire [31:0] output_r_d1;
  reg [31:0] input_r_q1 = 0;
`endif
  reg [31:0] input_memory [0:63];
  reg [31:0] output_memory [0:63];
  integer index, cycles, writes;

  `DATAFLOW_DUT dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .input_r_address0(input_r_address0), .input_r_ce0(input_r_ce0),
    .input_r_q0(input_r_q0),
    .output_r_address0(output_r_address0), .output_r_ce0(output_r_ce0),
    .output_r_we0(output_r_we0), .output_r_d0(output_r_d0)
`ifdef DATAFLOW_VITIS_DUAL_PORT
    , .input_r_address1(input_r_address1), .input_r_ce1(input_r_ce1),
    .input_r_q1(input_r_q1), .output_r_address1(output_r_address1),
    .output_r_ce1(output_r_ce1), .output_r_we1(output_r_we1),
    .output_r_d1(output_r_d1)
`endif
  );

  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (input_r_ce0) input_r_q0 <= input_memory[input_r_address0];
`ifdef DATAFLOW_VITIS_DUAL_PORT
    if (input_r_ce1) input_r_q1 <= input_memory[input_r_address1];
`endif
    if (output_r_ce0 && output_r_we0) begin
      output_memory[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
`ifdef DATAFLOW_VITIS_DUAL_PORT
    if (output_r_ce1 && output_r_we1) begin
      output_memory[output_r_address1] <= output_r_d1;
      writes = writes + 1;
    end
`endif
  end

`ifdef DATAFLOW_VITIS_FIFO_INIT_WORKAROUND
  // Nudge Vitis FIFO status signals during reset so Icarus evaluates the
  // generated time-zero combinational block conditions deterministically.
  initial begin
    #1;
    force dut.loaded_full_n = 1'b0;
    force dut.loaded_empty_n = 1'b1;
    force dut.computed_full_n = 1'b0;
    force dut.computed_empty_n = 1'b1;
    force dut.start_for_compute_stage_U0_full_n = 1'b0;
    force dut.start_for_compute_stage_U0_empty_n = 1'b1;
    force dut.start_for_store_stage_U0_full_n = 1'b0;
    force dut.start_for_store_stage_U0_empty_n = 1'b1;
    force dut.load_stage_U0.ap_block_state2_pp0_stage0_iter1 = 1'b0;
    #1;
    release dut.loaded_full_n;
    release dut.loaded_empty_n;
    release dut.computed_full_n;
    release dut.computed_empty_n;
    release dut.start_for_compute_stage_U0_full_n;
    release dut.start_for_compute_stage_U0_empty_n;
    release dut.start_for_store_stage_U0_full_n;
    release dut.start_for_store_stage_U0_empty_n;
    release dut.load_stage_U0.ap_block_state2_pp0_stage0_iter1;
  end
`endif

  initial begin
    for (index = 0; index < 64; index = index + 1) begin
      input_memory[index] = index * 7 + 3;
      output_memory[index] = 32'hdeadbeef;
    end
    cycles = 0;
    writes = 0;
    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    ap_start = 1'b1;
    begin : wait_for_acceptance
      @(posedge ap_clk);
      #1;
      cycles = cycles + 1;
      while (!ap_ready && cycles < 3000) begin
        @(posedge ap_clk);
        #1;
        cycles = cycles + 1;
      end
    end
    if (!ap_ready) $fatal(1, "FAIL: DATAFLOW input handshake timeout");
`ifndef DATAFLOW_VITIS_HOLD_START_UNTIL_DONE
    @(negedge ap_clk);
    ap_start = 1'b0;
`endif
    while (!ap_done && cycles < 3000) begin
      @(negedge ap_clk);
      cycles = cycles + 1;
    end
    if (!ap_done) $fatal(1, "FAIL: DATAFLOW timeout");
`ifdef DATAFLOW_VITIS_HOLD_START_UNTIL_DONE
    ap_start = 1'b0;
`endif
    @(posedge ap_clk);
    #1;
    if (writes != 64) $fatal(1, "FAIL: DATAFLOW writes=%0d", writes);
    for (index = 0; index < 64; index = index + 1) begin
      if (output_memory[index] !== input_memory[index] * 3 + 1)
        $fatal(1, "FAIL: DATAFLOW output[%0d]=%0d", index,
               output_memory[index]);
      $display("RESULT output %0d %0d", index, output_memory[index]);
    end
    $display("TESTS PASSED: stream DATAFLOW cycles=%0d", cycles);
    $finish;
  end
endmodule
