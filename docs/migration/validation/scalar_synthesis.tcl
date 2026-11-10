cd {/home/eeuser/Desktop/Light-HLS/runs/migration_20261110/scalar_synthesis/reports/vivado_report}
set_param general.maxThreads 1
read_verilog -sv {/home/eeuser/Desktop/Light-HLS/runs/migration_20261110/scalar_synthesis/reports/rtl/top.v}
read_xdc -unmanaged {/home/eeuser/Desktop/Light-HLS/runs/migration_20261110/scalar_synthesis/reports/vivado_report/constraints.xdc}
synth_design -top {backend_scalar} -part {xcu250-figd2104-2L-e} -mode out_of_context -flatten_hierarchy rebuilt
if {[llength [get_clocks -quiet ap_clk]] != 1} { error "Required pre-synthesis clock is missing" }
write_checkpoint -force {/home/eeuser/Desktop/Light-HLS/runs/migration_20261110/scalar_synthesis/reports/vivado_report/post_synth.dcp}
report_utilization -file {/home/eeuser/Desktop/Light-HLS/runs/migration_20261110/scalar_synthesis/reports/vivado_report/utilization.rpt}
report_timing_summary -delay_type max -max_paths 10 -file {/home/eeuser/Desktop/Light-HLS/runs/migration_20261110/scalar_synthesis/reports/vivado_report/timing_summary.rpt}
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
set metrics [open {/home/eeuser/Desktop/Light-HLS/runs/migration_20261110/scalar_synthesis/reports/vivado_report/vivado_metrics.txt} w]
puts $metrics "tool_version=[version -short]"
puts $metrics "lut=$lut"
puts $metrics "ff=$ff"
puts $metrics "dsp=$dsp"
puts $metrics "bram_tiles=$bram"
puts $metrics "worst_slack_ns=$wns"
puts $metrics "flow_id=vivado_ooc_native_v3_pre_synth_io20"
puts $metrics "io_budget_fraction=0.2"
close $metrics
exit
