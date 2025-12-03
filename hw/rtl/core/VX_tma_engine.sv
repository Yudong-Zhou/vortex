`include "VX_platform.vh"

module VX_tma_engine 
    import VX_gpu_pkg::*;
    import VX_tma_pkg::*;
#(
    parameter int CORE_REQS         = 16,   // 对应 VX_mem_scheduler.CORE_REQS
    parameter int WORD_SIZE         = 4,    // 目前假设和 elementSizeLog2 一致（可进一步扩展）
    parameter int ADDR_WIDTH        = 32,   // VX_mem_scheduler.ADDR_WIDTH = byte_addr >> log2(WORD_SIZE)
    parameter int TAG_WIDTH         = 8,    // 给 mem_scheduler 的 tag 宽度
    parameter int REQ_TAGW          = 8,    // 给“指令侧”的 tag 宽度（warp id / 指令 slot）
    parameter int SMEM_ADDR_WIDTH   = 16
) (
    input  logic                      clk,
    input  logic                      reset,

    // ============================================================
    // 1) 来自“指令执行单元”的 TMA 指令请求通道（指令控制）
    // ============================================================
    input  logic                      tma_req_valid, // 有新 TMA 指令
    input  tma_desc_t                 tma_req_desc,  // 这条 TMA 的完整描述符
    input  logic [REQ_TAGW-1:0]       tma_req_tag,   // 标记这条指令（返回时用）
    output logic                      tma_req_ready, // 引擎是否能接收新指令（空闲才为 1）

    // ============================================================
    // 2) 完成通道：TMA 引擎 → 指令流水线
    // ============================================================
    output logic                      tma_done_valid, // 有一条 TMA 完成
    output logic [REQ_TAGW-1:0]       tma_done_tag,   // 哪条（对应 tma_req_tag）
    input  logic                      tma_done_ready, // 上游是否接收完成事件

    // ============================================================
    // 3) shared memory 写接口（简化版）
    // ============================================================
    output logic                      smem_we,
    output logic [SMEM_ADDR_WIDTH-1:0] smem_addr,
    output logic [WORD_SIZE*8-1:0]    smem_wdata,
    input  logic                      smem_ready,

    // ============================================================
    // 4) 连接到 VX_mem_scheduler core 侧
    // ============================================================
    output logic                      core_req_valid,
    output logic                      core_req_rw,
    output logic [CORE_REQS-1:0]      core_req_mask,
    output logic [CORE_REQS-1:0][WORD_SIZE-1:0]    core_req_byteen,
    output logic [CORE_REQS-1:0][ADDR_WIDTH-1:0]   core_req_addr,
    output logic [CORE_REQS-1:0][WORD_SIZE*8-1:0]  core_req_data,
    output logic [TAG_WIDTH-1:0]      core_req_tag,
    input  logic                      core_req_ready,

    input  logic                      core_rsp_valid,
    input  logic [CORE_REQS-1:0]      core_rsp_mask,
    input  logic [CORE_REQS-1:0][WORD_SIZE*8-1:0]  core_rsp_data,
    input  logic [TAG_WIDTH-1:0]      core_rsp_tag,
    input  logic                      core_rsp_sop,
    input  logic                      core_rsp_eop,
    output logic                      core_rsp_ready
);

    localparam int WORD_WIDTH = WORD_SIZE * 8;

    // ============================================================
    // 内部状态：当前是否有 TMA 在跑 + 当前指令的 tag + 描述符
    // ============================================================

    typedef enum logic [2:0] {
        ST_IDLE,
        ST_SETUP,
        ST_ISSUE_REQ,
        ST_WAIT_RSP,
        ST_WRITE_SMEM,
        ST_DONE
    } state_e;

    state_e          state_r, state_n;

    // 当前正在执行的这条 TMA 的描述符和指令 tag
    tma_desc_t       desc_r, desc_n;
    logic [REQ_TAGW-1:0] instr_tag_r, instr_tag_n;

    // 元素大小（字节）
    logic [31:0] elem_bytes;
    assign elem_bytes = 32'(1) << desc_r.tensor.elementSizeLog2;

    // ============================================================
    // anchorGlobalAddr / anchorSharedAddr （对应专利 Fig.6 Setup）:contentReference[oaicite:0]{index=0}
    // ============================================================

    logic [TMA_ADDR_WIDTH-1:0] anchor_gaddr_r, anchor_gaddr_n;
    logic [31:0]               anchor_saddr_r, anchor_saddr_n;

    logic [TMA_ADDR_WIDTH-1:0] anchor_gaddr_comb;
    logic [31:0]               anchor_saddr_comb;

    always @(*) begin
        anchor_gaddr_comb = desc_r.tensor.baseGlobalAddr
                          + TMA_ADDR_WIDTH'(desc_r.access.boxCorner[0]) * desc_r.tensor.tensorStride[0]
                          + TMA_ADDR_WIDTH'(desc_r.access.boxCorner[1]) * desc_r.tensor.tensorStride[1];

        anchor_saddr_comb = desc_r.access.sharedBaseAddr
                          + desc_r.access.boxCorner[0] * desc_r.access.sharedStride[0]
                          + desc_r.access.boxCorner[1] * desc_r.access.sharedStride[1];
    end

    // ============================================================
    // box 内部的 2D 扫描（y, x）
    // ============================================================

    logic [15:0] box_y_r, box_y_n;  // 0 .. boxSize[0]-1
    logic [15:0] box_x_r, box_x_n;  // 0 .. boxSize[1]-1

    logic [$clog2(CORE_REQS+1)-1:0] batch_elems_r, batch_elems_n;

    // 当前 batch 的 global / shared 行基址
    logic [TMA_ADDR_WIDTH-1:0] g_row_base;
    logic [31:0]               s_row_base;

    assign g_row_base = anchor_gaddr_r
                      + TMA_ADDR_WIDTH'(box_y_r) * desc_r.tensor.tensorStride[0];

    assign s_row_base = anchor_saddr_r
                      + box_y_r * desc_r.access.sharedStride[0];

    // ============================================================
    // 一批 CORE_REQS 的请求：mask / byteen / addr
    // ============================================================

    logic [CORE_REQS-1:0]               req_mask_comb;
    logic [CORE_REQS-1:0][ADDR_WIDTH-1:0] req_addr_comb;
    logic [CORE_REQS-1:0][WORD_SIZE-1:0]  req_byteen_comb;
    logic [$clog2(CORE_REQS+1)-1:0]       batch_elems_comb;

    always @(*) begin
        req_mask_comb    = '0;
        req_byteen_comb  = '0;
        req_addr_comb    = '0;
        batch_elems_comb = '0;

        int remaining = (desc_r.access.boxSize[1] > box_x_r)
                      ? (desc_r.access.boxSize[1] - box_x_r)
                      : 0;
        int this_batch = (remaining > CORE_REQS) ? CORE_REQS : remaining;
        batch_elems_comb = this_batch[$clog2(CORE_REQS+1)-1:0];

        for (int i = 0; i < CORE_REQS; ++i) begin
            if (i < this_batch) begin
                req_mask_comb[i]   = 1'b1;
                req_byteen_comb[i] = {WORD_SIZE{1'b1}};
                // global byte addr = g_row_base + (box_x + i) * stride_x
                logic [TMA_ADDR_WIDTH-1:0] byte_addr;
                byte_addr = g_row_base
                          + TMA_ADDR_WIDTH'(box_x_r + i) * desc_r.tensor.tensorStride[1];

                req_addr_comb[i] = byte_addr[ADDR_WIDTH + $clog2(WORD_SIZE)-1 : $clog2(WORD_SIZE)];
            end
        end
    end

    // ============================================================
    // 响应缓存 + 写 shared memory
    // ============================================================

    logic [CORE_REQS-1:0]                  rsp_mask_r, rsp_mask_n;
    logic [CORE_REQS-1:0][WORD_WIDTH-1:0]  rsp_data_r, rsp_data_n;
    logic [$clog2(CORE_REQS):0]            lane_idx_r, lane_idx_n;

    logic [SMEM_ADDR_WIDTH-1:0] smem_addr_comb;
    logic [WORD_WIDTH-1:0]      smem_wdata_comb;
    logic                       smem_we_comb;

    always @(*) begin
        smem_we_comb   = 1'b0;
        smem_addr_comb = '0;
        smem_wdata_comb= '0;

        if (state_r == ST_WRITE_SMEM) begin
            if (lane_idx_r < CORE_REQS) begin
                if (rsp_mask_r[lane_idx_r]) begin
                    smem_we_comb    = 1'b1;
                    smem_wdata_comb = rsp_data_r[lane_idx_r];

                    smem_addr_comb = SMEM_ADDR_WIDTH'(
                        s_row_base + (box_x_r + lane_idx_r) * desc_r.access.sharedStride[1]
                    );
                end
            end
        end
    end

    // ============================================================
    // 指令侧的 ready / done 默认规则
    // ============================================================

    // 只有在 ST_IDLE 时才能接收新的指令
    wire engine_idle = (state_r == ST_IDLE);

    // 当 idle 时，req_ready=1，否则 0
    assign tma_req_ready = engine_idle;

    // done_valid/done_tag 在 ST_DONE 中拉高一次
    // 直到对方 tma_done_ready=1 才会真正返回到 IDLE

    // ============================================================
    // 主状态机：组合逻辑
    // ============================================================

    always @(*) begin
        state_n       = state_r;
        desc_n        = desc_r;
        instr_tag_n   = instr_tag_r;

        anchor_gaddr_n= anchor_gaddr_r;
        anchor_saddr_n= anchor_saddr_r;

        box_y_n       = box_y_r;
        box_x_n       = box_x_r;
        batch_elems_n = batch_elems_r;
        rsp_mask_n    = rsp_mask_r;
        rsp_data_n    = rsp_data_r;
        lane_idx_n    = lane_idx_r;

        // mem_scheduler core_req 默认拉低
        core_req_valid  = 1'b0;
        core_req_rw     = 1'b0;
        core_req_mask   = '0;
        core_req_byteen = '0;
        core_req_addr   = '0;
        core_req_data   = '0;
        core_req_tag    = '0;

        core_rsp_ready  = 1'b0;

        // shared mem 默认无写
        smem_we    = 1'b0;
        smem_addr  = smem_addr_comb;
        smem_wdata = smem_wdata_comb;

        // 指令侧完成通道默认无事件
        tma_done_valid = 1'b0;
        tma_done_tag   = instr_tag_r;

        case (state_r)

            // ----------------------------------------------------
            ST_IDLE: begin
                // 如果有新 TMA 指令，并且我们空闲，就接收
                if (tma_req_valid && tma_req_ready) begin
                    desc_n      = tma_req_desc;
                    instr_tag_n = tma_req_tag;

                    state_n     = ST_SETUP;
                end
            end

            // ----------------------------------------------------
            ST_SETUP: begin
                // 对应专利 Setup：计算 anchorGlobalAddr / sharedBase（根据 boxCorner）:contentReference[oaicite:1]{index=1}
                anchor_gaddr_n = anchor_gaddr_comb;
                anchor_saddr_n = anchor_saddr_comb;

                box_y_n       = '0;
                box_x_n       = '0;
                state_n       = ST_ISSUE_REQ;
            end

            // ----------------------------------------------------
            ST_ISSUE_REQ: begin
                // 整个 box 的所有行处理完，就 DONE
                if (box_y_r >= desc_r.access.boxSize[0]) begin
                    state_n = ST_DONE;
                end else begin
                    // 当前行的 x 走完了，换行
                    if (box_x_r >= desc_r.access.boxSize[1]) begin
                        box_y_n = box_y_r + 16'(1);
                        box_x_n = '0;
                    end else begin
                        // 正常根据 boxSize 发一批 req
                        if (desc_r.access.boxSize[0] == 0 || desc_r.access.boxSize[1] == 0) begin
                            state_n = ST_DONE;
                        end else begin
                            core_req_valid  = 1'b1;
                            core_req_rw     = 1'b0;
                            core_req_mask   = req_mask_comb;
                            core_req_byteen = req_byteen_comb;
                            core_req_addr   = req_addr_comb;
                            // core_req_tag 可以用 instr_tag_r 的低 TAG_WIDTH 位，或另外编码
                            core_req_tag    = TAG_WIDTH'(instr_tag_r);

                            if (core_req_ready) begin
                                batch_elems_n = batch_elems_comb;
                                state_n       = ST_WAIT_RSP;
                            end
                        end
                    end
                end
            end

            // ----------------------------------------------------
            ST_WAIT_RSP: begin
                if (core_rsp_valid) begin
                    core_rsp_ready = 1'b1;

                    rsp_mask_n = core_rsp_mask;
                    rsp_data_n = core_rsp_data;
                    lane_idx_n = '0;

                    state_n    = ST_WRITE_SMEM;
                end
            end

            // ----------------------------------------------------
            ST_WRITE_SMEM: begin
                smem_we = smem_we_comb && smem_ready;

                if (smem_we_comb && smem_ready) begin
                    lane_idx_n = lane_idx_r + 1;
                end

                if (lane_idx_r >= CORE_REQS-1) begin
                    box_x_n = box_x_r + CORE_REQS[15:0];
                    state_n = ST_ISSUE_REQ;
                end
            end

            // ----------------------------------------------------
            ST_DONE: begin
                // 向指令流水线报告：这条 TMA 指令完成了
                tma_done_valid = 1'b1;
                tma_done_tag   = instr_tag_r;

                if (tma_done_valid && tma_done_ready) begin
                    // 对方已经消费完成事件，可以回到 IDLE
                    state_n = ST_IDLE;
                end
            end

            default: begin
                state_n = ST_IDLE;
            end
        endcase
    end

    // ============================================================
    // 时序逻辑：状态寄存器
    // ============================================================

    always @(posedge clk) begin
        if (reset) begin
            state_r        <= ST_IDLE;
            desc_r         <= '0;
            instr_tag_r    <= '0;
            anchor_gaddr_r <= '0;
            anchor_saddr_r <= '0;
            box_y_r        <= '0;
            box_x_r        <= '0;
            batch_elems_r  <= '0;
            rsp_mask_r     <= '0;
            rsp_data_r     <= '0;
            lane_idx_r     <= '0;
        end else begin
            state_r        <= state_n;
            desc_r         <= desc_n;
            instr_tag_r    <= instr_tag_n;
            anchor_gaddr_r <= anchor_gaddr_n;
            anchor_saddr_r <= anchor_saddr_n;
            box_y_r        <= box_y_n;
            box_x_r        <= box_x_n;
            batch_elems_r  <= batch_elems_n;
            rsp_mask_r     <= rsp_mask_n;
            rsp_data_r     <= rsp_data_n;
            lane_idx_r     <= lane_idx_n;
        end
    end

`ifdef DBG_TRACE_MEM
    always @(posedge clk) begin
        if (tma_req_valid && tma_req_ready) begin
            `TRACE(1, ("%t: %s TMA instr start: tag=%0d, box=(%0d,%0d)@(%0d,%0d)\n",
                       $time, INSTANCE_ID, tma_req_tag,
                       tma_req_desc.access.boxSize[0],
                       tma_req_desc.access.boxSize[1],
                       tma_req_desc.access.boxCorner[0],
                       tma_req_desc.access.boxCorner[1]));
        end
        if (tma_done_valid && tma_done_ready) begin
            `TRACE(1, ("%t: %s TMA instr done: tag=%0d\n",
                       $time, INSTANCE_ID, tma_done_tag));
        end
    end
`endif

endmodule
