`timescale 1ns / 1ps

module waveform_top #(
    parameter DATA_WIDTH_IN = 32,       // Data width of the BRAM
    parameter DATA_WIDTH_OUT = 16,       // Data width of the BRAM
    parameter ADDR_WIDTH = 14,
    parameter DECIMATION_WIDTH = 16       // Address width for 20000 samples (2^15 > 20000)
)(
    input  wire                                 clk,
    input  wire                                 rst_n,
    
    output reg                                  bram_dout_valid,
    output wire [DATA_WIDTH_OUT-1:0]            bram_dout_1,
    output wire [DATA_WIDTH_OUT-1:0]            bram_dout_2,
    input wire  [ADDR_WIDTH-1:0]                bram_read_addr_1,
    input wire  [ADDR_WIDTH-1:0]                bram_read_addr_2,
    
    input wire [ADDR_WIDTH-1:0]                 bram_write_addr,
    input wire                                  bram_we,
    
    output      reg     [4:0]                   n_write,
    output      wire    [DATA_WIDTH_IN-1:0]     bram_read,
    input       wire    [DATA_WIDTH_IN-1:0]     bram_din     
);

    // BRAM signals
    wire                        bram_we_1;
    wire                        bram_we_2;
    
    wire                        bram_read_1;
    wire                        bram_read_2;
    
    assign bram_we_1 = ~bram_write_addr[ADDR_WIDTH-1] && bram_we;
    assign bram_we_2 = ~bram_we_1 && bram_we; 
    
    assign bram_read = bram_write_addr[ADDR_WIDTH-1] ? bram_read_2 : bram_read_1;
    
    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        bram_dout_valid <= 0;
      end else begin
        bram_dout_valid <= ~bram_we;
      end
    end
    
    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        n_write <= 0;
      end else if (bram_we_2) begin
        n_write <= n_write + 1;
      end
    end

    // Block Memory Generator instance
    blk_mem_gen_0 #(
    ) blk_mem_inst_1 (
        .addra(bram_write_addr[14-2:0]),
        .dina(bram_din),
        .douta(bram_read_1),
        .wea(bram_we_1),
        .clka(clk),
        .addrb(bram_read_addr_1),
        .dinb(16'b0),
        .doutb(bram_dout_1),
        .web(1'b0),
        .clkb(clk)
    );
    
    // Block Memory Generator instance
    blk_mem_gen_0 #(
    ) blk_mem_inst_2 (
        .addra(bram_write_addr[14-2:0]),
        .dina(bram_din),
        .douta(bram_read_2),
        .wea(bram_we_2),
        .clka(clk),
        .addrb(bram_read_addr_2),
        .dinb(16'b0),
        .doutb(bram_dout_2),
        .web(1'b0),
        .clkb(clk)
    );

endmodule
