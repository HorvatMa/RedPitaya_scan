`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 02/25/2025 11:55:04 AM
// Design Name: 
// Module Name: integrator_top
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


module integrator_top#(
    parameter   ADC_WIDTH   =   14,
    parameter   OUT_WIDTH   =   32
)(
    input       wire                            clk,
    input       wire                            rst_n,
    input       wire    [32-1:0]                integrator_dec,
    
    input       wire [ADC_WIDTH-1:0]         signal_in,
    
    input       wire                            trig,
    
    output      reg  signed   [OUT_WIDTH-1:0]         integrated_signal,
    output      wire signed   [OUT_WIDTH-1:0]         integrated_signal_test
    );
    
    reg         signed          [OUT_WIDTH-1:0]           signal_old;
    wire        signed          [OUT_WIDTH-1:0]           tmp;
    reg                         [32-1:0]                  dec;
    wire        signed          [OUT_WIDTH-1:0]           signal_in_extended;
    
    assign signal_in_extended = {18'b0, 14'd16383 - signal_in};
    
    wire                            activate;
    
    assign activate = dec >= integrator_dec;
    assign tmp = (signal_in_extended + signal_old) >>> 1;
    
    assign integrated_signal_test = {18'b0, 14'd16383 - signal_in};
    
    always @ (posedge clk) begin
        if (~rst_n) begin
            dec <= 0;
        end else if (activate || trig) begin
            dec <= 1;
        end else begin
            dec <= dec + 1;
        end              
    end
    
    always @ (posedge clk) begin
        if (~rst_n || trig) begin
            signal_old <= signal_in_extended;
            integrated_signal <= 0;
        end else if (activate) begin
            integrated_signal <= integrated_signal + tmp;
//            integrated_signal <= integrated_signal + 1;
            signal_old <= signal_in_extended;
        end        
    end
    
endmodule
