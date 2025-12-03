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

#include "dma_engine.h"
#include "debug.h"
#include "mem.h"
#include "core.h"
#include "local_mem.h"
#include <VX_config.h>

using namespace vortex;

DmaEngine::DmaEngine(const SimContext& ctx, const char* name, const Config& config)
    : SimObject<DmaEngine>(ctx, name)
    , config_(config)
    , ram_(nullptr)
    , core_(nullptr)
    , next_dma_id_(1)
{
    // 初始化所有通道为空
    for (uint32_t i = 0; i < MAX_CHANNELS; ++i) {
        active_transfers_[i] = nullptr;
    }
}

DmaEngine::~DmaEngine() {
    for (uint32_t i = 0; i < MAX_CHANNELS; ++i) {
        if (active_transfers_[i]) {
            delete active_transfers_[i];
        }
    }
}

void DmaEngine::reset() {
    perf_stats_ = PerfStats();
    while (!req_queue_.empty()) {
        req_queue_.pop();
    }
    for (uint32_t i = 0; i < MAX_CHANNELS; ++i) {
        if (active_transfers_[i]) {
            delete active_transfers_[i];
            active_transfers_[i] = nullptr;
        }
    }
    completed_transfers_.clear();
    next_dma_id_ = 1;
}

int32_t DmaEngine::request_transfer(uint64_t dst_addr, uint64_t src_addr, 
                                     uint64_t size, int direction) {
    if (is_queue_full()) {
        DT(3, this->name() << ": DMA queue full, rejecting request");
        perf_stats_.queue_stalls++;
        return -1;
    }
    
    uint32_t dma_id = next_dma_id_++;
    
    DmaRequest req;
    req.dma_id = dma_id;
    req.dst_addr = dst_addr;
    req.src_addr = src_addr;
    req.size = size;
    req.direction = direction;
    req.start_cycle = SimPlatform::instance().cycles();
    req.state = DmaState::IDLE;
    req.transfer_progress = 0;
    req.startup_counter = 0;
    
    req_queue_.push(req);
    
    DT(3, this->name() << ": DMA request enqueued: id=" << dma_id
        << ", dst=0x" << std::hex << dst_addr
        << ", src=0x" << src_addr 
        << ", size=" << std::dec << size 
        << ", dir=" << (direction == 0 ? "G2L" : "L2G"));
    
    return static_cast<int32_t>(dma_id);
}

bool DmaEngine::is_completed(uint32_t dma_id) const {
    return completed_transfers_.count(dma_id) > 0;
}

bool DmaEngine::wait_for_completion(uint32_t dma_id) {
    if (is_completed(dma_id)) {
        return true;
    }
    
    // 检查所有活跃通道
    for (uint32_t i = 0; i < MAX_CHANNELS; ++i) {
        if (active_transfers_[i] && active_transfers_[i]->dma_id == dma_id) {
            perf_stats_.wait_stalls++;
            return false;
        }
    }
    
    // 检查等待队列
    std::queue<DmaRequest> temp_queue = req_queue_;
    while (!temp_queue.empty()) {
        if (temp_queue.front().dma_id == dma_id) {
            perf_stats_.wait_stalls++;
            return false;
        }
        temp_queue.pop();
    }
    
    return true;
}

void DmaEngine::acknowledge_completion(uint32_t dma_id) {
    auto it = completed_transfers_.find(dma_id);
    if (it != completed_transfers_.end()) {
        DT(4, this->name() << ": Acknowledged DMA " << dma_id);
        completed_transfers_.erase(it);
    }
}

bool DmaEngine::is_busy() const {
    if (!req_queue_.empty()) return true;
    for (uint32_t i = 0; i < config_.num_channels && i < MAX_CHANNELS; ++i) {
        if (active_transfers_[i] != nullptr) return true;
    }
    return false;
}

bool DmaEngine::is_queue_full() const {
    return req_queue_.size() >= config_.queue_size;
}

void DmaEngine::tick() {
    uint32_t num_channels = std::min(config_.num_channels, MAX_CHANNELS);
    
    // 为空闲通道分配新请求
    for (uint32_t ch = 0; ch < num_channels; ++ch) {
        if (active_transfers_[ch] == nullptr && !req_queue_.empty()) {
            active_transfers_[ch] = new DmaRequest(req_queue_.front());
            req_queue_.pop();
            active_transfers_[ch]->state = DmaState::STARTUP;
            active_transfers_[ch]->startup_counter = 0;
            
            DT(3, this->name() << ": Starting DMA " << active_transfers_[ch]->dma_id 
                << " on channel " << ch);
        }
    }
    
    // 处理所有活跃通道的传输
    for (uint32_t ch = 0; ch < num_channels; ++ch) {
        if (active_transfers_[ch]) {
            process_transfer(active_transfers_[ch], ch);
        }
    }
}

void DmaEngine::process_transfer(DmaRequest* transfer, uint32_t channel_idx) {
    assert(transfer != nullptr);
    assert(ram_ != nullptr);
    assert(core_ != nullptr);
    
    switch (transfer->state) {
    case DmaState::STARTUP:
        transfer->startup_counter++;
        if (transfer->startup_counter >= config_.startup_latency) {
            transfer->state = DmaState::TRANSFERRING;
            DT(3, this->name() << ": DMA " << transfer->dma_id 
                << " (ch" << channel_idx << ") startup complete");
        }
        break;
        
    case DmaState::TRANSFERRING: {
        uint64_t remaining = transfer->size - transfer->transfer_progress;
        uint64_t transfer_size = std::min(remaining, (uint64_t)config_.bandwidth);
        
        if (transfer_size > 0) {
            std::vector<uint8_t> buffer(transfer_size);
            
            if (transfer->direction == 0) {
                // G2L: Global to Local
                ram_->read(buffer.data(), 
                          transfer->src_addr + transfer->transfer_progress, 
                          transfer_size);
                
                uint64_t local_offset = transfer->dst_addr - LMEM_BASE_ADDR 
                                       + transfer->transfer_progress;
                core_->local_mem()->write(buffer.data(), local_offset, transfer_size);
                
                perf_stats_.bytes_read += transfer_size;
                perf_stats_.bytes_written += transfer_size;
                
            } else {
                // L2G: Local to Global
                uint64_t local_offset = transfer->src_addr - LMEM_BASE_ADDR 
                                       + transfer->transfer_progress;
                core_->local_mem()->read(buffer.data(), local_offset, transfer_size);
                
                ram_->write(buffer.data(), 
                           transfer->dst_addr + transfer->transfer_progress, 
                           transfer_size);
                
                perf_stats_.bytes_read += transfer_size;
                perf_stats_.bytes_written += transfer_size;
            }
            
            transfer->transfer_progress += transfer_size;
        }
        
        if (transfer->transfer_progress >= transfer->size) {
            complete_transfer(channel_idx);
        }
        break;
    }
        
    default:
        break;
    }
}

void DmaEngine::complete_transfer(uint32_t channel_idx) {
    DmaRequest* transfer = active_transfers_[channel_idx];
    assert(transfer != nullptr);
    
    uint64_t latency = SimPlatform::instance().cycles() - transfer->start_cycle;
    
    DT(3, this->name() << ": DMA " << transfer->dma_id 
        << " (ch" << channel_idx << ") completed, latency=" << latency << " cycles");
    
    transfer->state = DmaState::COMPLETED;
    completed_transfers_[transfer->dma_id] = *transfer;
    
    perf_stats_.transfers++;
    perf_stats_.total_latency += latency;
    
    delete transfer;
    active_transfers_[channel_idx] = nullptr;
}

const DmaEngine::PerfStats& DmaEngine::perf_stats() const {
    return perf_stats_;
}

void DmaEngine::attach_ram(RAM* ram) {
    ram_ = ram;
}

void DmaEngine::attach_core(Core* core) {
    core_ = core;
}
