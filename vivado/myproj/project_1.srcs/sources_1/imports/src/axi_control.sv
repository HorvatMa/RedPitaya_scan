`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 11/23/2022 09:00:09 PM
// Design Name: 
// Module Name: axi_control
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


module axi_control#(
    parameter AXI_DATA_WID = 64,
    parameter ACQ_DATA_WID = 16,
    parameter CTRL_REG_ADDR = 1,
    parameter S_AXI_REG_ADDR_BITS = 12,
    parameter M_AXI_CNT_ADDR_BITS   = 32,
    parameter FIFO_DEPTH            = 15)
    (
    input wire [ACQ_DATA_WID-1:0]  osc1_data,
    input wire [ACQ_DATA_WID-1:0]  osc2_data,
    input wire [32-1:0]  cnt_data,
    input wire                     trigger,
    
    input wire                     clk,
    input wire                     rst_n,
    input wire [3:0]               fifo_min_thresh,
    input wire                     transfer,
    
    output reg                     transfer_in_progress,
    output wire                    last,
    output wire [AXI_DATA_WID-1:0] axi_data,
    output reg [FIFO_DEPTH-1:0]               transfer_rd_cnt,
    output reg                     intr,
    input  wire                    resp,
    
    input   wire    [31:0]          ctrl_reg,
    output  wire [FIFO_DEPTH-1:0]              fifo_rd_cnt,
    output  reg                     busy,
    output  reg     [4:0]           n_intr,
    output  reg [(M_AXI_CNT_ADDR_BITS-1):0]     addr_shift,
    output  reg        [31:0]                      buffer_n,
    output  reg                    full_o
    );
    
//    localparam SELECT_WAIT  = 3'b000;
//    localparam SELECT_OSC1  = 3'b001;
//    localparam SELECT_OSC2  = 3'b010;
//    localparam SELECT_CNT   = 3'b011;
//    localparam SELECT_LAST  = 3'b100;
    
    localparam INTR_ADDR            = 0;
    localparam REQUEST_INTR_ADDR    = 1;
    
//    reg [2:0]               selector;
//    reg [ACQ_DATA_WID-1:0]  sel_data;
//    reg [ACQ_DATA_WID-1:0]  osc1_fifo_data;
//    reg [ACQ_DATA_WID-1:0]  osc2_fifo_data;
//    reg [ACQ_DATA_WID-1:0]  cnt_fifo_data;
    reg [7:0]               cnt;
    
//    wire                    activate;
//    wire                    fifo_wr_en;
    wire                    fifo_rst;
//    wire [63:0]             data_swap;
    wire                    initiate_transfer;
    
    wire [63:0]             data_in;
    wire [63:0]             data_out;
    wire                    request_interrupt;
    wire                    data_valid;
    reg [FIFO_DEPTH-1:0]              fifo_transfer_rd_cnt;
    wire                    full;
    
    
    assign data_valid = buffer_n > fifo_transfer_rd_cnt;
    assign initiate_transfer = (fifo_rd_cnt > fifo_min_thresh) && (~busy) && (~request_interrupt);
    assign request_interrupt = ctrl_reg[REQUEST_INTR_ADDR] && data_valid;
    
//    assign axi_data[15:0] = data_swap[63:48];
//    assign axi_data[31:16] = data_swap[47:32];
//    assign axi_data[47:32] = data_swap[31:16];
//    assign axi_data[63:48] = data_swap[15:0];
    
    assign fifo_rst = ~rst_n;
    assign last = (cnt == transfer_rd_cnt);
    assign axi_data = data_out;
    
    assign data_in[15:0] = osc1_data;
    assign data_in[31:16] = osc2_data;
    assign data_in[63:32] = cnt_data;
    
//    assign activate = (selector == SELECT_WAIT) && trigger;
//    assign fifo_wr_en = selector > SELECT_OSC1;
    
    always @(posedge clk)
    begin
        if (~rst_n) begin
            busy <= 0;
        end else if (initiate_transfer) begin
            busy <= 1;
        end else if (resp) begin
            busy <= 0;
        end
    end
    
    always @(posedge clk)
    begin
        if (~rst_n) begin
            fifo_transfer_rd_cnt <= 0;
        end else if (ctrl_reg[INTR_ADDR]) begin
            fifo_transfer_rd_cnt <= {fifo_rd_cnt[FIFO_DEPTH-1:4], 4'b0} - 1;
        end
    end
    
    always @(posedge clk)
    begin
        if (~rst_n) begin
            full_o <= 0;
        end else if (ctrl_reg[INTR_ADDR]) begin
            full_o <= full;
        end
    end
    
    always @(posedge clk)
    begin
        if (~rst_n || ctrl_reg[INTR_ADDR]) begin
            buffer_n <= 0;
        end else if (initiate_transfer) begin
            buffer_n <= buffer_n + 16;
        end       
    end
    
    always @(posedge clk)
    begin
        if (~rst_n || ctrl_reg[INTR_ADDR]) begin
            addr_shift <= 0;
        end else if (last) begin
            addr_shift <= addr_shift + 128;
        end
    end
    
    always @(posedge clk)
    begin
        if (~rst_n || last) begin
            cnt <= 0;
        end else if (transfer) begin
            cnt += 1;
        end
    end
    
    always @(posedge clk)
    begin
        if (~rst_n) begin
            transfer_rd_cnt <= 15;
        end else if (initiate_transfer) begin
            if (fifo_rd_cnt > 15) begin
                transfer_rd_cnt <= 15;
            end else begin
//                transfer_rd_cnt <= fifo_rd_cnt;
                transfer_rd_cnt <= 15;
            end
        end
    end
    
    always @(posedge clk)
    begin
        if (~rst_n) begin
            transfer_in_progress <= 1'b0;
        end else if (initiate_transfer) begin
            transfer_in_progress <= 1'b1;
        end else if (last) begin
            transfer_in_progress <= 1'b0;
        end
    end
    
    
    always @(posedge clk)
    begin
        if (~rst_n || ctrl_reg[INTR_ADDR]) begin
            intr <= 0;
        end else if (request_interrupt && ~busy) begin
            intr <= 1;
        end 
    end
    
//    always @(posedge clk)
//    begin
//        if (activate) begin
//            osc1_fifo_data <= osc1_data;
//            osc2_fifo_data <= osc2_data;
//            cnt_fifo_data  <= cnt_data;
//        end
//    end
    
//    always @(posedge clk)
//    begin
//        if (~rst_n || selector == SELECT_LAST) begin
//            selector = SELECT_WAIT;
//        end else if (activate || (selector != SELECT_WAIT)) begin
//            selector += 1;
//        end
//    end
    
//    always @(posedge clk)
//    begin
//        case (selector)
//            SELECT_OSC1 : sel_data <= osc1_fifo_data;
//            SELECT_OSC2 : sel_data <= osc2_fifo_data;
//            SELECT_CNT  : sel_data <= cnt_fifo_data;
//            default     : sel_data <= {16{1'b1}};
//        endcase
//    end
    
    fifo_generator_64_64 FIFO(
        .srst            (fifo_rst),
        .clk            (clk),
        .din            (data_in),
        .wr_en          (trigger), 
        .dout           (data_out),    
        .rd_en          (transfer),                    
        .data_count     (fifo_rd_cnt),
        .full           (full)               
    );
    
//    FIFO_16_64 FIFO(
//        .rst            (fifo_rst),
//        .wr_clk         (clk),
//        .rd_clk         (clk),
//        .din            (sel_data),
//        .wr_en          (fifo_wr_en),               
//        .full           (),   
//        .dout           (data_swap),    
//        .rd_en          (transfer),                                 
//        .empty          (fifo_empty),                 
//        .rd_data_count  (fifo_rd_cnt), 
//        .wr_data_count  (),
//        .wr_rst_busy    (),     
//        .rd_rst_busy    ()
//    );
    
    reg intr_o;
    
    always @(posedge clk)
    begin
        intr_o <= intr;
    end
    
    always @(posedge clk)
    begin
        if (~rst_n) begin
            n_intr <= 0;
        end else if (~intr_o && intr) begin
            n_intr <= n_intr + 1;
        end
    end
    
endmodule
