set_param general.maxThreads 1
read_verilog -sv {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls/reports/rtl/top.v} {/tmp/lighthls-next3-20260928.t1orVg/dsp-map-100-v3/interface_normalization/light_hls/normalized_wrapper.v} {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/registered_shell.v}
read_xdc -unmanaged {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/constraints.xdc}
synth_design -top {registered_pressure_shell} -part {xcu250-figd2104-2L-e} -mode out_of_context -flatten_hierarchy rebuilt
set clock_ports [get_ports -quiet ap_clk]
if {[llength [get_clocks -quiet ap_clk]] != 1} { error "Required pre-synthesis clock constraint is missing" }
write_checkpoint -force {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/post_synth.dcp}
report_utilization -file {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/utilization.rpt}
report_timing_summary -delay_type max -max_paths 10 -file {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/timing_summary.rpt}
set utilization [report_utilization -return_string]
set lut -1
set ff -1
set dsp -1
set bram -1
regexp -line {\|\s*(Slice LUTs|CLB LUTs)\*?\s*\|\s*([0-9]+)} $utilization match label lut
regexp -line {\|\s*(Slice Registers|CLB Registers)\s*\|\s*([0-9]+)} $utilization match label ff
regexp -line {\|\s*DSPs\s*\|\s*([0-9]+)} $utilization match dsp
regexp -line {\|\s*Block RAM Tile\s*\|\s*([0-9.]+)} $utilization match bram
set wns nan
set timing_paths [get_timing_paths -delay_type max -max_paths 1]
if {[llength $timing_paths] > 0} { set wns [get_property SLACK [lindex $timing_paths 0]] }
set metrics [open {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/metrics.txt} w]
puts $metrics "tool_version=[version -short]"
puts $metrics "lut=$lut"
puts $metrics "ff=$ff"
puts $metrics "dsp=$dsp"
puts $metrics "bram_tiles=$bram"
puts $metrics "clock_constrained=[expr {[llength [get_clocks -quiet ap_clk]] > 0}]"
puts $metrics "constraints_before_synthesis=1"
puts $metrics "io_budget_fraction=0"
puts $metrics "worst_slack_ns=$wns"
foreach port [lsort [get_ports]] {
  set port_name [get_property NAME $port]
  set direction [get_property DIRECTION $port]
  set left [get_property -quiet LEFT $port]
  set right [get_property -quiet RIGHT $port]
  puts $metrics "port=$port_name|$direction|$left|$right"
}
close $metrics
if {[llength [get_ports]] != 1 || [get_property NAME [get_ports]] ne "ap_clk"} { error "Unexpected unconstrained external interface" }
if {[llength [get_cells -hier -filter {REF_NAME =~ BUFG*}]] != 1} { error "A physical global clock buffer is required" }
opt_design
place_design
phys_opt_design
route_design
write_checkpoint -force {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/post_route.dcp}
report_utilization -file {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/route_utilization.rpt}
report_timing_summary -delay_type min_max -report_unconstrained -file {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/route_timing_summary.rpt}
report_route_status -file {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/route_status.rpt}
report_drc -file {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/route_drc.rpt}
set utilization [report_utilization -return_string]
set lut -1
set ff -1
set dsp -1
set bram -1
regexp -line {\|\s*(Slice LUTs|CLB LUTs)\*?\s*\|\s*([0-9]+)} $utilization match label lut
regexp -line {\|\s*(Slice Registers|CLB Registers)\s*\|\s*([0-9]+)} $utilization match label ff
regexp -line {\|\s*DSPs\s*\|\s*([0-9]+)} $utilization match dsp
regexp -line {\|\s*Block RAM Tile\s*\|\s*([0-9.]+)} $utilization match bram
set wns nan
set timing_paths [get_timing_paths -delay_type max -max_paths 1]
if {[llength $timing_paths] > 0} { set wns [get_property SLACK [lindex $timing_paths 0]] }
set metrics [open {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/route_metrics.txt} w]
puts $metrics "tool_version=[version -short]"
puts $metrics "lut=$lut"
puts $metrics "ff=$ff"
puts $metrics "dsp=$dsp"
puts $metrics "bram_tiles=$bram"
puts $metrics "clock_constrained=[expr {[llength [get_clocks -quiet ap_clk]] > 0}]"
puts $metrics "constraints_before_synthesis=1"
puts $metrics "io_budget_fraction=0"
puts $metrics "worst_slack_ns=$wns"
foreach port [lsort [get_ports]] {
  set port_name [get_property NAME $port]
  set direction [get_property DIRECTION $port]
  set left [get_property -quiet LEFT $port]
  set right [get_property -quiet RIGHT $port]
  puts $metrics "port=$port_name|$direction|$left|$right"
}
close $metrics
set metrics [open {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/route_metrics.txt} a]
set hold_paths [get_timing_paths -delay_type min -max_paths 1]
puts $metrics "worst_hold_slack_ns=[get_property SLACK $hold_paths]"
puts $metrics "unrouted_nets=[llength [get_nets -hier -filter {ROUTE_STATUS == UNROUTED || ROUTE_STATUS == PARTIALLY_ROUTED}]]"
close $metrics
report_utilization -cells [get_cells kernel] -file {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/kernel_utilization.rpt}
report_utilization -hierarchical -file {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/hierarchical_utilization.rpt}
report_clock_utilization -file {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/clock_utilization.rpt}
report_timing -delay_type min -max_paths 10 -file {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/hold_paths.rpt}
report_timing -delay_type max -max_paths 10 -file {/tmp/lighthls-correctness-20260929.kZl9Xq/final-map-100/light_hls_context/setup_paths.rpt}
exit
