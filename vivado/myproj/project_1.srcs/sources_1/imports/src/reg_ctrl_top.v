`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 02/20/2025 12:32:01 PM
// Design Name: 
// Module Name: reg_ctrl_top
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

module reg_ctrl_top#
(   parameter S_AXI_REG_ADDR_BITS   = 16,
    parameter ADDR_WIDTH = 14,
    parameter DECIMATION_WIDTH = 32,
    parameter FIFO_DEPTH            = 15
)(
    input  wire                                   clk,
    input  wire                                   rst_n,
    
    input  wire                                   s_axi_reg_aclk,    
    input  wire                                   s_axi_reg_aresetn,    
    input  wire [S_AXI_REG_ADDR_BITS-1:0]         s_axi_reg_awaddr,     
    input  wire [2:0]                             s_axi_reg_awprot,  
    input  wire                                   s_axi_reg_awvalid,    
    output wire                                   s_axi_reg_awready,                                   
    input  wire [31:0]                            s_axi_reg_wdata,   
    input  wire [3:0]                             s_axi_reg_wstrb,     
    input  wire                                   s_axi_reg_wvalid,     
    output wire                                   s_axi_reg_wready,     
    output wire [1:0]                             s_axi_reg_bresp,       
    output wire                                   s_axi_reg_bvalid,       
    input  wire                                   s_axi_reg_bready,   
    input  wire [S_AXI_REG_ADDR_BITS-1:0]         s_axi_reg_araddr,     
    input  wire [2:0]                             s_axi_reg_arprot,  
    input  wire                                   s_axi_reg_arvalid,    
    output wire                                   s_axi_reg_arready,         
    output wire [31:0]                            s_axi_reg_rdata, 
    output wire [1:0]                             s_axi_reg_rresp,
    output wire                                   s_axi_reg_rvalid,
    input  wire                                   s_axi_reg_rready,
    
    //input wire [63:0]                             axi_data,
    input wire [FIFO_DEPTH-1:0]                              transfer_rd_cnt,
    
    output  reg     [3:0]                         fifo_min_thresh,
    output  reg     [31:0]                        dest_addr,
    output  reg     [31:0]                        buff_size,
    output  reg     [31:0]                        ctrl_reg,
    
    output  reg     [31:0]                        cfg_dec,
    output  reg                                   start_acq,
    
    output  reg     [ADDR_WIDTH-1:0]                waveform_len_1,
    output  reg     [ADDR_WIDTH-1:0]                waveform_len_2,
    output  reg                                     start_gen,
    output  reg     [DECIMATION_WIDTH-1:0]          decimation_1,
    output  reg     [DECIMATION_WIDTH-1:0]          decimation_2,
    output  reg     [DECIMATION_WIDTH-1:0]          integrator_dec,
    
    output wire                                     set_wfm,
    output wire     [ADDR_WIDTH-1:0]                wfm_addr,
    input   wire        [31:0]                      wfm_read,
    output  wire        [31:0]                      reg_wr_data,
    input   wire        [31:0]                      buffer_n,
    output  wire                                    req_int,
    output  reg                                     input_sel
);
   
    // PARAMETERS
    
    localparam DEC_FACTOR_ADDR       = 16'h0000;   //0  Decimation factor address     
    localparam START_ACQ_ADDR        = 16'h0004;   //4  Start acquisiton signal address     
    localparam DEST_ADDR             = 16'h0008;   //8  Destination address address
    localparam BUFF_SIZE_ADDR        = 16'h000C;   //12 Buffer size address
    localparam TRANSFER_RD_CNT_ADDR  = 16'h0010;  //20 Number of reads in fifo address
    localparam FIFO_MIN_THRESH_ADDR  = 16'h0014;  //24 Minimum number of counts in FIFO for transfer to happen
    localparam FIFO_DOUT_ADDR_1      = 16'h0018;
    localparam FIFO_DOUT_ADDR_2      = 16'h001C;
    localparam CTRL_REG_ADDR         = 16'h0020;
    localparam INTEGRATOR_DEC_ADDR   = 16'h0024;
    localparam BUFFER_N_ADDR         = 16'h0028;
    localparam INPUT_SEL_ADDR        = 16'h002C;
    
    localparam START_GEN_ADDR        = 16'h1000;
    localparam DECIMATION_GEN_ADDR_1 = 16'h1004;
    localparam DECIMATION_GEN_ADDR_2 = 16'h1008;
    localparam WAVEFORM_LEN_1_ADDR   = 16'h100C;
    localparam WAVEFORM_LEN_2_ADDR   = 16'h1010;
    
    localparam SHIFT_WFM             = 16'h2000;
    
    localparam CTRL_REG_INTR_ADDR       = 0;
    localparam REQ_CTRL_REG_INTR_ADDR   = 1;
    
    // SIGNALS
    
    wire                                                reg_en;
    wire [3:0]                                          reg_we;
    wire                                                reg_wr_we;
    wire [S_AXI_REG_ADDR_BITS-1:0]                      reg_addr;    
    reg  [31:0]                                         reg_rd_data;

    assign reg_wr_we = reg_en & (reg_we == 4'hF);
    assign set_wfm = rst_n & (reg_addr >= SHIFT_WFM) & (reg_wr_we == 1);
    assign wfm_addr = (reg_addr[S_AXI_REG_ADDR_BITS-1:2] - SHIFT_WFM[S_AXI_REG_ADDR_BITS-1:2]);
    
    assign req_int = ctrl_reg[REQ_CTRL_REG_INTR_ADDR];

    reg_ctrl U_reg_ctrl(
        .s_axi_aclk     (s_axi_reg_aclk),       
        .s_axi_aresetn  (s_axi_reg_aresetn), 
        .s_axi_awaddr   (s_axi_reg_awaddr),
        .s_axi_awprot   (s_axi_reg_awprot),   
        .s_axi_awvalid  (s_axi_reg_awvalid), 
        .s_axi_awready  (s_axi_reg_awready), 
        .s_axi_wdata    (s_axi_reg_wdata),     
        .s_axi_wstrb    (s_axi_reg_wstrb),     
        .s_axi_wvalid   (s_axi_reg_wvalid),   
        .s_axi_wready   (s_axi_reg_wready),   
        .s_axi_bresp    (s_axi_reg_bresp),     
        .s_axi_bvalid   (s_axi_reg_bvalid),   
        .s_axi_bready   (s_axi_reg_bready),   
        .s_axi_araddr   (s_axi_reg_araddr),   
        .s_axi_arprot   (s_axi_reg_arprot),   
        .s_axi_arvalid  (s_axi_reg_arvalid), 
        .s_axi_arready  (s_axi_reg_arready), 
        .s_axi_rdata    (s_axi_reg_rdata),     
        .s_axi_rresp    (s_axi_reg_rresp),     
        .s_axi_rvalid   (s_axi_reg_rvalid),   
        .s_axi_rready   (s_axi_reg_rready),   
        .bram_rst_a     (reg_rst),       
        .bram_clk_a     (reg_clk),       
        .bram_en_a      (reg_en),         
        .bram_we_a      (reg_we),         
        .bram_addr_a    (reg_addr),     
        .bram_wrdata_a  (reg_wr_data), 
        .bram_rddata_a  (reg_rd_data));  
           
    // GET DECIMATION FACTOR
    
    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        cfg_dec <= 1;
      end else begin
        if ((reg_addr[16-1:0] == DEC_FACTOR_ADDR ) && (reg_wr_we == 1)) begin
          cfg_dec <= reg_wr_data;
        end
      end
    end
    
    // GET DECIMATION FACTOR
    
    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        integrator_dec <= 1;
      end else begin
        if ((reg_addr[16-1:0] == INTEGRATOR_DEC_ADDR ) && (reg_wr_we == 1)) begin
          integrator_dec <= reg_wr_data;
        end
      end
    end
    
    // GET START_ACQ SIGNAL
    
    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        start_acq <= 1'b0;
      end else begin
        if ((reg_addr[16-1:0] == START_ACQ_ADDR ) && (reg_wr_we == 1)) begin
          start_acq <= reg_wr_data[0];
        end
      end
    end
    
    // GET DEST_ADDR 
    
    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        dest_addr <= 0;
      end else begin
        if ((reg_addr[16-1:0] == DEST_ADDR ) && (reg_wr_we == 1)) begin
          dest_addr <= reg_wr_data;
        end
      end
    end
    
    // GET BUFF_SIZE 
    
    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        buff_size <= 1024;
      end else begin
        if ((reg_addr[16-1:0] == BUFF_SIZE_ADDR ) && (reg_wr_we == 1)) begin
          buff_size <= reg_wr_data;
        end
      end
    end
    
    // GET fifo_min_thresh 
    
    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        fifo_min_thresh <= 15;
      end else begin
        if ((reg_addr[16-1:0] == FIFO_MIN_THRESH_ADDR ) && (reg_wr_we == 1)) begin
          fifo_min_thresh <= reg_wr_data[3:0];
        end
      end
    end
    
    // GET fifo_min_thresh 
    
    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        input_sel <= 0;
      end else begin
        if ((reg_addr[16-1:0] == INPUT_SEL_ADDR ) && (reg_wr_we == 1)) begin
          input_sel <= reg_wr_data[0];
        end
      end
    end
    
    // GET ctrl_reg 
    
    always @(posedge clk)
    begin
      if (~rst_n) begin
        ctrl_reg <= 32'h0002;
      end else if((reg_addr[16-1:0] == CTRL_REG_ADDR) && (reg_wr_we == 1)) begin
        ctrl_reg <= reg_wr_data;
      end else begin
            
        if (ctrl_reg[CTRL_REG_INTR_ADDR]) begin
            ctrl_reg[CTRL_REG_INTR_ADDR] <= 1'b0;
        end
            
      end        
    end
    
    // GET decimation_gen 
    
    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        decimation_2 <= 1;        
      end else begin
        if ((reg_addr[16-1:0] == DECIMATION_GEN_ADDR_2 ) && (reg_wr_we == 1)) begin
          decimation_2 <= reg_wr_data;
        end
      end
    end
    
    // GET decimation_gen 
    
    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        decimation_1 <= 1;       
      end else begin
        if ((reg_addr[16-1:0] == DECIMATION_GEN_ADDR_1 ) && (reg_wr_we == 1)) begin
          decimation_1 <= reg_wr_data;
        end
      end
    end
    
    // GET waveform_len_1 
    
    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        waveform_len_1 <= 16384;     
      end else begin
        if ((reg_addr[16-1:0] == WAVEFORM_LEN_1_ADDR) && (reg_wr_we == 1)) begin
          waveform_len_1 <= reg_wr_data[ADDR_WIDTH-1:0];
        end
      end
    end
    
    // GET waveform_len_2
    
    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        waveform_len_2 <= 16384;     
      end else begin
        if ((reg_addr[16-1:0] == WAVEFORM_LEN_2_ADDR) && (reg_wr_we == 1)) begin
          waveform_len_2 <= reg_wr_data[ADDR_WIDTH-1:0];
        end
      end
    end
    
    // GET start_gen 
    
    always @(posedge clk)
    begin
      if (rst_n == 0) begin
        start_gen <= 1'b0;
      end else begin
        if ((reg_addr[16-1:0] == START_GEN_ADDR ) && (reg_wr_we == 1)) begin
          start_gen <= reg_wr_data[0];
        end
      end
    end

////////////////////////////////////////////////////////////
// Name : Register Read Data
// 
////////////////////////////////////////////////////////////

always @(posedge clk)
begin
  case (reg_addr[16-1:0])
    DEC_FACTOR_ADDR:        reg_rd_data <= cfg_dec;
    START_ACQ_ADDR:         reg_rd_data <= {31'b0, start_acq};
    DEST_ADDR:              reg_rd_data <= dest_addr;
    BUFF_SIZE_ADDR:         reg_rd_data <= buff_size;
    TRANSFER_RD_CNT_ADDR:   reg_rd_data <= {{(32-FIFO_DEPTH){1'b0}}, transfer_rd_cnt};    
    FIFO_MIN_THRESH_ADDR:   reg_rd_data <= {28'b0, fifo_min_thresh}; 
    START_GEN_ADDR:         reg_rd_data <= {31'b0, start_gen};
    DECIMATION_GEN_ADDR_1:  reg_rd_data <= decimation_1;
    DECIMATION_GEN_ADDR_2:  reg_rd_data <= decimation_2;
    WAVEFORM_LEN_1_ADDR:    reg_rd_data <= waveform_len_1;
    WAVEFORM_LEN_2_ADDR:    reg_rd_data <= waveform_len_2;
    INTEGRATOR_DEC_ADDR:    reg_rd_data <= integrator_dec;
    BUFFER_N_ADDR:          reg_rd_data <= buffer_n;
    default                 reg_rd_data <= wfm_read;                  
  endcase
end
    
endmodule
