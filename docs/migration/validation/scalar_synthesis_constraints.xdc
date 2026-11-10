# Apply the target clock and a symmetric external IO budget before synthesis.
create_clock -name ap_clk -period 10 [get_ports ap_clk]
set data_inputs [get_ports -filter {DIRECTION == IN && NAME != ap_clk}]
set_input_delay -clock ap_clk -max 2 $data_inputs
set_input_delay -clock ap_clk -min 0 $data_inputs
set_output_delay -clock ap_clk -max 2 [all_outputs]
set_output_delay -clock ap_clk -min 0 [all_outputs]
