`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 19.05.2025 11:33:42
// Design Name: 
// Module Name: vector_split
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


module vector_split (
    input  [4:0] in_vector,
    output       out0,
    output       out1,
    output       out2,
    output       out3,
    output       out4
);

assign out0 = in_vector[0];
assign out1 = in_vector[1];
assign out2 = in_vector[2];
assign out3 = in_vector[3];
assign out4 = in_vector[4];

endmodule
