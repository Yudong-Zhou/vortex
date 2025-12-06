// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

`include "VX_define.vh"

module VX_sfu_unit import VX_gpu_pkg::*; #(
    parameter `STRING INSTANCE_ID = "",
    parameter CORE_ID = 0
) (
    input wire              clk,
    input wire              reset,

`ifdef PERF_ENABLE
    input sysmem_perf_t     sysmem_perf,
    input pipeline_perf_t   pipeline_perf,
`endif

    input base_dcrs_t       base_dcrs,

    // Inputs
    VX_dispatch_if.slave    dispatch_if [`ISSUE_WIDTH],

`ifdef EXT_F_ENABLE
    VX_fpu_csr_if.slave     fpu_csr_if [`NUM_FPU_BLOCKS],
`endif

    VX_commit_csr_if.slave  commit_csr_if,
    VX_sched_csr_if.slave   sched_csr_if,

    // Outputs
    VX_commit_if.master     commit_if [`ISSUE_WIDTH],
    VX_warp_ctl_if.master   warp_ctl_if,

    // === 新增：DMA 接口 & warp stall 输出 ===
    VX_dma_bus_if.master    dma_bus_if,
    output wire [`NUM_WARPS-1:0] dma_warp_stall
);
    `UNUSED_SPARAM (INSTANCE_ID)

function automatic logic inst_sfu_is_dma(input logic [INST_OP_BITS-1:0] op_type);
    return (op_type == INST_OP_BITS'(INST_DMA_TRIGGER)
         || op_type == INST_OP_BITS'(INST_DMA_SET_DST)
         || op_type == INST_OP_BITS'(INST_DMA_SET_SRC)
         || op_type == INST_OP_BITS'(INST_DMA_SET_SIZE)
         || op_type == INST_OP_BITS'(INST_DMA_WAIT));
endfunction

    localparam BLOCK_SIZE   = 1;
    localparam NUM_LANES    = `NUM_SFU_LANES;

    // === 更新 PE 索引与数量（4.3 要求） ===
    localparam PE_IDX_WCTL  = 0;
    localparam PE_IDX_CSRS  = 1;
    localparam PE_IDX_DMA   = 2;   // 新增 DMA PE
    localparam PE_COUNT     = 3;   // 从 2 扩展为 3
    localparam PE_SEL_BITS  = `CLOG2(PE_COUNT);

    VX_execute_if #(
        .data_t (sfu_exe_t)
    ) per_block_execute_if[BLOCK_SIZE]();

    VX_result_if #(
        .data_t (sfu_res_t)
    ) per_block_result_if[BLOCK_SIZE]();

    VX_dispatch_unit #(
        .BLOCK_SIZE (BLOCK_SIZE),
        .NUM_LANES  (NUM_LANES),
        .OUT_BUF    (3)
    ) dispatch_unit (
        .clk        (clk),
        .reset      (reset),
        .dispatch_if(dispatch_if),
        .execute_if (per_block_execute_if)
    );

    VX_execute_if #(
        .data_t (sfu_exe_t)
    ) pe_execute_if[PE_COUNT]();

    VX_result_if#(
        .data_t (sfu_res_t)
    ) pe_result_if[PE_COUNT]();

    // === 修改后的 PE 选择逻辑：增加 DMA PE ===
    reg [PE_SEL_BITS-1:0] pe_select;
    always @(*) begin
        pe_select = PE_IDX_WCTL;
        if (inst_sfu_is_csr(per_block_execute_if[0].data.op_type)) begin
            pe_select = PE_IDX_CSRS;
        end else if (inst_sfu_is_dma(per_block_execute_if[0].data.op_type)) begin
            pe_select = PE_IDX_DMA;
        end
    end

    VX_pe_switch #(
        .PE_COUNT   (PE_COUNT),
        .NUM_LANES  (NUM_LANES),
        .ARBITER    ("R"),
        .REQ_OUT_BUF(0),
        .RSP_OUT_BUF(3)
    ) pe_switch (
        .clk           (clk),
        .reset         (reset),
        .pe_sel        (pe_select),
        .execute_in_if (per_block_execute_if[0]),
        .result_out_if (per_block_result_if[0]),
        .execute_out_if(pe_execute_if),
        .result_in_if  (pe_result_if)
    );

    VX_wctl_unit #(
        .INSTANCE_ID (`SFORMATF(("%s-wctl", INSTANCE_ID))),
        .NUM_LANES (NUM_LANES)
    ) wctl_unit (
        .clk        (clk),
        .reset      (reset),
        .execute_if (pe_execute_if[PE_IDX_WCTL]),
        .warp_ctl_if(warp_ctl_if),
        .result_if  (pe_result_if[PE_IDX_WCTL])
    );

    VX_csr_unit #(
        .INSTANCE_ID (`SFORMATF(("%s-csr", INSTANCE_ID))),
        .CORE_ID   (CORE_ID),
        .NUM_LANES (NUM_LANES)
    ) csr_unit (
        .clk            (clk),
        .reset          (reset),

        .base_dcrs      (base_dcrs),
        .execute_if     (pe_execute_if[PE_IDX_CSRS]),

    `ifdef PERF_ENABLE
        .sysmem_perf    (sysmem_perf),
        .pipeline_perf  (pipeline_perf),
    `endif

    `ifdef EXT_F_ENABLE
        .fpu_csr_if     (fpu_csr_if),
    `endif

        .sched_csr_if   (sched_csr_if),
        .commit_csr_if  (commit_csr_if),
        .result_if      (pe_result_if[PE_IDX_CSRS])
    );

    // === 新增：DMA Unit 实例（4.3 里的内容） ===
    VX_dma_unit #(
        .INSTANCE_ID (`SFORMATF(("%s-dma", INSTANCE_ID))),
        .NUM_LANES (NUM_LANES)
    ) dma_unit (
        .clk            (clk),
        .reset          (reset),
        .execute_if     (pe_execute_if[PE_IDX_DMA]),
        .result_if      (pe_result_if[PE_IDX_DMA]),
        .dma_bus_if     (dma_bus_if),
        .warp_stall_mask(dma_warp_stall)
    );

    VX_gather_unit #(
        .BLOCK_SIZE (BLOCK_SIZE),
        .NUM_LANES  (NUM_LANES),
        .OUT_BUF    (3)
    ) gather_unit (
        .clk       (clk),
        .reset     (reset),
        .result_if (per_block_result_if),
        .commit_if (commit_if)
    );

endmodule