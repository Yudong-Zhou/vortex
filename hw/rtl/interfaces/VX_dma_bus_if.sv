`include "VX_define.vh"

interface VX_dma_bus_if #(
    parameter ADDR_WIDTH = 32,
    parameter DATA_WIDTH = 64,
    parameter TAG_WIDTH  = 8
) ();
    // 请求通道
    typedef struct packed {
        logic [ADDR_WIDTH-1:0] src_addr;
        logic [ADDR_WIDTH-1:0] dst_addr;
        logic [15:0]           size;
        logic                  direction;  // 0=G2L, 1=L2G
        logic [TAG_WIDTH-1:0]  tag;        // DMA ID
    } req_data_t;
    
    logic      req_valid;
    req_data_t req_data;
    logic      req_ready;
    
    typedef struct packed {
        logic [TAG_WIDTH-1:0] tag;
    } rsp_data_t;
    
    logic      rsp_valid;
    rsp_data_t rsp_data;
    logic      rsp_ready;
    
    modport master (output req_valid, req_data, rsp_ready,
                    input  req_ready, rsp_valid, rsp_data);
    modport slave  (input  req_valid, req_data, rsp_ready,
                    output req_ready, rsp_valid, rsp_data);
endinterface