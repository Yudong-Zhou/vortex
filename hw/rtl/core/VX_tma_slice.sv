`include "VX_define.vh"

module VX_tma_slice import VX_gpu_pkg::*; #(
    parameter `STRING INSTANCE_ID = ""
) (
    `SCOPE_IO_DECL

    input  wire             clk,
    input  wire             reset,

    // 来自 EX_TMA 的指令
    VX_execute_if.slave     execute_if,

    // 给 commit 的结果（语义类似 LSU，只是不写寄存器）
    VX_result_if.master     result_if,

    // 和 mem_unit / dcache 的接口
    VX_lsu_mem_if.master    lsu_mem_if
);
    localparam NUM_LANES     = `NUM_LSU_LANES;
    localparam PID_BITS      = `CLOG2(`NUM_THREADS / NUM_LANES);
    localparam PID_WIDTH     = `UP(PID_BITS);
    localparam LSUQ_SIZEW    = `LOG2UP(`LSUQ_IN_SIZE);
    localparam REQ_ASHIFT    = `CLOG2(LSU_WORD_SIZE);
    localparam MEM_ASHIFT    = `CLOG2(`MEM_BLOCK_SIZE);
    localparam MEM_ADDRW     = `MEM_ADDR_WIDTH - MEM_ASHIFT;

    // 简化版 tag：
    // tag_id = wid + PC + pid + pkt_addr
    localparam TAG_ID_WIDTH  = NW_WIDTH + PC_BITS + PID_WIDTH + LSUQ_SIZEW;
    localparam TAG_WIDTH     = UUID_WIDTH + TAG_ID_WIDTH;

    localparam RSP_ARB_DATAW = UUID_WIDTH + NW_WIDTH + NUM_LANES
                             + PC_BITS + 1 + NUM_REGS_BITS
                             + NUM_LANES * `XLEN + PID_WIDTH + 1 + 1;

    VX_result_if #(
        .data_t (lsu_res_t)
    ) result_rsp_if();

    VX_result_if #(
        .data_t (lsu_res_t)
    ) result_no_rsp_if();

    `UNUSED_VAR (execute_if.data.rs3_data)

    // -------------------------------
    // 地址计算（暂时直接 rs1 + offset）
    // -------------------------------
    wire [NUM_LANES-1:0][`XLEN-1:0] full_addr;
    for (genvar i = 0; i < NUM_LANES; ++i) begin : g_full_addr
        assign full_addr[i] = execute_if.data.rs1_data[i]
                            + `SEXT(`XLEN, execute_if.data.op_args.lsu.offset);
    end

    // -------------------------------
    // 地址类型判断（IO / LMEM）
    // -------------------------------
    wire [NUM_LANES-1:0][MEM_FLAGS_WIDTH-1:0] mem_req_flags;
    for (genvar i = 0; i < NUM_LANES; ++i) begin : g_mem_req_flags
        wire [MEM_ADDRW-1:0] block_addr = full_addr[i][MEM_ASHIFT +: MEM_ADDRW];

        // IO 区域
        wire [MEM_ADDRW-1:0] io_addr_start =
            MEM_ADDRW'(`XLEN'(`IO_BASE_ADDR) >> MEM_ASHIFT);
        wire [MEM_ADDRW-1:0] io_addr_end =
            MEM_ADDRW'(`XLEN'(`IO_END_ADDR) >> MEM_ASHIFT);

        assign mem_req_flags[i][MEM_REQ_FLAG_FLUSH] = 1'b0; // 这里不做 fence
        assign mem_req_flags[i][MEM_REQ_FLAG_IO] =
            (block_addr >= io_addr_start) && (block_addr < io_addr_end);

    `ifdef LMEM_ENABLE
        // local memory 区域
        wire [MEM_ADDRW-1:0] lmem_addr_start =
            MEM_ADDRW'(`XLEN'(`LMEM_BASE_ADDR) >> MEM_ASHIFT);
        wire [MEM_ADDRW-1:0] lmem_addr_end =
            MEM_ADDRW'((`XLEN'(`LMEM_BASE_ADDR) + `XLEN'(1 << `LMEM_LOG_SIZE)) >> MEM_ASHIFT);

        assign mem_req_flags[i][MEM_REQ_FLAG_LOCAL] =
            (block_addr >= lmem_addr_start) && (block_addr < lmem_addr_end);
    `endif
    end

    // -------------------------------
    // scheduler 前端的 req/rsp 信号
    // -------------------------------
    wire                            mem_req_valid;
    wire [NUM_LANES-1:0]            mem_req_mask;
    wire                            mem_req_rw;
    wire [NUM_LANES-1:0][LSU_ADDR_WIDTH-1:0] mem_req_addr;
    wire [NUM_LANES-1:0][LSU_WORD_SIZE-1:0]  mem_req_byteen;
    wire [NUM_LANES-1:0][LSU_WORD_SIZE*8-1:0] mem_req_data;
    wire [TAG_WIDTH-1:0]            mem_req_tag;
    wire                            mem_req_ready;

    wire                            mem_rsp_valid;
    wire [NUM_LANES-1:0]            mem_rsp_mask;
    wire [NUM_LANES-1:0][LSU_WORD_SIZE*8-1:0] mem_rsp_data;
    wire [TAG_WIDTH-1:0]            mem_rsp_tag;
    wire                            mem_rsp_sop;
    wire                            mem_rsp_eop;
    wire                            mem_rsp_ready;

    wire mem_req_fire = mem_req_valid && mem_req_ready;
    wire mem_rsp_fire = mem_rsp_valid && mem_rsp_ready;

    // 这里我们和 LSU 不一样：不区分 fence / store 无返回 / load 有返回，
    // 先简单认为：所有 TMA 指令都要等内存读完才算完成。
    assign mem_req_valid   = execute_if.valid;
    assign execute_if.ready= mem_req_ready;

    // 每 lane 是否参与这次 TMA（延用 tmask）
    assign mem_req_mask    = execute_if.data.tmask;

    // 先假设 TMA 是 global → local 读，不写回：
    assign mem_req_rw      = 1'b0;

    // 地址拆分：和 LSU 一样，低 REQ_ASHIFT 位是对齐，之后是 word addr
    wire [NUM_LANES-1:0][REQ_ASHIFT-1:0] req_align;
    for (genvar i = 0; i < NUM_LANES; ++i) begin : g_mem_req_addr
        assign req_align[i]  = full_addr[i][REQ_ASHIFT-1:0];
        assign mem_req_addr[i] = full_addr[i][`MEM_ADDR_WIDTH-1:REQ_ASHIFT];
    end

    // byteenable：TMA 版本先简化为整 word 全写/全读
    for (genvar i = 0; i < NUM_LANES; ++i) begin : g_mem_req_byteen
        assign mem_req_byteen[i] = {LSU_WORD_SIZE{1'b1}};
    end

    // store data：TMA 先只做读，这里填 0 即可（对读没影响）
    for (genvar i = 0; i < NUM_LANES; ++i) begin : g_mem_req_data
        assign mem_req_data[i] = '0;
    end

    // -------------------------------
    // multi-packet 处理（沿用 LSU 的 pkt allocator 形式，但简化）
    // 这里只考虑“多条 TMA 请求 = 多个 pkt”的场景，
    // 先支持 multi-packet，但不做复杂 SOP/EOP 逻辑。
    // -------------------------------
    wire [LSUQ_SIZEW-1:0] pkt_waddr, pkt_raddr;

    if (PID_BITS != 0) begin : g_pid
        reg [`LSUQ_IN_SIZE-1:0] pkt_valid;

        wire mem_req_rd_fire      = mem_req_fire && ~mem_req_rw;
        wire mem_req_rd_eop_fire  = mem_req_rd_fire && execute_if.data.eop;
        wire mem_rsp_eop_fire     = mem_rsp_fire  && mem_rsp_eop;

        wire full;

        VX_allocator #(
            .SIZE (`LSUQ_IN_SIZE)
        ) pkt_allocator (
            .clk         (clk),
            .reset       (reset),
            .acquire_en  (mem_req_rd_eop_fire),
            .acquire_addr(pkt_waddr),
            .release_en  (mem_rsp_eop_fire),
            .release_addr(pkt_raddr),
            `UNUSED_PIN  (empty),
            .full        (full)
        );

        always @(posedge clk) begin
            if (reset) begin
                pkt_valid <= '0;
            end else begin
                if (mem_req_rd_eop_fire)
                    pkt_valid[pkt_waddr] <= 1'b1;
                if (mem_rsp_eop_fire)
                    pkt_valid[pkt_raddr] <= 1'b0;
            end
        end

        // 对 TMA 来说，我们只在 pkt 的 EOP 时认为一条 DMA 结束
        assign mem_rsp_sop = 1'b1;          // 简化：不区分 SOP
        assign mem_rsp_eop = pkt_valid[pkt_raddr];

        `RUNTIME_ASSERT(~(mem_req_rd_fire && full),
            ("%t: TMA pkt allocator full!", $time))
    end else begin : g_no_pid
        assign pkt_waddr  = '0;
        assign pkt_raddr  = '0;
        assign mem_rsp_sop = 1'b1;
        assign mem_rsp_eop = 1'b1;
    end

    // -------------------------------
    // 打包 tag：uuid + (wid, PC, pid, pkt_addr)
    // -------------------------------
    assign mem_req_tag = {
        execute_if.data.uuid,
        execute_if.data.wid,
        execute_if.data.PC,
        execute_if.data.pid,
        pkt_waddr
    };

    // -------------------------------
    // 调用 VX_mem_scheduler
    // -------------------------------
    wire                                    lsu_mem_req_valid;
    wire                                    lsu_mem_req_rw;
    wire [NUM_LANES-1:0]                    lsu_mem_req_mask;
    wire [NUM_LANES-1:0][LSU_WORD_SIZE-1:0] lsu_mem_req_byteen;
    wire [NUM_LANES-1:0][LSU_ADDR_WIDTH-1:0] lsu_mem_req_addr;
    wire [NUM_LANES-1:0][MEM_FLAGS_WIDTH-1:0] lsu_mem_req_flags;
    wire [NUM_LANES-1:0][(LSU_WORD_SIZE*8)-1:0] lsu_mem_req_data;
    wire [LSU_TAG_WIDTH-1:0]                lsu_mem_req_tag;
    wire                                    lsu_mem_req_ready;

    wire                                    lsu_mem_rsp_valid;
    wire [NUM_LANES-1:0]                    lsu_mem_rsp_mask;
    wire [NUM_LANES-1:0][(LSU_WORD_SIZE*8)-1:0] lsu_mem_rsp_data;
    wire [LSU_TAG_WIDTH-1:0]                lsu_mem_rsp_tag;
    wire                                    lsu_mem_rsp_ready;

    VX_mem_scheduler #(
        .INSTANCE_ID   (`SFORMATF(("%s-tma-memsched", INSTANCE_ID))),
        .CORE_REQS     (NUM_LANES),
        .MEM_CHANNELS  (NUM_LANES),
        .WORD_SIZE     (LSU_WORD_SIZE),
        .LINE_SIZE     (LSU_WORD_SIZE),
        .ADDR_WIDTH    (LSU_ADDR_WIDTH),
        .FLAGS_WIDTH   (MEM_FLAGS_WIDTH),
        .TAG_WIDTH     (TAG_WIDTH),
        .CORE_QUEUE_SIZE (`LSUQ_IN_SIZE),
        .MEM_QUEUE_SIZE  (`LSUQ_OUT_SIZE),
        .UUID_WIDTH    (UUID_WIDTH),
        .RSP_PARTIAL   (1),
        .MEM_OUT_BUF   (0),
        .CORE_OUT_BUF  (0)
    ) mem_scheduler (
        .clk            (clk),
        .reset          (reset),

        // Core side request
        .core_req_valid (mem_req_valid),
        .core_req_rw    (mem_req_rw),
        .core_req_mask  (mem_req_mask),
        .core_req_byteen(mem_req_byteen),
        .core_req_addr  (mem_req_addr),
        .core_req_flags (mem_req_flags),
        .core_req_data  (mem_req_data),
        .core_req_tag   (mem_req_tag),
        .core_req_ready (mem_req_ready),

        // 这些队列状态信号先不使用
        `UNUSED_PIN     (req_queue_empty),
        `UNUSED_PIN     (req_queue_rw_notify),

        // Core side response
        .core_rsp_valid (mem_rsp_valid),
        .core_rsp_mask  (mem_rsp_mask),
        .core_rsp_data  (mem_rsp_data),
        .core_rsp_tag   (mem_rsp_tag),
        .core_rsp_sop   (/*unused*/),
        .core_rsp_eop   (mem_rsp_eop),
        .core_rsp_ready (mem_rsp_ready),

        // Memory side
        .mem_req_valid  (lsu_mem_req_valid),
        .mem_req_rw     (lsu_mem_req_rw),
        .mem_req_mask   (lsu_mem_req_mask),
        .mem_req_byteen (lsu_mem_req_byteen),
        .mem_req_addr   (lsu_mem_req_addr),
        .mem_req_flags  (lsu_mem_req_flags),
        .mem_req_data   (lsu_mem_req_data),
        .mem_req_tag    (lsu_mem_req_tag),
        .mem_req_ready  (lsu_mem_req_ready),

        .mem_rsp_valid  (lsu_mem_rsp_valid),
        .mem_rsp_mask   (lsu_mem_rsp_mask),
        .mem_rsp_data   (lsu_mem_rsp_data),
        .mem_rsp_tag    (lsu_mem_rsp_tag),
        .mem_rsp_ready  (lsu_mem_rsp_ready)
    );

    // 接到 mem_unit / dcache
    assign lsu_mem_if.req_valid    = lsu_mem_req_valid;
    assign lsu_mem_if.req_data.mask= lsu_mem_req_mask;
    assign lsu_mem_if.req_data.rw  = lsu_mem_req_rw;
    assign lsu_mem_if.req_data.byteen = lsu_mem_req_byteen;
    assign lsu_mem_if.req_data.addr   = lsu_mem_req_addr;
    assign lsu_mem_if.req_data.flags  = lsu_mem_req_flags;
    assign lsu_mem_if.req_data.data   = lsu_mem_req_data;
    assign lsu_mem_if.req_data.tag    = lsu_mem_req_tag;
    assign lsu_mem_req_ready          = lsu_mem_if.req_ready;

    assign lsu_mem_rsp_valid          = lsu_mem_if.rsp_valid;
    assign lsu_mem_rsp_mask           = lsu_mem_if.rsp_data.mask;
    assign lsu_mem_rsp_data           = lsu_mem_if.rsp_data.data;
    assign lsu_mem_rsp_tag            = lsu_mem_if.rsp_data.tag;
    assign lsu_mem_if.rsp_ready       = lsu_mem_rsp_ready;

    // -------------------------------
    // 解 tag：uuid, wid, PC, pid, pkt_addr
    // -------------------------------
    wire [UUID_WIDTH-1:0]   rsp_uuid;
    wire [NW_WIDTH-1:0]     rsp_wid;
    wire [PC_BITS-1:0]      rsp_pc;
    wire [PID_WIDTH-1:0]    rsp_pid;
    wire [LSUQ_SIZEW-1:0]   rsp_pkt_addr;

    assign {
        rsp_uuid,
        rsp_wid,
        rsp_pc,
        rsp_pid,
        rsp_pkt_addr
    } = mem_rsp_tag;

    // -------------------------------
    // 结果：TMA 不写寄存器，只是告诉 commit：
    // “这条 TMA 指令已经在内存侧完成了”
    // -------------------------------

    // 我们只在每个 TMA “packet”的 EOP 时输出一次 result
    assign mem_rsp_ready              = result_rsp_if.ready;

    assign result_rsp_if.valid        = mem_rsp_valid && mem_rsp_eop;

    assign result_rsp_if.data.uuid    = rsp_uuid;
    assign result_rsp_if.data.wid     = rsp_wid;
    assign result_rsp_if.data.tmask   = mem_rsp_mask;
    assign result_rsp_if.data.PC      = rsp_pc;

    assign result_rsp_if.data.wb      = 1'b0;
    assign result_rsp_if.data.rd      = '0;
    assign result_rsp_if.data.data    = '0;

    assign result_rsp_if.data.pid     = rsp_pid;
    assign result_rsp_if.data.sop     = 1'b1;
    assign result_rsp_if.data.eop     = 1'b1;

    // 这里 TMA 不存在 “no response” 的场景，no_rsp_buf 直接打空
    assign result_no_rsp_if.valid     = 1'b0;
    assign result_no_rsp_if.data      = '0;

    // result arb：沿用 LSU 的形式，但只有一个有效输入
    VX_stream_arb #(
        .NUM_INPUTS (2),
        .DATAW      (RSP_ARB_DATAW),
        .ARBITER    ("P"),
        .OUT_BUF    (3)
    ) rsp_arb (
        .clk        (clk),
        .reset      (reset),
        .valid_in   ({result_no_rsp_if.valid, result_rsp_if.valid}),
        .ready_in   ({result_no_rsp_if.ready, result_rsp_if.ready}),
        .data_in    ({result_no_rsp_if.data,  result_rsp_if.data}),
        .data_out   (result_if.data),
        .valid_out  (result_if.valid),
        .ready_out  (result_if.ready),
        `UNUSED_PIN (sel_out)
    );

`ifdef DBG_TRACE_MEM
    always @(posedge clk) begin
        if (mem_req_fire) begin
            `TRACE(2, ("%t: %s TMA Req: wid=%0d, PC=0x%0h, tmask=%b, addr=",
                $time, INSTANCE_ID, execute_if.data.wid, to_fullPC(execute_if.data.PC), mem_req_mask))
            `TRACE_ARRAY1D(2, "0x%h", full_addr, NUM_LANES)
            `TRACE(2, (", flags="))
            `TRACE_ARRAY1D(2, "%b", mem_req_flags, NUM_LANES)
            `TRACE(2, (", tag=0x%0h (#%0d)\n", mem_req_tag, execute_if.data.uuid))
        end
        if (mem_rsp_fire && mem_rsp_eop) begin
            `TRACE(2, ("%t: %s TMA Rsp(EOP): wid=%0d, PC=0x%0h, tmask=%b, tag=0x%0h (#%0d)\n",
                $time, INSTANCE_ID, rsp_wid, to_fullPC(rsp_pc), mem_rsp_mask, mem_rsp_tag, rsp_uuid))
        end
    end
`endif

endmodule
