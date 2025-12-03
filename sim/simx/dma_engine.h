// Copyright © 2019-2023
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

#pragma once

#include <simobject.h>
#include <queue>
#include <unordered_map>
#include "types.h"

namespace vortex {

class RAM;
class Core;

class DmaEngine : public SimObject<DmaEngine> {
public:
    struct Config {
        uint32_t queue_size;        // 请求队列大小
        uint32_t bandwidth;         // 每周期每通道传输字节数
        uint32_t startup_latency;   // 启动延迟（周期）
        uint32_t num_channels;      // 并行通道数量
    };
    
    struct PerfStats {
        uint64_t transfers;         // 传输总数
        uint64_t bytes_read;        // 读取字节数
        uint64_t bytes_written;     // 写入字节数
        uint64_t total_latency;     // 总延迟
        uint64_t queue_stalls;      // 队列满的周期数
        uint64_t wait_stalls;       // wait指令等待的周期数
        
        PerfStats() 
            : transfers(0), bytes_read(0), bytes_written(0)
            , total_latency(0), queue_stalls(0), wait_stalls(0) {}
        
        PerfStats& operator+=(const PerfStats& other) {
            transfers += other.transfers;
            bytes_read += other.bytes_read;
            bytes_written += other.bytes_written;
            total_latency += other.total_latency;
            queue_stalls += other.queue_stalls;
            wait_stalls += other.wait_stalls;
            return *this;
        }
    };
    
    // DMA传输状态
    enum class DmaState {
        IDLE,
        STARTUP,        // 启动延迟阶段
        TRANSFERRING,   // 正在传输
        COMPLETED       // 完成
    };
    
    // DMA请求结构
    struct DmaRequest {
        uint32_t dma_id;
        uint64_t dst_addr;
        uint64_t src_addr;
        uint64_t size;
        int direction;              // 0=G2L, 1=L2G
        uint64_t start_cycle;
        DmaState state;
        uint64_t transfer_progress;
        uint64_t startup_counter;
        
        DmaRequest() 
            : dma_id(0), dst_addr(0), src_addr(0), size(0)
            , direction(0), start_cycle(0), state(DmaState::IDLE)
            , transfer_progress(0), startup_counter(0) {}
    };

    DmaEngine(const SimContext& ctx, const char* name, const Config& config);
    ~DmaEngine();

    void reset();
    void tick();

    // 提交DMA传输（返回DMA ID，失败返回-1）
    int32_t request_transfer(uint64_t dst_addr, uint64_t src_addr, 
                            uint64_t size, int direction);
    
    // 检查DMA是否完成
    bool is_completed(uint32_t dma_id) const;
    
    // 等待DMA完成（返回true表示已完成）
    bool wait_for_completion(uint32_t dma_id);
    
    // 确认并清理已完成的DMA
    void acknowledge_completion(uint32_t dma_id);
    
    bool is_busy() const;
    bool is_queue_full() const;

    const PerfStats& perf_stats() const;

    void attach_ram(RAM* ram);
    void attach_core(Core* core);

private:
    Config config_;
    RAM* ram_;
    Core* core_;
    PerfStats perf_stats_;
    
    uint32_t next_dma_id_;      // DMA ID生成器
    
    std::queue<DmaRequest> req_queue_;
    
    // 支持多通道并行传输
    static const uint32_t MAX_CHANNELS = 8;
    DmaRequest* active_transfers_[MAX_CHANNELS];
    
    std::unordered_map<uint32_t, DmaRequest> completed_transfers_;
    
    void process_transfer(DmaRequest* transfer, uint32_t channel_idx);
    void complete_transfer(uint32_t channel_idx);
};

} // namespace vortex
