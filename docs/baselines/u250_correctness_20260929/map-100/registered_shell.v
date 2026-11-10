`timescale 1ns/1ps
// Closed on-chip verification context, not a U250 board/host interface.
module registered_pressure_shell(input wire ap_clk);
  wire kernel_clk;
`ifdef SHELL_SIM_CLOCK
  // The functional model omits only the ideal global clock buffer.
  assign kernel_clk = ap_clk;
`else
  BUFG clock_buffer(.I(ap_clk), .O(kernel_clk));
`endif
  (* DONT_TOUCH = "yes" *) reg [1:0] reset_count = 0;
  wire ap_rst = reset_count != 3;
  (* DONT_TOUCH = "yes" *) reg [5:0] offers = 0;
  wire ap_start = !ap_rst && offers < 32;
  (* DONT_TOUCH = "yes" *) reg [31:0] coefficient = 7;
  wire ap_done, ap_idle, ap_ready;
  wire [7:0] input_r_address0, output_r_address0;
  wire input_r_ce0, output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  (* DONT_TOUCH = "yes" *) reg [31:0] input_r_q0 = 0;
  // Extra normalized ports are inserted only for the DATAFLOW contract.
  
  (* DONT_TOUCH = "yes" *) reg [53:0] observed_outputs = 0;
  always @(posedge kernel_clk) begin
    if (ap_rst) begin
      reset_count <= reset_count + 1;
      offers <= 0;
      coefficient <= 7;
      input_r_q0 <= 0;
    end else begin
      if (ap_start && ap_ready) begin
        offers <= offers + 1;
        coefficient <= coefficient + 29;
      end
      // One-cycle immutable RAM response; no added address or data stage.
      if (input_r_ce0)
        input_r_q0 <= (input_r_address0 * 131) ^ 32'ha531e297;
    end
    // Retain every meaningful output so the kernel cannot become dead logic.
    observed_outputs <= {ap_done, ap_idle, ap_ready, input_r_address0,
      input_r_ce0, output_r_address0, output_r_ce0, output_r_we0, output_r_d0};
  end
  (* keep_hierarchy = "yes" *) qor_pressure__normalized kernel (
    .ap_clk(kernel_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .coefficient(coefficient), .input_r_address0(input_r_address0),
    .input_r_ce0(input_r_ce0), .input_r_q0(input_r_q0),
    .output_r_address0(output_r_address0), .output_r_ce0(output_r_ce0),
    .output_r_we0(output_r_we0), .output_r_d0(output_r_d0)
    
  );
endmodule
