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

#include "socket.h"
#include "cluster.h"
#include <VX_config.h>

using namespace vortex;

// DMA DCR 地址定义
#define VX_DCR_DMA_SRC_ADDR0     0x006
#define VX_DCR_DMA_SRC_ADDR1     0x007
#define VX_DCR_DMA_DST_ADDR0     0x008
#define VX_DCR_DMA_DST_ADDR1     0x009
#define VX_DCR_DMA_SIZE0         0x00A
#define VX_DCR_DMA_SIZE1         0x00B
#define VX_DCR_DMA_CORE_ID       0x00C
#define VX_DCR_DMA_CTRL          0x00D
#define VX_DCR_DMA_STATUS        0x00E

Socket::Socket(const SimContext& ctx,
                uint32_t socket_id,
                Cluster* cluster,
                const Arch &arch,
                const DCRS &dcrs)
  : SimObject(ctx, StrFormat("socket%d", socket_id))
  , mem_req_ports(L1_MEM_PORTS, this)
  , mem_rsp_ports(L1_MEM_PORTS, this)
  , socket_id_(socket_id)
  , cluster_(cluster)
  , cores_(arch.socket_size())
{
  auto cores_per_socket = cores_.size();

  char sname[100];
  snprintf(sname, 100, "%s-icaches", this->name().c_str());
  icaches_ = CacheCluster::Create(sname, cores_per_socket, NUM_ICACHES, CacheSim::Config{
    !ICACHE_ENABLED,
    log2ceil(ICACHE_SIZE),  // C
    log2ceil(L1_LINE_SIZE), // L
    log2ceil(sizeof(uint32_t)), // W
    log2ceil(ICACHE_NUM_WAYS),// A
    log2ceil(1),            // B
    XLEN,                   // address bits
    1,                      // number of inputs
    ICACHE_MEM_PORTS,       // memory ports
    false,                  // write-back
    false,                  // write response
    ICACHE_MSHR_SIZE,       // mshr size
    2,                      // pipeline latency
  });

  snprintf(sname, 100, "%s-dcaches", this->name().c_str());
  dcaches_ = CacheCluster::Create(sname, cores_per_socket, NUM_DCACHES, CacheSim::Config{
    !DCACHE_ENABLED,
    log2ceil(DCACHE_SIZE),  // C
    log2ceil(L1_LINE_SIZE), // L
    log2ceil(DCACHE_WORD_SIZE), // W
    log2ceil(DCACHE_NUM_WAYS),// A
    log2ceil(DCACHE_NUM_BANKS), // B
    XLEN,                   // address bits
    DCACHE_NUM_REQS,        // number of inputs
    L1_MEM_PORTS,           // memory ports
    DCACHE_WRITEBACK,       // write-back
    false,                  // write response
    DCACHE_MSHR_SIZE,       // mshr size
    2,                      // pipeline latency
  });

  // Create DMA Engine
  snprintf(sname, 100, "%s-dma", this->name().c_str());
  dma_engine_ = DmaEngine::Create(sname, DmaEngine::Config{
    socket_id,
    static_cast<uint32_t>(cores_.size()),
    4,   // max_outstanding_reads
    4,   // max_outstanding_writes
    64   // transfer_size (cache line)
  });
  dma_engine_->set_socket(this);

  // find overlap
  uint32_t overlap = MIN(ICACHE_MEM_PORTS, L1_MEM_PORTS);

  // connect l1 caches and DMA to outgoing memory interfaces
  for (uint32_t i = 0; i < L1_MEM_PORTS; ++i) {
    snprintf(sname, 100, "%s-l1_arb%d", this->name().c_str(), i);
    // Extend arbiter to include DMA (3 inputs instead of 2)
    auto l1_arb = MemArbiter::Create(sname, ArbiterType::RoundRobin, 2 * overlap + 1, overlap);

    if (i < overlap) {
      icaches_->MemReqPorts.at(i).bind(&l1_arb->ReqIn.at(i));
      l1_arb->RspIn.at(i).bind(&icaches_->MemRspPorts.at(i));

      dcaches_->MemReqPorts.at(i).bind(&l1_arb->ReqIn.at(overlap + i));
      l1_arb->RspIn.at(overlap + i).bind(&dcaches_->MemRspPorts.at(i));

      // Connect DMA to the first arbiter only
      if (i == 0) {
        dma_engine_->mem_req_port.bind(&l1_arb->ReqIn.at(2 * overlap));
        l1_arb->RspIn.at(2 * overlap).bind(&dma_engine_->mem_rsp_port);
      }

      l1_arb->ReqOut.at(i).bind(&this->mem_req_ports.at(i));
      this->mem_rsp_ports.at(i).bind(&l1_arb->RspOut.at(i));
    } else {
      if (L1_MEM_PORTS > ICACHE_MEM_PORTS) {
        // if more dcache ports
        dcaches_->MemReqPorts.at(i).bind(&this->mem_req_ports.at(i));
        this->mem_rsp_ports.at(i).bind(&dcaches_->MemRspPorts.at(i));
      } else {
        // if more icache ports
        icaches_->MemReqPorts.at(i).bind(&this->mem_req_ports.at(i));
        this->mem_rsp_ports.at(i).bind(&icaches_->MemRspPorts.at(i));
      }
    }
  }

  // create cores
  for (uint32_t i = 0; i < cores_per_socket; ++i) {
    uint32_t core_id = socket_id * cores_per_socket + i;
    cores_.at(i) = Core::Create(core_id, this, arch, dcrs);
  }

  // connect cores to caches
  for (uint32_t i = 0; i < cores_per_socket; ++i) {
    cores_.at(i)->icache_req_ports.at(0).bind(&icaches_->CoreReqPorts.at(i).at(0));
    icaches_->CoreRspPorts.at(i).at(0).bind(&cores_.at(i)->icache_rsp_ports.at(0));

    for (uint32_t j = 0; j < DCACHE_NUM_REQS; ++j) {
      cores_.at(i)->dcache_req_ports.at(j).bind(&dcaches_->CoreReqPorts.at(i).at(j));
      dcaches_->CoreRspPorts.at(i).at(j).bind(&cores_.at(i)->dcache_rsp_ports.at(j));
    }
  }
}

Socket::~Socket() {
  //--
}

void Socket::reset() {
  if (dma_engine_) {
    dma_engine_->reset();
  }
}

void Socket::tick() {
  if (dma_engine_) {
    dma_engine_->tick();
  }
}

void Socket::attach_ram(RAM* ram) {
  for (auto core : cores_) {
    core->attach_ram(ram);
  }
}

#ifdef VM_ENABLE
void Socket::set_satp(uint64_t satp) {
  for (auto core : cores_) {
    core->set_satp(satp);
  }
}
#endif

bool Socket::running() const {
  for (auto& core : cores_) {
    if (core->running())
      return true;
  }
  return false;
}

int Socket::get_exitcode() const {
  int exitcode = 0;
  for (auto& core : cores_) {
    exitcode |= core->get_exitcode();
  }
  return exitcode;
}

void Socket::barrier(uint32_t bar_id, uint32_t count, uint32_t core_id) {
  cluster_->barrier(bar_id, count, socket_id_ * cores_.size() + core_id);
}

void Socket::resume(uint32_t core_index) {
  cores_.at(core_index)->resume(-1);
}

void Socket::dcr_write(uint32_t addr, uint32_t value) {
  // Check if this is a DMA DCR
  if (addr >= VX_DCR_DMA_SRC_ADDR0 && addr <= VX_DCR_DMA_CTRL) {
    if (dma_engine_) {
      // Special handling for CORE_ID: convert to socket-local index
      if (addr == VX_DCR_DMA_CORE_ID) {
        uint32_t global_core_id = value;
        uint32_t cores_per_socket = cores_.size();
        uint32_t target_socket = global_core_id / cores_per_socket;
        
        // Only the target socket processes this DCR
        if (target_socket == socket_id_) {
          uint32_t local_core_id = global_core_id % cores_per_socket;
          dma_engine_->dcr_write(addr, local_core_id);
          
          // Dynamically bind DMA to target core's LocalMem
          if (local_core_id < cores_.size()) {
            auto core = cores_[local_core_id].get();
            auto lmem = core->local_mem();
            
            // Use the last LocalMem port for DMA
            uint32_t dma_channel = LSU_CHANNELS - 1;
            dma_engine_->lmem_req_port.bind(&lmem->Inputs.at(dma_channel));
            lmem->Outputs.at(dma_channel).bind(&dma_engine_->lmem_rsp_port);
            
            DT(3, this->name() << ": DMA bound to core " << local_core_id);
          }
        }
      } else {
        dma_engine_->dcr_write(addr, value);
      }
    }
  }
}

uint32_t Socket::dcr_read(uint32_t addr) const {
  if (addr >= VX_DCR_DMA_SRC_ADDR0 && addr <= VX_DCR_DMA_STATUS) {
    if (dma_engine_) {
      return dma_engine_->dcr_read(addr);
    }
  }
  return 0;
}

Socket::PerfStats Socket::perf_stats() const {
  PerfStats perf_stats;
  perf_stats.icache = icaches_->perf_stats();
  perf_stats.dcache = dcaches_->perf_stats();
  if (dma_engine_) {
    perf_stats.dma = dma_engine_->perf_stats();
  }
  return perf_stats;
}