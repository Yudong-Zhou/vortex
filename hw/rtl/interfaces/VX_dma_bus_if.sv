`include "VX_define.vh"
import VX_gpu_pkg::*;

interface VX_dma_bus_if ();
    // 请求通道
    logic      req_valid;
    req_data_t req_data;
    logic      req_ready;
    
    logic      rsp_valid;
    rsp_data_t rsp_data;
    logic      rsp_ready;
    
    modport master (output req_valid, req_data, rsp_ready,
                    input  req_ready, rsp_valid, rsp_data);
    modport slave  (input  req_valid, req_data, rsp_ready,
                    output req_ready, rsp_valid, rsp_data);
endinterface
