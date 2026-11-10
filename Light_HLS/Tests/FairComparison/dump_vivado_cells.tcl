# Inspect a synthesized checkpoint without changing its implementation.
if {[llength $argv] != 2} {
    error "Usage: vivado -mode batch -source dump_vivado_cells.tcl -tclargs input.dcp output.tsv"
}
set_param general.maxThreads 1
open_checkpoint [lindex $argv 0]
set report [open [lindex $argv 1] w]
puts $report "cell\tprimitive"
foreach cell [lsort [get_cells -hierarchical -filter {IS_PRIMITIVE == 1}]] {
    puts $report "$cell\t[get_property REF_NAME $cell]"
}
close $report
exit
