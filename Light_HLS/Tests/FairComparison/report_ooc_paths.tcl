# Separate register-to-register timing from virtual OOC boundary paths.
# This diagnostic does not override a failing full-interface timing result.
set_param general.maxThreads 1
open_checkpoint [lindex $argv 0]
set output_directory [lindex $argv 1]
set registers [all_registers -clock ap_clk]
set metrics [open [file join $output_directory internal_timing.txt] w]
foreach {delay label} {max setup min hold} {
    set paths [get_timing_paths -delay_type $delay -from $registers -to $registers -max_paths 1]
    if {[llength $paths]} {
        puts $metrics "$label=[get_property SLACK [lindex $paths 0]]"
        report_timing -delay_type $delay -from $registers -to $registers -max_paths 5 -file [file join $output_directory internal_$label.rpt]
    } else {
        puts $metrics "$label=nan"
    }
}
close $metrics
exit
