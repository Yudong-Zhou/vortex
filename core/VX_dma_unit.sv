`include "VX_define.vh"

module VX_dma_unit import VX_gpu_pkg::*; #(
    parameter `STRING INSTANCE_ID = "",
    parameter NUM_LANES          = 1
) (
    input  wire                      clk,
    input  wire                      reset,

    // Inputs
    VX_execute_if.slave              execute_if,

    // Outputs to SFU result arbiter
    VX_result_if.master              result_if,

    // DMA request/response bus to VX_dma_engine
    VX_dma_bus_if.master             dma_bus_if,

    // Warp stall mask output (per core)
    output wire [`NUM_WARPS-1:0]     warp_stall_mask
);

    `UNUSED_SPARAM (INSTANCE_ID)

    // ====== 常量与类型推导 ======

    // 与 CSR 单元保持一致的 PID / DATAW 计算方式
    localparam PID_BITS   = `CLOG2(`NUM_THREADS / NUM_LANES);
    localparam PID_WIDTH  = `UP(PID_BITS);
    localparam DATAW      = UUID_WIDTH
                          + NW_WIDTH              // wid
                          + NUM_LANES             // tmask
                          + PC_BITS
                          + 5              // rd
                          + 1                     // wb
                          + NUM_LANES * `XLEN     // data
                          + PID_WIDTH             // pid
                          + 1                     // sop
                          + 1;                    // eop

    // 从 dma_bus_if 推导地址宽度和 DMA ID 宽度
    typedef req_data_t dma_req_t;
    typedef rsp_data_t dma_rsp_t;

    localparam ADDR_WIDTH   = 32;
    localparam SIZE_WIDTH   = 16;
    localparam DMA_ID_WIDTH = 8;

    localparam NUM_WARPS    = `NUM_WARPS;

    // ====== Execute 数据拆包 ======

    wire [NUM_LANES-1:0][`XLEN-1:0] rs1_data;
    `UNUSED_VAR (execute_if.data.rs2_data)
    `UNUSED_VAR (execute_if.data.rs3_data)

    for (genvar i = 0; i < NUM_LANES; ++i) begin
        assign rs1_data[i] = execute_if.data.rs1_data[i];
    end

    wire [NW_WIDTH-1:0] wid = execute_if.data.wid;

    // ====== 指令类型判定 ======

    // 注意：op_type 在 decode 中已经被编码为 INST_OP_BITS 类型
    wire is_dma_trigger  = (execute_if.data.op_type == INST_OP_BITS'(INST_DMA_TRIGGER));
    wire is_dma_set_dst  = (execute_if.data.op_type == INST_OP_BITS'(INST_DMA_SET_DST));
    wire is_dma_set_src  = (execute_if.data.op_type == INST_OP_BITS'(INST_DMA_SET_SRC));
    wire is_dma_set_size = (execute_if.data.op_type == INST_OP_BITS'(INST_DMA_SET_SIZE));
    wire is_dma_wait     = (execute_if.data.op_type == INST_OP_BITS'(INST_DMA_WAIT));

    wire is_dma_inst     = is_dma_trigger
                        || is_dma_set_dst
                        || is_dma_set_src
                        || is_dma_set_size
                        || is_dma_wait;

    // 仅 lane0 作为 scalar 寄存器操作数
    wire [`XLEN-1:0] rs1_scalar = rs1_data[0];

    // DMA 方向
    wire dma_dir_to_lmem = (execute_if.data.op_args.dma.direction == 1'b1);

    // ====== 每 warp DMA 配置表 ======

    // 每个 warp 一份配置：dst/src/size + 当前 dma_id + pending 状态
    reg [ADDR_WIDTH-1:0]   warp_dst_addr   [NUM_WARPS];
    reg [ADDR_WIDTH-1:0]   warp_src_addr   [NUM_WARPS];
    reg [SIZE_WIDTH-1:0]   warp_size       [NUM_WARPS];
    reg [DMA_ID_WIDTH-1:0] warp_dma_id     [NUM_WARPS];
    reg                    warp_pending    [NUM_WARPS];  // 该 warp 是否有未完成 DMA

    // WAIT 对应的 warp 是否正在等待 / 等待的是哪个 dma_id
    reg                    warp_waiting    [NUM_WARPS];
    reg [DMA_ID_WIDTH-1:0] warp_wait_id    [NUM_WARPS];

    // Warp stall mask: 当前 warp 正在等待且对应 DMA 仍 pending
    reg [`NUM_WARPS-1:0] warp_stall_r;
    assign warp_stall_mask = warp_stall_r;

    // ====== DMA ID 分配器 ======

    reg [DMA_ID_WIDTH-1:0] next_dma_id;

    // 本条指令（如果是 TRIGGER）使用的 dma_id
    wire [DMA_ID_WIDTH-1:0] curr_dma_id = next_dma_id;

    // ====== 内部请求握手（execute ↔ dma_unit ↔ result_if） ======

    wire dma_req_valid = execute_if.valid && is_dma_inst;
    wire dma_req_ready;

    wire dma_fire = dma_req_valid && dma_req_ready;

    // 外部 execute_if ready
    assign execute_if.ready = dma_req_ready;

    // ====== 写回数据（给 result_if.data.data） ======

    // 对于大部分 DMA 指令，rd 不写回，decode 里已经设置 wb = 0
    // 仅 DMA_TRIGGER 需要写回 dma_id
    wire [NUM_LANES-1:0][`XLEN-1:0] dma_result_data;

    genvar li;
    generate
        for (li = 0; li < NUM_LANES; ++li) begin : g_dma_result_data
            assign dma_result_data[li] = (is_dma_trigger && dma_req_valid)
                ? { {(`XLEN-DMA_ID_WIDTH){1'b0}}, curr_dma_id }
                : {`XLEN{1'b0}};
        end
    endgenerate

    // ====== 与 result_if 之间的 elastic buffer ======

    VX_elastic_buffer #(
        .DATAW (DATAW),
        .SIZE  (2)
    ) rsp_buf (
        .clk       (clk),
        .reset     (reset),
        .valid_in  (dma_req_valid),
        .ready_in  (dma_req_ready),
        .data_in   ({
            execute_if.data.uuid,
            execute_if.data.wid,
            execute_if.data.tmask,
            execute_if.data.PC,
            execute_if.data.rd,
            execute_if.data.wb,
            dma_result_data,
            execute_if.data.pid,
            execute_if.data.sop,
            execute_if.data.eop
        }),
        .data_out  ({
            result_if.data.uuid,
            result_if.data.wid,
            result_if.data.tmask,
            result_if.data.PC,
            result_if.data.rd,
            result_if.data.wb,
            result_if.data.data,
            result_if.data.pid,
            result_if.data.sop,
            result_if.data.eop
        }),
        .valid_out (result_if.valid),
        .ready_out (result_if.ready)
    );

    // ====== 与 VX_dma_engine 的请求 / 响应握手 ======

    // 简化假设：VX_dma_engine 的 request queue 足够深，req_ready 总为 1，
    // 因此这里不会对 execute / result 做反压。
    // 如果后续你在 dma_engine 里实现了真正的 backpressure，可以把下面逻辑改成带 FIFO 的版本。

    // 默认 always ready 消费完成响应
    assign dma_bus_if.rsp_ready = 1'b1;

    // Request 只在 DMA_TRIGGER 指令 fire 时发出
    wire dma_trigger_fire  = dma_fire && is_dma_trigger;
    wire dma_set_dst_fire  = dma_fire && is_dma_set_dst;
    wire dma_set_src_fire  = dma_fire && is_dma_set_src;
    wire dma_set_size_fire = dma_fire && is_dma_set_size;
    wire dma_wait_fire     = dma_fire && is_dma_wait;

    // req_valid 直接由 trigger_fire 驱动（这假设 dma_bus_if.req_ready 始终为 1）
    assign dma_bus_if.req_valid               = dma_trigger_fire;
    assign dma_bus_if.req_data.src_addr       = warp_src_addr[wid];
    assign dma_bus_if.req_data.dst_addr       = warp_dst_addr[wid];
    assign dma_bus_if.req_data.size           = warp_size[wid];
    assign dma_bus_if.req_data.direction        = dma_dir_to_lmem;
    assign dma_bus_if.req_data.tag            = curr_dma_id;

    // 你如果希望更安全一点，也可以在这里加 assert：
    // `ASSERT (dma_trigger_fire -> dma_bus_if.req_ready)

    // ====== 主时序逻辑 ======

    integer w;

    always @(posedge clk) begin
        if (reset) begin
            next_dma_id   <= '0;

            for (w = 0; w < NUM_WARPS; ++w) begin
                warp_dst_addr[w] <= '0;
                warp_src_addr[w] <= '0;
                warp_size[w]     <= '0;
                warp_dma_id[w]   <= '0;
                warp_pending[w]  <= 1'b0;
                warp_waiting[w]  <= 1'b0;
                warp_wait_id[w]  <= '0;
            end

            warp_stall_r <= '0;
        end else begin
            // ==== 配置寄存器更新 ====

            if (dma_set_dst_fire) begin
                warp_dst_addr[wid] <= rs1_scalar[ADDR_WIDTH-1:0];
            end

            if (dma_set_src_fire) begin
                warp_src_addr[wid] <= rs1_scalar[ADDR_WIDTH-1:0];
            end

            if (dma_set_size_fire) begin
                warp_size[wid] <= rs1_scalar[SIZE_WIDTH-1:0];
            end

            // ==== TRIGGER：分配 DMA ID + 标记 pending + 发请求 ====

            if (dma_trigger_fire) begin
                // 当前指令使用 curr_dma_id
                warp_dma_id[wid]  <= curr_dma_id;
                warp_pending[wid] <= 1'b1;

                // DMA 触发后，分配器递增
                next_dma_id <= next_dma_id + 1'b1;
            end

            // ==== WAIT：记录期望等待的 dma_id，并拉高 warp_waiting ====

            if (dma_wait_fire) begin
                // rs1 = dma_id，可能和 warp_dma_id 不同，允许软件指定旧 id
                warp_wait_id[wid] <= rs1_scalar[DMA_ID_WIDTH-1:0];
                // 只有在当前仍有 pending 时才真正等待；否则 WAIT 退化为 NOP
                if (warp_pending[wid]) begin
                    warp_waiting[wid] <= 1'b1;
                end
            end

            // ==== 从 VX_dma_engine 接收完成通知 ====

            if (dma_bus_if.rsp_valid && dma_bus_if.rsp_ready) begin
                dma_rsp_t rsp = dma_bus_if.rsp_data;
                // 简单起见：假设每个 dma_id 只对应一个 warp（由 dma_unit 分配器保证）
                for (w = 0; w < NUM_WARPS; ++w) begin
                    if (warp_pending[w] && (warp_dma_id[w] == rsp.tag)) begin
                        warp_pending[w] <= 1'b0;
                        // 如果某个 warp 正在等待这个 id，则清除 waiting
                        if (warp_waiting[w] && (warp_wait_id[w] == rsp.tag)) begin
                            warp_waiting[w] <= 1'b0;
                        end
                    end
                end
            end

            // ==== 更新 warp_stall_mask ====
            // 只有同时处于 waiting 且对应 DMA 仍 pending 时才真正 stall

            for (w = 0; w < NUM_WARPS; ++w) begin
                warp_stall_r[w] <= warp_waiting[w] && warp_pending[w];
            end
        end
    end

endmodule