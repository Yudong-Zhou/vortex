`include "VX_define.vh"

module VX_dma_engine #(
    parameter `STRING INSTANCE_ID = "",
    parameter NUM_CHANNELS    = 4,
    parameter QUEUE_SIZE      = 8,
    parameter BANDWIDTH       = 64,      // bytes per cycle
    parameter STARTUP_LATENCY = 10       // cycles
) (
    input  wire                 clk,
    input  wire                 reset,

    // DMA 请求接口
    VX_dma_bus_if.slave         dma_req_if,

    // 本地内存接口
    VX_mem_bus_if.master        lmem_bus_if,

    // 全局内存接口 (NC bypass)
    VX_mem_bus_if.master        gmem_bus_if
);
    `UNUSED_SPARAM (INSTANCE_ID)

    // -------------------------------------------------------------
    // 一些基本类型与宽度
    // -------------------------------------------------------------

    typedef req_data_t  dma_req_t;
    typedef rsp_data_t  dma_rsp_t;

    localparam ADDR_WIDTH = 32;
    localparam SIZE_WIDTH = 16;
    localparam TAG_WIDTH  = 8;

    // 请求队列中存的东西 = 原始 dma_req_t
    localparam REQ_Q_DATAW = $bits(dma_req_t);

    // 完成队列只需要 tag
    localparam RSP_Q_DATAW = TAG_WIDTH;

    // Channel FSM 状态
    typedef enum logic [1:0] {
        CH_IDLE     = 2'b00,
        CH_STARTUP  = 2'b01,
        CH_TRANSFER = 2'b10,
        CH_DONE     = 2'b11
    } ch_state_e;

    // 每个 channel 内部保存的请求信息
    typedef struct packed {
        logic [ADDR_WIDTH-1:0] src_addr;
        logic [ADDR_WIDTH-1:0] dst_addr;
        logic [SIZE_WIDTH-1:0] size;
        logic                  direction;       // 0=G2L, 1=L2G
        logic [TAG_WIDTH-1:0]  tag;
    } ch_req_info_t;

    // -------------------------------------------------------------
    // Request Queue：接收来自 VX_dma_unit 的请求
    // -------------------------------------------------------------

    dma_req_t  req_q_data;
    logic      req_q_empty, req_q_full;
    logic      req_q_push, req_q_pop;

    // dma_req_if.req_ready 由队列满信号驱动
    assign dma_req_if.req_ready = ~req_q_full;

    // 入队：上游 valid & ready
    assign req_q_push = dma_req_if.req_valid && dma_req_if.req_ready;

    VX_fifo_queue #(
        .DATAW (REQ_Q_DATAW),
        .DEPTH (QUEUE_SIZE)
    ) req_queue (
        .clk      (clk),
        .reset    (reset),
        .push     (req_q_push),
        .pop      (req_q_pop),
        .data_in  (dma_req_if.req_data),
        .data_out (req_q_data),
        .empty    (req_q_empty),
        .full     (req_q_full)
    );

    // -------------------------------------------------------------
    // 多 Channel 状态机
    // -------------------------------------------------------------

    // 每个 channel 的状态/计数器/请求信息
    ch_state_e             ch_state     [NUM_CHANNELS];
    ch_req_info_t          ch_info      [NUM_CHANNELS];
    logic [SIZE_WIDTH-1:0] ch_bytes_rem [NUM_CHANNELS];
    logic [15:0]           ch_startup_cnt [NUM_CHANNELS];

    // Round-robin 分发用指针
    logic [$clog2(NUM_CHANNELS)-1:0] rr_ptr;

    // -------------------------------------------------------------
    // Completion Queue：收集完成的 DMA tag，返回给 dma_req_if.rsp
    // -------------------------------------------------------------

    logic [RSP_Q_DATAW-1:0] rsp_q_data;
    logic                   rsp_q_empty, rsp_q_full;
    logic                   rsp_q_push, rsp_q_pop;

    VX_fifo_queue #(
        .DATAW (RSP_Q_DATAW),
        .DEPTH (QUEUE_SIZE)
    ) rsp_queue (
        .clk      (clk),
        .reset    (reset),
        .push     (rsp_q_push),
        .pop      (rsp_q_pop),
        .data_in  (rsp_q_data),
        .data_out (rsp_q_data),
        .empty    (rsp_q_empty),
        .full     (rsp_q_full)
    );

    // 输出给 dma_req_if.rsp
    assign dma_req_if.rsp_valid       = ~rsp_q_empty;
    assign dma_req_if.rsp_data.tag    = rsp_q_data[TAG_WIDTH-1:0];
    assign rsp_q_pop                  = dma_req_if.rsp_valid && dma_req_if.rsp_ready;

    // -------------------------------------------------------------
    // Channel Dispatcher：从 Request Queue 分发请求到空闲 Channel
    // -------------------------------------------------------------

    // combinational：本周期是否有空闲 channel 可以分配
    logic                               dispatch_fire;
    logic [$clog2(NUM_CHANNELS)-1:0]    dispatch_ch_id;

    integer i;
    always @* begin
        dispatch_fire   = 1'b0;
        dispatch_ch_id  = '0;

        if (!req_q_empty) begin
            // 简单 round-robin：从 rr_ptr 开始找第一个空闲 channel
            for (i = 0; i < NUM_CHANNELS; ++i) begin
                int idx = (rr_ptr + i) % NUM_CHANNELS;
                if (ch_state[idx] == CH_IDLE) begin
                    dispatch_fire  = 1'b1;
                    dispatch_ch_id = idx[$clog2(NUM_CHANNELS)-1:0];
                    break;
                end
            end
        end
    end

    // 当实际分发一个请求时，Request Queue 出队
    assign req_q_pop = dispatch_fire;

    // -------------------------------------------------------------
    // Channel 完成 -> Completion Queue
    // 只允许一个 channel 在一个周期内 push 完成
    // -------------------------------------------------------------

    logic                           any_done;
    logic [$clog2(NUM_CHANNELS)-1:0] done_ch_id;
    dma_rsp_t                       done_rsp_data;

    always @* begin
        any_done    = 1'b0;
        done_ch_id  = '0;
        done_rsp_data.tag = '0;

        // 这里用简单优先级（小 index 优先），你后面可以换成 VX_stream_arb
        for (i = 0; i < NUM_CHANNELS; ++i) begin
            if (!any_done && (ch_state[i] == CH_DONE)) begin
                any_done           = 1'b1;
                done_ch_id         = i[$clog2(NUM_CHANNELS)-1:0];
                done_rsp_data.tag  = ch_info[i].tag;
            end
        end
    end

    // 完成队列写入条件：有 DONE channel 且队列未满
    assign rsp_q_push = any_done && ~rsp_q_full;
    assign rsp_q_data = done_rsp_data.tag;

    // -------------------------------------------------------------
    // Memory 接口（当前版本先占位，后续再接 VX_mem_bus_if 真正读写）
    // -------------------------------------------------------------

    // 目前先不驱动任何 DMA 读写，只做“内部计数递减”模拟带宽，
    // 真正与 lmem/gmem 交互的逻辑后续再通过 VX_stream_arb + VX_mem_bus_if 接入。

    // 防止未使用警告（根据你本地宏可以替换）
    `UNUSED_VAR (lmem_bus_if.req_valid)
    `UNUSED_VAR (lmem_bus_if.req_data)
    `UNUSED_VAR (lmem_bus_if.rsp_ready)
    `UNUSED_VAR (gmem_bus_if.req_valid)
    `UNUSED_VAR (gmem_bus_if.req_data)
    `UNUSED_VAR (gmem_bus_if.rsp_ready)

    // -------------------------------------------------------------
    // 主时序逻辑：Channel FSM + 分发 + 完成
    // -------------------------------------------------------------

    integer c;

    always @(posedge clk) begin
        if (reset) begin
            rr_ptr <= '0;

            for (c = 0; c < NUM_CHANNELS; ++c) begin
                ch_state[c]       <= CH_IDLE;
                ch_info[c]        <= '0;
                ch_bytes_rem[c]   <= '0;
                ch_startup_cnt[c] <= '0;
            end
        end else begin
            // -----------------------------
            // 1) 分发：Request Queue -> 空闲 Channel
            // -----------------------------
            if (dispatch_fire) begin
                int cid = dispatch_ch_id;
                // 将当前队列头的请求塞给该 channel
                ch_info[cid].src_addr   <= req_q_data.src_addr;
                ch_info[cid].dst_addr   <= req_q_data.dst_addr;
                ch_info[cid].size       <= req_q_data.size;
                ch_info[cid].direction  <= req_q_data.direction;
                ch_info[cid].tag        <= req_q_data.tag;
                ch_bytes_rem[cid]       <= req_q_data.size;
                ch_startup_cnt[cid]     <= STARTUP_LATENCY[15:0];
                ch_state[cid]           <= CH_STARTUP;

                // 更新 round-robin 指针
                rr_ptr <= (cid + 1) % NUM_CHANNELS;
            end

            // -----------------------------
            // 2) Channel FSM 状态推进
            // -----------------------------
            for (c = 0; c < NUM_CHANNELS; ++c) begin
                case (ch_state[c])
                    CH_IDLE: begin
                        // 在分发逻辑中已经处理进入 STARTUP，这里不用管
                    end

                    CH_STARTUP: begin
                        if (ch_startup_cnt[c] != 0) begin
                            ch_startup_cnt[c] <= ch_startup_cnt[c] - 1'b1;
                        end else begin
                            ch_state[c] <= CH_TRANSFER;
                        end
                    end

                    CH_TRANSFER: begin
                        if (ch_bytes_rem[c] == 0) begin
                            // 已经无剩余（理论上不会进入这种分支）
                            ch_state[c] <= CH_DONE;
                        end else begin
                            // 一拍传输 BANDWIDTH 字节（简化模型，无背压）
                            if (ch_bytes_rem[c] <= BANDWIDTH[SIZE_WIDTH-1:0]) begin
                                ch_bytes_rem[c] <= '0;
                                ch_state[c]     <= CH_DONE;
                            end else begin
                                ch_bytes_rem[c] <= ch_bytes_rem[c] - BANDWIDTH[SIZE_WIDTH-1:0];
                            end

                            // 同时更新 src/dst 地址
                            if (ch_info[c].direction == 1'b0) begin
                                // G2L: src=gmem, dst=lmem
                                ch_info[c].src_addr <= ch_info[c].src_addr + BANDWIDTH[ADDR_WIDTH-1:0];
                                ch_info[c].dst_addr <= ch_info[c].dst_addr + BANDWIDTH[ADDR_WIDTH-1:0];
                            end else begin
                                // L2G: src=lmem, dst=gmem
                                ch_info[c].src_addr <= ch_info[c].src_addr + BANDWIDTH[ADDR_WIDTH-1:0];
                                ch_info[c].dst_addr <= ch_info[c].dst_addr + BANDWIDTH[ADDR_WIDTH-1:0];
                            end
                        end
                    end

                    CH_DONE: begin
                        // 等待完成队列有空位并 push
                        if (rsp_q_push && any_done && (done_ch_id == c[$clog2(NUM_CHANNELS)-1:0])) begin
                            // 当前 channel 的完成已被放入完成队列，可以回到 IDLE
                            ch_state[c] <= CH_IDLE;
                        end
                    end

                    default: begin
                        ch_state[c] <= CH_IDLE;
                    end
                endcase
            end
        end
    end

endmodule
