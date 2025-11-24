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

#include "dma_engine.h"
#include "debug.h"
#include "mem.h"       // For RAM access
#include "core.h"      // For Core and Local Memory access
#include "local_mem.h" // For LocalMem interface
#include <VX_config.h> // For LMEM_BASE_ADDR

using namespace vortex;

DmaEngine::DmaEngine(const SimContext& ctx, const char* name)
    : SimObject<DmaEngine>(ctx, name)
    , ram_(nullptr)
    , core_(nullptr)
{}

DmaEngine::~DmaEngine() {}

void DmaEngine::reset() {
    perf_stats_ = PerfStats();
}

void DmaEngine::tick() {
    // Synchronous DMA, no internal state machine needed for tick
}

void DmaEngine::trigger_transfer(uint64_t dst_addr, uint64_t src_addr, uint64_t size, int direction) {
    if (!ram_) {
        DT(0, this->name() << ": Error: DMA Engine not attached to RAM!");
        std::abort();
    }
    if (!core_) {
        DT(0, this->name() << ": Error: DMA Engine not attached to Core!");
        std::abort();
    }

    DT(3, this->name() << ": DMA transfer triggered: dst=0x" << std::hex << dst_addr
        << ", src=0x" << src_addr << ", size=" << std::dec << size << ", dir=" << direction);

    // Perform direct memory copy (synchronous)
    std::vector<uint8_t> buffer(size);
    
    if (direction == 0) { // DMA_DIR_G2L: Global to Local
        // Read from global memory (RAM)
        ram_->read(buffer.data(), src_addr, size);
        perf_stats_.reads += size;
        
        // Write to local memory (through Core's LocalMem)
        // Convert virtual local address to offset within local memory
        uint64_t local_offset = dst_addr - LMEM_BASE_ADDR;
        core_->local_mem()->write(buffer.data(), local_offset, size);
        perf_stats_.writes += size;
        
        DT(3, this->name() << ": G2L: Read from RAM 0x" << std::hex << src_addr 
            << ", wrote to LocalMem offset 0x" << local_offset);
    } else { // DMA_DIR_L2G: Local to Global
        // Read from local memory (through Core's LocalMem)
        // Convert virtual local address to offset within local memory
        uint64_t local_offset = src_addr - LMEM_BASE_ADDR;
        core_->local_mem()->read(buffer.data(), local_offset, size);
        perf_stats_.reads += size;
        
        // Write to global memory (RAM)
        ram_->write(buffer.data(), dst_addr, size);
        perf_stats_.writes += size;
        
        DT(3, this->name() << ": L2G: Read from LocalMem offset 0x" << std::hex << local_offset 
            << ", wrote to RAM 0x" << dst_addr);
    }
    
    perf_stats_.latency += 1; // Assume 1 cycle for synchronous transfer for now
    
    DT(3, this->name() << ": DMA transfer completed.");
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

