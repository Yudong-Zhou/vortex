// ********************************************************************************
// Simple TMA unit skeleton, Vortex-style
// 和 VX_lsu_unit 同样风格：
//  - dispatch_if -> per_block_execute_if
//  - per_block_execute_if[block] -> VX_tma_slice
//  - VX_gather_unit 合并 commit
// ********************************************************************************

`include "VX_define.vh"

module VX_tma_unit import VX_gpu_pkg::*; #(
    parameter `STRING INSTANCE_ID = ""
) (
    `SCOPE_IO_DECL

    input  wire             clk,
    input  wire             reset,

    // 来自 execute 阶段，对应 EX_TMA 那一段 dispatch
    VX_dispatch_if.slave    dispatch_if [`ISSUE_WIDTH],

    // 回写到 commit 阶段，对应 EX_TMA 那一段 commit
    VX_commit_if.master     commit_if   [`ISSUE_WIDTH],

    // 和数据 cache 的本地内存接口
    // 这里直接沿用 lsu 的接口形式，每个 block 一条 VX_lsu_mem_if
    VX_lsu_mem_if.master    tma_mem_if  [`NUM_LSU_BLOCKS]
);

    localparam BLOCK_SIZE = `NUM_LSU_BLOCKS;
    localparam NUM_LANES  = `NUM_LSU_LANES;

`ifdef SCOPE
    // 跟 VX_lsu_unit 一样，让每个 slice 能单独打 SCOPE
    `SCOPE_IO_SWITCH (BLOCK_SIZE);
`endif

    // 每个 block 有一个 execute_if（warp+lane 粒度的执行信息）
    VX_execute_if #(
        .NUM_LANES (NUM_LANES)
    ) per_block_execute_if [BLOCK_SIZE] ();

    // dispatch_unit：把来自 EX_TMA 那组 dispatch_if
    // 均匀分发到 per_block_execute_if[0 .. BLOCK_SIZE-1]
    VX_dispatch_unit #(
        .BLOCK_SIZE (BLOCK_SIZE),
        .NUM_LANES  (NUM_LANES),
        .OUT_BUF    (1)
    ) dispatch_unit (
        .clk         (clk),
        .reset       (reset),
        .dispatch_if (dispatch_if),
        .execute_if  (per_block_execute_if)
    );

    // 每个 block 一个 commit_if，后面用 VX_gather_unit 合并
    VX_commit_if #(
        .NUM_LANES (NUM_LANES)
    ) per_block_commit_if [BLOCK_SIZE] ();

    // 为每个 LSU block 实例化一个 TMA slice
    // 你之后在 VX_tma_slice 里接 TMA 引擎和 VX_mem_scheduler
    for (genvar block_idx = 0; block_idx < BLOCK_SIZE; ++block_idx) begin : tma_slices

        `RESET_RELAY (slice_reset, reset);

        VX_tma_slice #(
            .INSTANCE_ID (`SFORMATF(("%s-tma%0d", INSTANCE_ID, block_idx)))
        ) tma_slice (
            `SCOPE_IO_BIND  (block_idx)

            .clk        (slice_reset ? 1'b0 : clk), // 或者直接 .clk(clk)，这里仿照 lsu 写法
            .reset      (slice_reset),

            // 来自该 block 的执行信息（已经由 dispatch_unit 分发）
            .execute_if (per_block_execute_if[block_idx]),

            // 该 block 对应的 commit_if（写回寄存器/完成 TMA 指令）
            .commit_if  (per_block_commit_if[block_idx]),

            // 对应这个 block 的本地内存接口
            .tma_mem_if (tma_mem_if[block_idx])
        );
    end

    // gather_unit：把各 block 的 commit 合并成 EX_TMA 那一组 commit_if
    VX_gather_unit #(
        .BLOCK_SIZE (BLOCK_SIZE),
        .NUM_LANES  (NUM_LANES),
        .OUT_BUF    (3)
    ) gather_unit (
        .clk           (clk),
        .reset         (reset),
        .commit_in_if  (per_block_commit_if),
        .commit_out_if (commit_if)
    );

endmodule
