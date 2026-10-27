`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 16.05.2025 10:54:12
// Design Name: 
// Module Name: generator_test
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: 
// 
// Dependencies: 
// 
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
// 
//////////////////////////////////////////////////////////////////////////////////


module generator_test(
        input       wire            clk,
        input       wire            rst_n,
        output      reg     [15:0]  dac_val,
        output      wire            state
    );
    
    localparam      switch_val      =       12500000;
    
    reg         [31:0]          count;
    wire                        switch;
    
    assign switch = (count == switch_val);
    assign state = dac_val[0];
    
    always @(posedge clk)
    begin
      if (rst_n == 0 || switch) begin
        count <= 0;
      end else begin
        count <= count + 1;
      end
    end

    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        dac_val <= 16'h7FFF;
      end else if (switch) begin
        dac_val = ~dac_val;
        dac_val = dac_val + 1;
      end
    end
endmodule
