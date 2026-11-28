module nussinov_writes;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  wire [31:0] ap_return;
  benchmark_main dut(.*);
  always #5 ap_clk = ~ap_clk;
  initial begin
    repeat (2) @(negedge ap_clk);
    ap_rst = 0;
    ap_start = 1;
    @(negedge ap_clk);
    ap_start = 0;
    repeat (170000) @(negedge ap_clk);
    $fatal(1, "Selected cell not observed");
  end
  always @(posedge ap_clk) begin
    if (dut.v_HI_new17 == 55 && dut.v_indvars_iv348_i == 58) begin
      if (dut.state == 27 || dut.state == 31)
        $display("write state=%0d addr=%0d data=%0d prediag=%0d diagmax=%0d loopmax=%0d",
                 dut.state, dut.table_v_address0, dut.table_v_d0, dut.v_v80,
                 dut.v_v98, dut.v_add166_341_i);
      if (dut.state == 32) $finish;
    end
  end
endmodule

module adi_completion;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  wire [31:0] ap_return;
  benchmark_main dut(.*);
  always #5 ap_clk = ~ap_clk;
  integer cycle = 0;
  initial begin
    repeat (2) @(negedge ap_clk);
    ap_rst = 0;
    ap_start = 1;
    @(negedge ap_clk);
    ap_start = 0;
    while (ap_done !== 1'b1 && cycle < 219490) begin
      @(negedge ap_clk);
      cycle = cycle + 1;
      if (cycle >= 219478)
        $display("cycle=%0d state=%0d done=%b return=%h accumulated=%h bound=%h sum=%h exit=%b",
                 cycle, dut.state, ap_done, ap_return, dut.v_acc_06_i44_i430_i,
                 dut.v_v124, dut.v_v169, dut.v_cmp_not_i48_i434_i);
    end
    $display("RESULT cycle=%0d state=%0d done=%b return=%h", cycle, dut.state, ap_done, ap_return);
    $finish;
  end
endmodule

module adi_first_unknown;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  wire [31:0] ap_return;
  integer cycle = 0;
  benchmark_main dut(.*);
  always #5 ap_clk = ~ap_clk;
  initial begin
    repeat (2) @(negedge ap_clk);
    ap_rst = 0;
    ap_start = 1;
    @(negedge ap_clk);
    ap_start = 0;
    repeat (300000) @(negedge ap_clk);
    $finish;
  end
  task check_port(input [7:0] name, input enable, write_enable, input [8:0] address,
                  input [31:0] stored_word, write_word);
    if (enable && ((^(write_enable ? write_word : stored_word)) === 1'bx)) begin
      $display("unknown memory=%s write=%b edge=%0d state=%0d address=%0d",
               name, write_enable, cycle, dut.state, address);
      $finish;
    end
  endtask
  always @(posedge ap_clk) if (!ap_rst) begin
    cycle = cycle + 1;
    check_port("u", dut.u_ce0, dut.u_we0, dut.u_address0, dut.u_storage[dut.u_address0], dut.u_d0);
    check_port("p", dut.p_ce0, dut.p_we0, dut.p_address0, dut.p_storage[dut.p_address0], dut.p_d0);
    check_port("q", dut.q_ce0, dut.q_we0, dut.q_address0, dut.q_storage[dut.q_address0], dut.q_d0);
    check_port("v", dut.v_ce0, dut.v_we0, dut.v_address0, dut.v_storage[dut.v_address0], dut.v_d0);
  end
endmodule
