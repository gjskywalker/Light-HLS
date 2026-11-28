# Run from a fresh results directory. Match the PLDI 2024 OOC implementation sequence.
set rtl [lindex $argv 0]
set top [lindex $argv 1]
set clock_port [lindex $argv 2]
set part xc7z020clg484-1
set_param general.maxThreads 2
create_project -in_memory -part $part
read_verilog -sv $rtl
synth_design -mode out_of_context -no_iobuf -top $top -part $part
create_clock -name clk -period 10.000 [get_ports $clock_port]
opt_design
place_design -directive Explore
if {[get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]] < 0.5} {
    phys_opt_design
}
route_design -directive Explore
report_route_status -file post_route_status.rpt
report_timing_summary -report_unconstrained -file post_route_timing.rpt
report_utilization -file post_route_util.rpt
report_drc -file post_route_drc.rpt
set setup [get_timing_paths -max_paths 1 -nworst 1 -setup]
set hold [get_timing_paths -max_paths 1 -nworst 1 -hold]
if {![llength $setup] || ![llength $hold]} {
    error "No constrained setup/hold path for routed design"
}
set fp [open timing.tsv w]
puts $fp "vivado\t[version -short]"
puts $fp "part\t[get_property PART [current_design]]"
puts $fp "wns_ns\t[get_property SLACK $setup]"
puts $fp "whs_ns\t[get_property SLACK $hold]"
puts $fp "start_pin\t[get_property STARTPOINT_PIN $setup]"
puts $fp "end_pin\t[get_property ENDPOINT_PIN $setup]"
close $fp
close_project
