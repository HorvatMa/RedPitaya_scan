`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 09.06.2025 11:06:56
// Design Name: 
// Module Name: input_selector
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


module input_selector(
        input   wire                                    rst_n,
        input   wire                                    clk,
        input   wire                                    input_sel,
        
        input   wire                [14-1:0]            input_adc,
        input   wire                [16-1:0]            input_dac,
        
        output  wire    signed      [16-1:0]            output_val
    );
    
    wire    signed      [16-1:0]        adc_trans;
    
    assign adc_trans = {14'sd8192 - input_adc, 2'b0};
    
    assign output_val = input_sel ? adc_trans : input_dac;
    
endmodule
