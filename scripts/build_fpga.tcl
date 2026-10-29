# Rebuild the deployed FPGA design (vivado/myproj/project_1.xpr) from the command line.
# Needs Vivado 2020.2 (the project's version; other versions would upgrade/lock the IP).
#
#   vivado -mode batch -nojournal -nolog -source scripts/build_fpga.tcl          (from the repo root)
#   vivado -mode batch -nojournal -nolog -source scripts/build_fpga.tcl -tclargs 8   (8 parallel jobs)
#
# Outputs: fpga/rp_top.bit (copied from impl_1) and fpga/reports/{timing_summary,utilization}.rpt.
# Exits non-zero if the bitstream is missing; timing failure is reported but does not abort.

set jobs 4
if {[llength $argv] > 0} { set jobs [lindex $argv 0] }

set root [file normalize [file join [file dirname [info script]] ..]]
open_project [file join $root vivado myproj project_1.xpr]

# The custom blocks are module references in the block design: refresh them so edits to
# imports/src/*.v and new/*.v reach the BD, then regenerate the BD outputs.
foreach ip [get_ips -quiet] {
    if {[catch {update_module_reference $ip} msg] == 0} { puts "refreshed module reference $ip" }
}
set bd [get_files system.bd]
generate_target all $bd
update_compile_order -fileset sources_1

reset_run synth_1
launch_runs impl_1 -to_step write_bitstream -jobs $jobs
wait_on_run impl_1

set bit [file join $root vivado myproj project_1.runs impl_1 rp_top.bit]
if {![file exists $bit]} {
    puts "ERROR: no bitstream; see vivado/myproj/project_1.runs/{synth_1,impl_1}/runme.log"
    exit 1
}

open_run impl_1
set rep [file join $root fpga reports]
file mkdir $rep
report_timing_summary -max_paths 100 -report_unconstrained -file [file join $rep timing_summary.rpt]
report_utilization -file [file join $rep utilization.rpt]
file copy -force $bit [file join $root fpga rp_top.bit]

set wns [get_property SLACK [get_timing_paths -max_paths 1 -setup]]
set whs [get_property SLACK [get_timing_paths -max_paths 1 -hold]]
puts "BUILD DONE: fpga/rp_top.bit  WNS=$wns ns  WHS=$whs ns"
if {$wns < 0 || $whs < 0} { puts "WARNING: timing NOT met, see fpga/reports/timing_summary.rpt" }
exit 0
