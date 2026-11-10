`timescale 1ns/1ps

// Optional diagnostic for the unchanged backend_pipeline App testbench.
// Select pipeline_event_probe as the simulation top and include both this
// file and backend_pipeline_tb.v alongside either generator's RTL files.
// EDGE records pre-NBA signals sampled by the synchronous memory model.
module pipeline_event_probe;
  backend_pipeline_tb bench();
  always @(posedge bench.ap_clk) begin
    if (!bench.ap_rst && (bench.ap_start || bench.input_ce0 || bench.output_ce0))
      $display("EDGE time=%0t read=%b read_addr=%d write=%b write_addr=%d done=%b",
               $time, bench.input_ce0, bench.input_address0,
               bench.output_ce0 && bench.output_we0, bench.output_address0,
               bench.ap_done);
  end
  always @(posedge bench.ap_done)
    $display("DONE time=%0t", $time);
endmodule
