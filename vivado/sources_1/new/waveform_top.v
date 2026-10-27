`timescale 1ns / 1ps

module waveform_top #(
    parameter DATA_WIDTH_IN = 32,       // Data width of the BRAM
    parameter DATA_WIDTH_OUT = 16,       // Data width of the BRAM
    parameter ADDR_WIDTH = 14,
    parameter DECIMATION_WIDTH = 16       // Address width for 20000 samples (2^15 > 20000)
)(
    input  wire                 clk,
    input  wire                 rst_n,
    
    output reg                          bram_dout_valid,
    output wire [DATA_WIDTH_OUT-1:0]    bram_dout_1,
    output wire [DATA_WIDTH_OUT-1:0]    bram_dout_2,
    input wire  [ADDR_WIDTH-1:0]        bram_read_addr_1,
    input wire  [ADDR_WIDTH-1:0]        bram_read_addr_2,
    
    input wire [ADDR_WIDTH-1:0]         bram_write_addr,
    input wire                          bram_we
);

    // BRAM signals
    wire [ADDR_WIDTH-1:0]       bram_addr_1;
    wire [ADDR_WIDTH-1:0]       bram_addr_2;
    wire [DATA_WIDTH_IN-1:0]    bram_din;
    wire                        bram_we_1;
    wire                        bram_we_2;
    wire                        bram_clk;
    
    localparam      waveform_border  =  8000;
    
    assign bram_addr_1 = bram_we ? bram_write_addr : bram_read_addr_1;
    assign bram_addr_2 = bram_we ? bram_write_addr - waveform_border : bram_read_addr_2;
    
    assign bram_we_1 = (bram_write_addr < waveform_border) && bram_we;
    assign bram_we_2 = ~bram_we_1 && bram_we; 
    
    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        bram_dout_valid <= 0;
      end else begin
        bram_dout_valid <= ~bram_we;
      end
    end

    // Block Memory Generator instance
    blk_mem_gen_0 #(
    ) blk_mem_inst_1 (
        .addra(bram_addr_1),
        .dina(bram_din),
        .douta(bram_dout_1),
        .wea(bram_we_1),
        .clka(bram_clk)
    );
    
    // Block Memory Generator instance
    blk_mem_gen_0 #(
    ) blk_mem_inst_2 (
        .addra(bram_addr_2),
        .dina(bram_din),
        .douta(bram_dout_2),
        .wea(bram_we_2),
        .clka(bram_clk)
    );

endmodule
