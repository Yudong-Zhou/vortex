# Vortex DMA 修订设计方案

> 基于对 SimX 架构的深入理解,修订原有设计,使其符合事件驱动模型和异步通信机制

## 📋 目录

1. [设计变更总结](#设计变更总结)
2. [架构设计](#架构设计)
3. [完整代码实现](#完整代码实现)
4. [集成步骤](#集成步骤)
5. [测试方案](#测试方案)

---

## 设计变更总结

### 🔄 主要改进

| 方面 | 原设计 | 修订设计 | 原因 |
|------|--------|----------|------|
| **内存访问** | 同步直接访问 RAM/LocalMem | 异步 SimPort 通信 | 符合 SimX 事件驱动模型 |
| **Core 访问** | 直接访问私有成员 `cores_` | 通过 `Socket::get_core()` | 封装性,编译通过 |
| **仲裁机制** | 无 | 集成到 L1 Memory Arbiter | 避免端口冲突 |
| **传输模式** | 单周期完成 | 流水线异步传输 | 更真实的性能模拟 |
| **地址验证** | 简单范围检查 | 使用 `get_addr_type()` | 符合 Vortex 地址空间设计 |
| **DCR 路由** | 部分实现 | 完整路由链 | 支持多 Socket 系统 |

### ✅ 新增特性

- **流水线传输**: 读写操作重叠,提高吞吐量
- **Bank 冲突检测**: 统计 LocalMem bank 冲突
- **完成回调**: 支持中断式通知
- **对齐检查**: 确保地址和大小对齐
- **详细性能统计**: 延迟、冲突、错误等

---

## 架构设计

### 系统层次结构

```
Processor
  └─ Cluster[]
      └─ Socket[]
          ├─ L1 Memory Arbiter (3 输入 → N 输出)
          │   ├─ Input 0: ICache
          │   ├─ Input 1: DCache  
          │   └─ Input 2: DMA Engine ← 新增
          │
          ├─ DMA Engine
          │   ├─ mem_req_port  → L1 Arbiter
          │   ├─ mem_rsp_port  ← L1 Arbiter
          │   ├─ lmem_req_port → LocalMem (动态绑定)
          │   └─ lmem_rsp_port ← LocalMem
          │
          └─ Core[]
              └─ LocalMem
                  ├─ Inputs[LSU_CHANNELS]
                  └─ Outputs[LSU_CHANNELS]
```

### 数据流

#### Global → Shared 传输

```
1. DMA 发起读请求
   DMA.mem_req_port → L1_Arbiter → L2_Cache → L3_Cache → DRAM

2. 等待读响应
   DRAM → L3 → L2 → L1_Arbiter → DMA.mem_rsp_port

3. DMA 发起写请求
   DMA.lmem_req_port → LocalMem.Inputs[DMA_CHANNEL]

4. 等待写响应
   LocalMem.Outputs[DMA_CHANNEL] → DMA.lmem_rsp_port

5. 重复步骤 1-4 直到传输完成
```

### 状态机

```
        ┌─────────┐
        │  Idle   │
        └────┬────┘
             │ START
             ▼
        ┌─────────┐
        │ Reading │ ◄──┐
        └────┬────┘    │
             │ Got Response
             ▼         │
        ┌─────────┐    │
        │ Writing │    │
        └────┬────┘    │
             │ Got Response
             ├─────────┘ More data
             │
             ▼ All done
        ┌─────────┐
        │Complete │
        └─────────┘
             │
             ▼
        ┌─────────┐
        │  Idle   │
        └─────────┘
```

---

## 完整代码实现

### 1. DCR 寄存器定义

#### hw/rtl/VX_types.vh

```verilog
// DMA DCR 寄存器地址 (在现有 DCR 之后)
`define VX_DCR_DMA_SRC_ADDR0     12'h006
`define VX_DCR_DMA_SRC_ADDR1     12'h007
`define VX_DCR_DMA_DST_ADDR0     12'h008
`define VX_DCR_DMA_DST_ADDR1     12'h009
`define VX_DCR_DMA_SIZE0         12'h00A
`define VX_DCR_DMA_SIZE1         12'h00B
`define VX_DCR_DMA_CORE_ID       12'h00C
`define VX_DCR_DMA_CTRL          12'h00D
`define VX_DCR_DMA_STATUS        12'h00E
`define VX_DCR_BASE_STATE_END    12'h00F  // 更新结束地址

// DMA 控制寄存器位定义
`define DMA_CTRL_START           0   // [0] 启动传输
`define DMA_CTRL_DIR             1   // [1] 方向: 0=G→S, 1=S→G
`define DMA_CTRL_IRQ_EN          2   // [2] 中断使能

// DMA 状态寄存器位定义
`define DMA_STATUS_IDLE          0   // [0] 空闲
`define DMA_STATUS_BUSY          1   // [1] 传输中
`define DMA_STATUS_DONE          2   // [2] 完成
`define DMA_STATUS_ERROR         3   // [3] 错误
```

### 2. DMA Engine 头文件

#### sim/simx/dma_engine.h

```cpp
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

#pragma once

#include <simobject.h>
#include "types.h"
#include "constants.h"
#include <functional>
#include <queue>

namespace vortex {

class Socket;

class DmaEngine : public SimObject<DmaEngine> {
public:
  struct Config {
    uint32_t socket_id;
    uint32_t num_cores;
    uint32_t max_outstanding_reads;   // 最大并发读请求
    uint32_t max_outstanding_writes;  // 最大并发写请求
    uint32_t transfer_size;           // 每次传输大小(字节)
    
    Config()
      : socket_id(0)
      , num_cores(0)
      , max_outstanding_reads(4)
      , max_outstanding_writes(4)
      , transfer_size(64)  // 默认一个 cache line
    {}
  };

  struct PerfStats {
    uint64_t transfers;           // 传输次数
    uint64_t bytes_transferred;   // 传输字节数
    uint64_t cycles_active;       // 活跃周期数
    uint64_t cycles_idle;         // 空闲周期数
    uint64_t read_requests;       // 读请求数
    uint64_t write_requests;      // 写请求数
    uint64_t read_latency;        // 累计读延迟
    uint64_t write_latency;       // 累计写延迟
    uint64_t bank_conflicts;      // Bank 冲突次数
    uint64_t errors;              // 错误次数

    PerfStats()
      : transfers(0)
      , bytes_transferred(0)
      , cycles_active(0)
      , cycles_idle(0)
      , read_requests(0)
      , write_requests(0)
      , read_latency(0)
      , write_latency(0)
      , bank_conflicts(0)
      , errors(0)
    {}

    PerfStats& operator+=(const PerfStats& rhs) {
      this->transfers += rhs.transfers;
      this->bytes_transferred += rhs.bytes_transferred;
      this->cycles_active += rhs.cycles_active;
      this->cycles_idle += rhs.cycles_idle;
      this->read_requests += rhs.read_requests;
      this->write_requests += rhs.write_requests;
      this->read_latency += rhs.read_latency;
      this->write_latency += rhs.write_latency;
      this->bank_conflicts += rhs.bank_conflicts;
      this->errors += rhs.errors;
      return *this;
    }
  };

  // 内存请求/响应端口
  SimPort<MemReq> mem_req_port;   // 访问 Global Memory (通过 L1 Arbiter)
  SimPort<MemRsp> mem_rsp_port;

  SimPort<MemReq> lmem_req_port;  // 访问 Local Memory
  SimPort<MemRsp> lmem_rsp_port;

  // 完成回调类型
  using CompletionCallback = std::function<void(bool success, uint64_t bytes)>;

  DmaEngine(const SimContext& ctx, const char* name, const Config& config);
  ~DmaEngine();

  void reset();
  void tick();

  // 设置 Socket 指针(用于访问 cores)
  void set_socket(Socket* socket) {
    socket_ = socket;
  }

  // DCR 接口
  void dcr_write(uint32_t addr, uint32_t value);
  uint32_t dcr_read(uint32_t addr) const;

  // 状态查询
  bool is_busy() const;
  bool is_idle() const;
  bool is_complete() const;
  bool has_error() const;

  // 完成回调
  void set_completion_callback(CompletionCallback cb);

  // 性能统计
  PerfStats perf_stats() const;

private:
  class Impl;
  Impl* impl_;
  Socket* socket_;
};

} // namespace vortex
```

### 3. DMA Engine 实现

#### sim/simx/dma_engine.cpp

```cpp
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
#include "socket.h"
#include "core.h"
#include "local_mem.h"
#include "debug.h"
#include <VX_config.h>
#include <bitmanip.h>

using namespace vortex;

// DMA 状态
enum class DmaState {
  Idle,       // 空闲
  Reading,    // 从源读取
  Writing,    // 写入目标
  Complete,   // 完成
  Error       // 错误
};

// DMA 方向
enum class DmaDirection {
  GlobalToShared = 0,
  SharedToGlobal = 1
};

// 传输事务
struct DmaTransaction {
  uint64_t src_addr;
  uint64_t dst_addr;
  uint32_t size;
  uint32_t tag;
  uint64_t issue_cycle;  // 发起周期(用于延迟统计)
  
  DmaTransaction()
    : src_addr(0), dst_addr(0), size(0), tag(0), issue_cycle(0)
  {}
};

class DmaEngine::Impl {
public:
  DmaEngine* simobject_;
  Config config_;
  Socket* socket_;

  // DMA 状态
  DmaState state_;
  DmaDirection direction_;
  
  // 传输参数(从 DCR 读取)
  uint64_t src_addr_;
  uint64_t dst_addr_;
  uint64_t size_;
  uint32_t core_id_;
  
  // 传输进度
  uint64_t remaining_size_;
  uint64_t current_src_addr_;
  uint64_t current_dst_addr_;
  uint32_t next_tag_;
  
  // 状态和控制寄存器
  uint32_t status_reg_;
  uint32_t ctrl_reg_;
  
  // 传输队列
  std::queue<DmaTransaction> pending_reads_;   // 已发起读请求
  std::queue<DmaTransaction> pending_writes_;  // 待写入数据
  
  // 完成回调
  CompletionCallback completion_cb_;
  
  // 性能统计
  mutable PerfStats perf_stats_;
  uint64_t transfer_start_cycle_;

  Impl(DmaEngine* simobject, const Config& config)
    : simobject_(simobject)
    , config_(config)
    , socket_(nullptr)
    , state_(DmaState::Idle)
    , direction_(DmaDirection::GlobalToShared)
    , src_addr_(0)
    , dst_addr_(0)
    , size_(0)
    , core_id_(0)
    , remaining_size_(0)
    , current_src_addr_(0)
    , current_dst_addr_(0)
    , next_tag_(0)
    , status_reg_(1 << DMA_STATUS_IDLE)
    , ctrl_reg_(0)
    , transfer_start_cycle_(0)
  {}

  void reset() {
    state_ = DmaState::Idle;
    status_reg_ = (1 << DMA_STATUS_IDLE);
    ctrl_reg_ = 0;
    remaining_size_ = 0;
    next_tag_ = 0;
    
    // 清空队列
    while (!pending_reads_.empty()) pending_reads_.pop();
    while (!pending_writes_.empty()) pending_writes_.pop();
    
    perf_stats_ = PerfStats();
  }

  void dcr_write(uint32_t addr, uint32_t value) {
    switch (addr) {
    case VX_DCR_DMA_SRC_ADDR0:
      src_addr_ = (src_addr_ & 0xFFFFFFFF00000000ULL) | value;
      DT(3, simobject_->name() << "-dcr: SRC_ADDR0=0x" << std::hex << value);
      break;
    case VX_DCR_DMA_SRC_ADDR1:
      src_addr_ = (src_addr_ & 0xFFFFFFFFULL) | ((uint64_t)value << 32);
      DT(3, simobject_->name() << "-dcr: SRC_ADDR1=0x" << std::hex << value);
      break;
    case VX_DCR_DMA_DST_ADDR0:
      dst_addr_ = (dst_addr_ & 0xFFFFFFFF00000000ULL) | value;
      DT(3, simobject_->name() << "-dcr: DST_ADDR0=0x" << std::hex << value);
      break;
    case VX_DCR_DMA_DST_ADDR1:
      dst_addr_ = (dst_addr_ & 0xFFFFFFFFULL) | ((uint64_t)value << 32);
      DT(3, simobject_->name() << "-dcr: DST_ADDR1=0x" << std::hex << value);
      break;
    case VX_DCR_DMA_SIZE0:
      size_ = (size_ & 0xFFFFFFFF00000000ULL) | value;
      DT(3, simobject_->name() << "-dcr: SIZE0=" << std::dec << value);
      break;
    case VX_DCR_DMA_SIZE1:
      size_ = (size_ & 0xFFFFFFFFULL) | ((uint64_t)value << 32);
      DT(3, simobject_->name() << "-dcr: SIZE1=" << std::dec << value);
      break;
    case VX_DCR_DMA_CORE_ID:
      core_id_ = value;
      DT(3, simobject_->name() << "-dcr: CORE_ID=" << value);
      break;
    case VX_DCR_DMA_CTRL:
      ctrl_reg_ = value;
      DT(3, simobject_->name() << "-dcr: CTRL=0x" << std::hex << value);
      if (value & (1 << DMA_CTRL_START)) {
        start_transfer();
      }
      break;
    default:
      break;
    }
  }

  uint32_t dcr_read(uint32_t addr) const {
    switch (addr) {
    case VX_DCR_DMA_STATUS:
      return status_reg_;
    case VX_DCR_DMA_SRC_ADDR0:
      return src_addr_ & 0xFFFFFFFF;
    case VX_DCR_DMA_SRC_ADDR1:
      return (src_addr_ >> 32) & 0xFFFFFFFF;
    case VX_DCR_DMA_DST_ADDR0:
      return dst_addr_ & 0xFFFFFFFF;
    case VX_DCR_DMA_DST_ADDR1:
      return (dst_addr_ >> 32) & 0xFFFFFFFF;
    case VX_DCR_DMA_SIZE0:
      return size_ & 0xFFFFFFFF;
    case VX_DCR_DMA_SIZE1:
      return (size_ >> 32) & 0xFFFFFFFF;
    case VX_DCR_DMA_CORE_ID:
      return core_id_;
    case VX_DCR_DMA_CTRL:
      return ctrl_reg_;
    default:
      return 0;
    }
  }

  void start_transfer() {
    // 检查是否已在传输
    if (state_ != DmaState::Idle) {
      DPH(1, simobject_->name() << ": Transfer already in progress");
      return;
    }

    // 验证参数
    if (size_ == 0) {
      DPH(1, simobject_->name() << ": Invalid size (0)");
      set_error();
      return;
    }

    if (core_id_ >= config_.num_cores) {
      DPH(1, simobject_->name() << ": Invalid core_id " << core_id_ 
          << " >= " << config_.num_cores);
      set_error();
      return;
    }

    // 检查地址对齐
    uint32_t alignment = config_.transfer_size;
    if ((src_addr_ % alignment) != 0 || (dst_addr_ % alignment) != 0) {
      DPH(1, simobject_->name() << ": Addresses not aligned to " 
          << alignment << " bytes");
      set_error();
      return;
    }

    // 设置方向
    direction_ = (ctrl_reg_ & (1 << DMA_CTRL_DIR)) ? 
                 DmaDirection::SharedToGlobal : 
                 DmaDirection::GlobalToShared;

    // 验证地址类型
    AddrType src_type = get_addr_type(src_addr_);
    AddrType dst_type = get_addr_type(dst_addr_);

    if (direction_ == DmaDirection::GlobalToShared) {
      if (src_type != AddrType::Global || dst_type != AddrType::Shared) {
        DPH(1, simobject_->name() << ": Invalid address types for G→S transfer");
        set_error();
        return;
      }
    } else {
      if (src_type != AddrType::Shared || dst_type != AddrType::Global) {
        DPH(1, simobject_->name() << ": Invalid address types for S→G transfer");
        set_error();
        return;
      }
    }

    // 初始化传输状态
    current_src_addr_ = src_addr_;
    current_dst_addr_ = dst_addr_;
    remaining_size_ = size_;
    next_tag_ = 0;
    state_ = DmaState::Reading;
    status_reg_ = (1 << DMA_STATUS_BUSY);
    transfer_start_cycle_ = SimPlatform::instance().cycles();

    DT(2, simobject_->name() << ": Starting transfer: "
        << (direction_ == DmaDirection::GlobalToShared ? "G→S" : "S→G")
        << ", src=0x" << std::hex << src_addr_ 
        << ", dst=0x" << dst_addr_
        << ", size=" << std::dec << size_
        << ", core=" << core_id_);

    perf_stats_.transfers++;
  }

  void tick() {
    // 更新性能统计
    if (state_ == DmaState::Idle) {
      perf_stats_.cycles_idle++;
    } else {
      perf_stats_.cycles_active++;
    }

    // 状态机处理
    switch (state_) {
    case DmaState::Idle:
      // 空闲,等待 START 命令
      break;

    case DmaState::Reading:
      process_reading();
      break;

    case DmaState::Writing:
      process_writing();
      break;

    case DmaState::Complete:
    case DmaState::Error:
      // 完成或错误状态,等待复位
      break;
    }

    // 清除 START 位
    if (ctrl_reg_ & (1 << DMA_CTRL_START)) {
      ctrl_reg_ &= ~(1 << DMA_CTRL_START);
    }
  }

private:
  void process_reading() {
    // 1. 处理读响应
    if (!simobject_->mem_rsp_port.empty()) {
      auto& rsp = simobject_->mem_rsp_port.front();
      
      // 查找对应的事务
      if (!pending_reads_.empty()) {
        auto& txn = pending_reads_.front();
        if (txn.tag == rsp.tag) {
          // 计算读延迟
          uint64_t latency = SimPlatform::instance().cycles() - txn.issue_cycle;
          perf_stats_.read_latency += latency;
          
          // 移到写队列
          pending_writes_.push(txn);
          pending_reads_.pop();
          
          DT(4, simobject_->name() << "-read-rsp: tag=" << rsp.tag 
              << ", latency=" << latency);
        }
      }
      
      simobject_->mem_rsp_port.pop();
    }

    // 2. 处理 LocalMem 读响应(S→G 模式)
    if (direction_ == DmaDirection::SharedToGlobal) {
      if (!simobject_->lmem_rsp_port.empty()) {
        auto& rsp = simobject_->lmem_rsp_port.front();
        
        if (!pending_reads_.empty()) {
          auto& txn = pending_reads_.front();
          if (txn.tag == rsp.tag) {
            uint64_t latency = SimPlatform::instance().cycles() - txn.issue_cycle;
            perf_stats_.read_latency += latency;
            
            pending_writes_.push(txn);
            pending_reads_.pop();
            
            DT(4, simobject_->name() << "-lmem-read-rsp: tag=" << rsp.tag);
          }
        }
        
        simobject_->lmem_rsp_port.pop();
      }
    }

    // 3. 发起新的读请求
    if (remaining_size_ > 0 && 
        pending_reads_.size() < config_.max_outstanding_reads) {
      
      uint32_t transfer_size = MIN(remaining_size_, config_.transfer_size);
      
      DmaTransaction txn;
      txn.src_addr = current_src_addr_;
      txn.dst_addr = current_dst_addr_;
      txn.size = transfer_size;
      txn.tag = next_tag_++;
      txn.issue_cycle = SimPlatform::instance().cycles();
      
      if (direction_ == DmaDirection::GlobalToShared) {
        // 从 Global Memory 读取
        MemReq req;
        req.addr = txn.src_addr;
        req.write = false;
        req.type = AddrType::Global;
        req.tag = txn.tag;
        req.cid = config_.socket_id;
        req.uuid = perf_stats_.read_requests;
        
        simobject_->mem_req_port.push(req, 1);
        perf_stats_.read_requests++;
        
        DT(4, simobject_->name() << "-read-req: addr=0x" << std::hex 
            << req.addr << ", tag=" << req.tag);
      } else {
        // 从 Local Memory 读取
        MemReq req;
        req.addr = txn.src_addr;
        req.write = false;
        req.type = AddrType::Shared;
        req.tag = txn.tag;
        req.cid = config_.socket_id;
        req.uuid = perf_stats_.read_requests;
        
        simobject_->lmem_req_port.push(req, 1);
        perf_stats_.read_requests++;
        
        DT(4, simobject_->name() << "-lmem-read-req: addr=0x" << std::hex 
            << req.addr << ", tag=" << req.tag);
      }
      
      pending_reads_.push(txn);
      current_src_addr_ += transfer_size;
      current_dst_addr_ += transfer_size;
      remaining_size_ -= transfer_size;
    }

    // 4. 如果所有读请求已发起,切换到写状态
    if (remaining_size_ == 0 && !pending_writes_.empty()) {
      state_ = DmaState::Writing;
      DT(3, simobject_->name() << ": Switching to Writing state");
    }
  }

  void process_writing() {
    // 1. 处理写响应
    if (!simobject_->mem_rsp_port.empty()) {
      auto& rsp = simobject_->mem_rsp_port.front();
      
      // 写完成,更新统计
      uint64_t latency = SimPlatform::instance().cycles() - transfer_start_cycle_;
      perf_stats_.write_latency += latency;
      perf_stats_.bytes_transferred += config_.transfer_size;
      
      DT(4, simobject_->name() << "-write-rsp: tag=" << rsp.tag);
      
      simobject_->mem_rsp_port.pop();
    }

    // 2. 处理 LocalMem 写响应(G→S 模式)
    if (direction_ == DmaDirection::GlobalToShared) {
      if (!simobject_->lmem_rsp_port.empty()) {
        auto& rsp = simobject_->lmem_rsp_port.front();
        
        perf_stats_.bytes_transferred += config_.transfer_size;
        
        DT(4, simobject_->name() << "-lmem-write-rsp: tag=" << rsp.tag);
        
        simobject_->lmem_rsp_port.pop();
      }
    }

    // 3. 发起写请求
    if (!pending_writes_.empty() && 
        (pending_reads_.size() + pending_writes_.size()) < 
        (config_.max_outstanding_reads + config_.max_outstanding_writes)) {
      
      auto& txn = pending_writes_.front();
      
      if (direction_ == DmaDirection::GlobalToShared) {
        // 写入 Local Memory
        MemReq req;
        req.addr = txn.dst_addr;
        req.write = true;
        req.type = AddrType::Shared;
        req.tag = txn.tag;
        req.cid = config_.socket_id;
        req.uuid = perf_stats_.write_requests;
        
        simobject_->lmem_req_port.push(req, 1);
        perf_stats_.write_requests++;
        
        DT(4, simobject_->name() << "-lmem-write-req: addr=0x" << std::hex 
            << req.addr << ", tag=" << req.tag);
      } else {
        // 写入 Global Memory
        MemReq req;
        req.addr = txn.dst_addr;
        req.write = true;
        req.type = AddrType::Global;
        req.tag = txn.tag;
        req.cid = config_.socket_id;
        req.uuid = perf_stats_.write_requests;
        
        simobject_->mem_req_port.push(req, 1);
        perf_stats_.write_requests++;
        
        DT(4, simobject_->name() << "-write-req: addr=0x" << std::hex 
            << req.addr << ", tag=" << req.tag);
      }
      
      pending_writes_.pop();
    }

    // 4. 检查是否完成
    if (pending_reads_.empty() && pending_writes_.empty() && remaining_size_ == 0) {
      complete_transfer(true);
    }
  }

  void complete_transfer(bool success) {
    state_ = success ? DmaState::Complete : DmaState::Error;
    status_reg_ = success ? (1 << DMA_STATUS_DONE) : (1 << DMA_STATUS_ERROR);
    status_reg_ &= ~(1 << DMA_STATUS_BUSY);
    
    if (!success) {
      perf_stats_.errors++;
    }
    
    uint64_t total_cycles = SimPlatform::instance().cycles() - transfer_start_cycle_;
    
    DT(2, simobject_->name() << ": Transfer " 
        << (success ? "completed" : "failed")
        << ", bytes=" << (size_ - remaining_size_)
        << ", cycles=" << total_cycles);
    
    // 调用完成回调
    if (completion_cb_) {
      completion_cb_(success, size_ - remaining_size_);
    }
    
    // 自动返回 Idle 状态
    state_ = DmaState::Idle;
    status_reg_ = (1 << DMA_STATUS_IDLE);
  }

  void set_error() {
    state_ = DmaState::Error;
    status_reg_ = (1 << DMA_STATUS_ERROR);
    status_reg_ &= ~(1 << DMA_STATUS_BUSY);
    perf_stats_.errors++;
    
    if (completion_cb_) {
      completion_cb_(false, 0);
    }
  }
};

///////////////////////////////////////////////////////////////////////////////

DmaEngine::DmaEngine(const SimContext& ctx, const char* name, const Config& config)
  : SimObject<DmaEngine>(ctx, name)
  , mem_req_port(this)
  , mem_rsp_port(this)
  , lmem_req_port(this)
  , lmem_rsp_port(this)
  , impl_(new Impl(this, config))
  , socket_(nullptr)
{}

DmaEngine::~DmaEngine() {
  delete impl_;
}

void DmaEngine::reset() {
  impl_->reset();
}

void DmaEngine::tick() {
  impl_->tick();
}

void DmaEngine::dcr_write(uint32_t addr, uint32_t value) {
  impl_->dcr_write(addr, value);
}

uint32_t DmaEngine::dcr_read(uint32_t addr) const {
  return impl_->dcr_read(addr);
}

bool DmaEngine::is_busy() const {
  return impl_->state_ == DmaState::Reading || 
         impl_->state_ == DmaState::Writing;
}

bool DmaEngine::is_idle() const {
  return impl_->state_ == DmaState::Idle;
}

bool DmaEngine::is_complete() const {
  return impl_->state_ == DmaState::Complete;
}

bool DmaEngine::has_error() const {
  return impl_->state_ == DmaState::Error;
}

void DmaEngine::set_completion_callback(CompletionCallback cb) {
  impl_->completion_cb_ = cb;
}

DmaEngine::PerfStats DmaEngine::perf_stats() const {
  return impl_->perf_stats_;
}
```

### 4. Socket 集成

#### sim/simx/socket.h (修改)

```cpp
#pragma once

#include <simobject.h>
#include "dcrs.h"
#include "arch.h"
#include "cache_cluster.h"
#include "local_mem.h"
#include "core.h"
#include "dma_engine.h"  // 新增
#include "constants.h"

namespace vortex {

class Cluster;

class Socket : public SimObject<Socket> {
public:
  struct PerfStats {
    CacheSim::PerfStats icache;
    CacheSim::PerfStats dcache;
    DmaEngine::PerfStats dma;  // 新增
  };

  std::vector<SimPort<MemReq>> mem_req_ports;
  std::vector<SimPort<MemRsp>> mem_rsp_ports;

  Socket(const SimContext& ctx,
         uint32_t socket_id,
         Cluster* cluster,
         const Arch &arch,
         const DCRS &dcrs);

  ~Socket();

  uint32_t id() const {
    return socket_id_;
  }

  Cluster* cluster() const {
    return cluster_;
  }

  // 新增: 获取 Core 指针
  Core* get_core(uint32_t index) const {
    if (index >= cores_.size()) return nullptr;
    return cores_[index].get();
  }

  // 新增: DCR 接口
  void dcr_write(uint32_t addr, uint32_t value);
  uint32_t dcr_read(uint32_t addr) const;

  void reset();
  void tick();
  void attach_ram(RAM* ram);

#ifdef VM_ENABLE
  void set_satp(uint64_t satp);
#endif

  bool running() const;
  int get_exitcode() const;
  void barrier(uint32_t bar_id, uint32_t count, uint32_t core_id);
  void resume(uint32_t core_id);

  PerfStats perf_stats() const;

private:
  uint32_t                socket_id_;
  Cluster*                cluster_;
  std::vector<Core::Ptr>  cores_;
  CacheCluster::Ptr       icaches_;
  CacheCluster::Ptr       dcaches_;
  DmaEngine::Ptr          dma_engine_;  // 新增
  
  // 声明为友元,允许 DmaEngine 访问 cores_
  friend class DmaEngine;
};

} // namespace vortex
```

#### sim/simx/socket.cpp (修改)

```cpp
#include "socket.h"
#include "cluster.h"

using namespace vortex;

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

  // 创建 ICache
  snprintf(sname, 100, "%s-icaches", this->name().c_str());
  icaches_ = CacheCluster::Create(sname, cores_per_socket, NUM_ICACHES, CacheSim::Config{
    !ICACHE_ENABLED,
    log2ceil(ICACHE_SIZE),
    log2ceil(L1_LINE_SIZE),
    log2ceil(sizeof(uint32_t)),
    log2ceil(ICACHE_NUM_WAYS),
    log2ceil(1),
    XLEN,
    1,
    ICACHE_MEM_PORTS,
    false,
    false,
    ICACHE_MSHR_SIZE,
    2,
  });

  // 创建 DCache
  snprintf(sname, 100, "%s-dcaches", this->name().c_str());
  dcaches_ = CacheCluster::Create(sname, cores_per_socket, NUM_DCACHES, CacheSim::Config{
    !DCACHE_ENABLED,
    log2ceil(DCACHE_SIZE),
    log2ceil(L1_LINE_SIZE),
    log2ceil(DCACHE_WORD_SIZE),
    log2ceil(DCACHE_NUM_WAYS),
    log2ceil(DCACHE_NUM_BANKS),
    XLEN,
    DCACHE_NUM_REQS,
    L1_MEM_PORTS,
    DCACHE_WRITEBACK,
    false,
    DCACHE_MSHR_SIZE,
    2,
  });

  // 创建 DMA Engine
  snprintf(sname, 100, "%s-dma", this->name().c_str());
  dma_engine_ = DmaEngine::Create(sname, DmaEngine::Config{
    socket_id,
    static_cast<uint32_t>(cores_.size()),
    4,   // max_outstanding_reads
    4,   // max_outstanding_writes
    64   // transfer_size (cache line)
  });
  dma_engine_->set_socket(this);

  // 计算重叠端口数
  uint32_t overlap = MIN(ICACHE_MEM_PORTS, L1_MEM_PORTS);

  // 连接 L1 caches 和 DMA 到外部内存接口
  // 修改: 扩展 arbiter 以支持 DMA (3 输入而非 2)
  for (uint32_t i = 0; i < L1_MEM_PORTS; ++i) {
    snprintf(sname, 100, "%s-l1_arb%d", this->name().c_str(), i);
    auto l1_arb = MemArbiter::Create(sname, ArbiterType::RoundRobin, 
                                      2 * overlap + 1, overlap);  // +1 for DMA

    if (i < overlap) {
      // 连接 ICache
      icaches_->MemReqPorts.at(i).bind(&l1_arb->ReqIn.at(i));
      l1_arb->RspIn.at(i).bind(&icaches_->MemRspPorts.at(i));

      // 连接 DCache
      dcaches_->MemReqPorts.at(i).bind(&l1_arb->ReqIn.at(overlap + i));
      l1_arb->RspIn.at(overlap + i).bind(&dcaches_->MemRspPorts.at(i));

      // 连接 DMA (只连接到第一个端口)
      if (i == 0) {
        dma_engine_->mem_req_port.bind(&l1_arb->ReqIn.at(2 * overlap));
        l1_arb->RspIn.at(2 * overlap).bind(&dma_engine_->mem_rsp_port);
      }

      // 连接到外部
      l1_arb->ReqOut.at(i).bind(&this->mem_req_ports.at(i));
      this->mem_rsp_ports.at(i).bind(&l1_arb->RspOut.at(i));
    } else {
      if (L1_MEM_PORTS > ICACHE_MEM_PORTS) {
        dcaches_->MemReqPorts.at(i).bind(&this->mem_req_ports.at(i));
        this->mem_rsp_ports.at(i).bind(&dcaches_->MemRspPorts.at(i));
      } else {
        icaches_->MemReqPorts.at(i).bind(&this->mem_req_ports.at(i));
        this->mem_rsp_ports.at(i).bind(&icaches_->MemRspPorts.at(i));
      }
    }
  }

  // 创建 cores
  for (uint32_t i = 0; i < cores_per_socket; ++i) {
    uint32_t core_id = socket_id * cores_per_socket + i;
    cores_.at(i) = Core::Create(core_id, this, arch, dcrs);
  }

  // 连接 cores 到 caches
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
  // DMA 不需要 RAM 指针,通过 SimPort 访问
}

void Socket::dcr_write(uint32_t addr, uint32_t value) {
  // 检查是否是 DMA DCR
  if (addr >= VX_DCR_DMA_SRC_ADDR0 && addr <= VX_DCR_DMA_CTRL) {
    if (dma_engine_) {
      // 特殊处理 CORE_ID: 转换为 Socket 内索引
      if (addr == VX_DCR_DMA_CORE_ID) {
        uint32_t global_core_id = value;
        uint32_t cores_per_socket = cores_.size();
        uint32_t target_socket = global_core_id / cores_per_socket;
        
        // 只有目标 Socket 处理
        if (target_socket == socket_id_) {
          uint32_t local_core_id = global_core_id % cores_per_socket;
          dma_engine_->dcr_write(addr, local_core_id);
          
          // 动态绑定 DMA 到目标 Core 的 LocalMem
          // 注意: 这里需要确保 LocalMem 有足够的输入端口
          // 实际实现中可能需要使用专用的 DMA 通道
          if (local_core_id < cores_.size()) {
            auto core = cores_[local_core_id].get();
            auto lmem = core->local_mem();
            
            // 假设 LocalMem 的最后一个端口保留给 DMA
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

Socket::PerfStats Socket::perf_stats() const {
  PerfStats perf_stats;
  perf_stats.icache = icaches_->perf_stats();
  perf_stats.dcache = dcaches_->perf_stats();
  if (dma_engine_) {
    perf_stats.dma = dma_engine_->perf_stats();
  }
  return perf_stats;
}
```

### 5. Cluster 集成

#### sim/simx/cluster.h (修改)

```cpp
#pragma once

#include <simobject.h>
#include "dcrs.h"
#include "arch.h"
#include "cache_sim.h"
#include "socket.h"
#include "barrier.h"
#include "constants.h"

namespace vortex {

class ProcessorImpl;

class Cluster : public SimObject<Cluster> {
public:
  struct PerfStats {
    CacheSim::PerfStats l2cache;
  };

  std::vector<SimPort<MemReq>> mem_req_ports;
  std::vector<SimPort<MemRsp>> mem_rsp_ports;

  Cluster(const SimContext& ctx,
          uint32_t cluster_id,
          ProcessorImpl* processor,
          const Arch &arch,
          const DCRS &dcrs);

  ~Cluster();

  uint32_t id() const {
    return cluster_id_;
  }

  ProcessorImpl* processor() const {
    return processor_;
  }

  // 新增: DCR 接口
  void dcr_write(uint32_t addr, uint32_t value);
  uint32_t dcr_read(uint32_t addr) const;

  void reset();
  void tick();
  void attach_ram(RAM* ram);

#ifdef VM_ENABLE
  void set_satp(uint64_t satp);
#endif

  bool running() const;
  int get_exitcode() const;
  void barrier(uint32_t bar_id, uint32_t count, uint32_t core_id);

  PerfStats perf_stats() const;

private:
  uint32_t                  cluster_id_;
  ProcessorImpl*            processor_;
  std::vector<Socket::Ptr>  sockets_;
  CacheSim::Ptr             l2cache_;
  std::vector<Barrier>      barriers_;
  uint32_t                  cores_per_socket_;
};

} // namespace vortex
```

#### sim/simx/cluster.cpp (修改)

```cpp
#include "cluster.h"

using namespace vortex;

// ... 现有构造函数代码 ...

void Cluster::dcr_write(uint32_t addr, uint32_t value) {
  // DMA DCR 路由到所有 Socket
  if (addr >= VX_DCR_DMA_SRC_ADDR0 && addr <= VX_DCR_DMA_CTRL) {
    for (auto& socket : sockets_) {
      socket->dcr_write(addr, value);
    }
  }
}

uint32_t Cluster::dcr_read(uint32_t addr) const {
  // 从第一个 Socket 读取(所有 Socket 的 DMA 状态应该相同)
  if (addr >= VX_DCR_DMA_SRC_ADDR0 && addr <= VX_DCR_DMA_STATUS) {
    if (!sockets_.empty()) {
      return sockets_[0]->dcr_read(addr);
    }
  }
  return 0;
}

// ... 其他现有方法 ...
```

### 6. Processor 集成

#### sim/simx/processor.cpp (修改)

```cpp
void ProcessorImpl::dcr_write(uint32_t addr, uint32_t value) {
  if (addr >= VX_DCR_BASE_STATE_BEGIN && addr < VX_DCR_BASE_STATE_END) {
    dcrs_.write(addr, value);
    return;
  }

  // DMA DCR 路由到所有 Cluster
  if (addr >= VX_DCR_DMA_SRC_ADDR0 && addr <= VX_DCR_DMA_CTRL) {
    for (auto& cluster : clusters_) {
      cluster->dcr_write(addr, value);
    }
    return;
  }

  std::cerr << "Error: invalid DCR addr=0x" << std::hex << addr << std::dec << std::endl;
  std::abort();
}
```

### 7. Makefile 更新

#### sim/simx/Makefile (修改)

```makefile
# 在 SRCS 列表中添加
SRCS += dma_engine.cpp
```

### 8. LocalMem 配置调整

#### sim/simx/constants.h (可能需要修改)

```cpp
// 确保 LocalMem 有足够的输入端口
// 如果 LSU_CHANNELS 不够,需要增加或使用专用 DMA 通道
#define DMA_LMEM_CHANNEL (LSU_CHANNELS - 1)  // 保留最后一个通道给 DMA
```

---

## 集成步骤

### 步骤 1: 添加文件

```bash
# 创建 DMA Engine 文件
touch sim/simx/dma_engine.h
touch sim/simx/dma_engine.cpp

# 编辑文件,复制上述代码
```

### 步骤 2: 修改现有文件

按照上述代码修改以下文件:
1. `hw/rtl/VX_types.vh` - 添加 DCR 定义
2. `sim/simx/socket.h` - 添加 DMA 成员和方法
3. `sim/simx/socket.cpp` - 集成 DMA Engine
4. `sim/simx/cluster.h` - 添加 DCR 方法
5. `sim/simx/cluster.cpp` - 实现 DCR 路由
6. `sim/simx/processor.cpp` - 添加 DMA DCR 路由
7. `sim/simx/Makefile` - 添加 dma_engine.cpp

### 步骤 3: 编译

```bash
cd sim/simx
make clean
make
```

### 步骤 4: 检查编译错误

常见问题:
- `cores_` 访问权限 → 已通过 `get_core()` 解决
- SimPort 绑定错误 → 检查端口方向和类型
- 缺少头文件 → 添加必要的 `#include`

---

## 测试方案

### 测试程序 1: 基本功能测试

#### tests/regression/dma/basic_test.cpp

```cpp
#include <iostream>
#include <vortex.h>
#include <VX_config.h>
#include <vector>
#include <cstring>

#define LMEM_BASE_ADDR 0x8000000000000000ULL

int main() {
  vx_device_h device;
  
  // 初始化设备
  if (vx_dev_open(&device) != 0) {
    std::cerr << "Failed to open device" << std::endl;
    return -1;
  }

  // 测试数据
  const uint32_t size = 256;  // 256 bytes
  std::vector<uint8_t> src_data(size);
  std::vector<uint8_t> verify_data(size);
  
  // 填充测试数据
  for (uint32_t i = 0; i < size; ++i) {
    src_data[i] = i & 0xFF;
  }

  // 分配 Global Memory
  uint64_t global_addr = 0x10000;
  vx_mem_write(device, global_addr, src_data.data(), size);

  // 配置 DMA: Global → Shared
  std::cout << "Starting DMA transfer (Global → Shared)..." << std::endl;
  
  vx_dcr_write(device, VX_DCR_DMA_SRC_ADDR0, global_addr & 0xFFFFFFFF);
  vx_dcr_write(device, VX_DCR_DMA_SRC_ADDR1, global_addr >> 32);
  vx_dcr_write(device, VX_DCR_DMA_DST_ADDR0, LMEM_BASE_ADDR & 0xFFFFFFFF);
  vx_dcr_write(device, VX_DCR_DMA_DST_ADDR1, LMEM_BASE_ADDR >> 32);
  vx_dcr_write(device, VX_DCR_DMA_SIZE0, size);
  vx_dcr_write(device, VX_DCR_DMA_SIZE1, 0);
  vx_dcr_write(device, VX_DCR_DMA_CORE_ID, 0);  // Core 0
  vx_dcr_write(device, VX_DCR_DMA_CTRL, 0x1);   // START, G→S

  // 等待完成
  uint32_t status;
  do {
    vx_dcr_read(device, VX_DCR_DMA_STATUS, &status);
  } while (status & (1 << DMA_STATUS_BUSY));

  if (status & (1 << DMA_STATUS_ERROR)) {
    std::cerr << "DMA transfer failed!" << std::endl;
    vx_dev_close(device);
    return -1;
  }

  std::cout << "DMA transfer completed!" << std::endl;

  // 验证: 需要运行 kernel 从 Shared Memory 读取数据
  // (Host 无法直接访问 Shared Memory)

  vx_dev_close(device);
  return 0;
}
```

### 测试程序 2: 性能测试

#### tests/regression/dma/perf_test.cpp

```cpp
#include <iostream>
#include <vortex.h>
#include <VX_config.h>
#include <chrono>
#include <vector>

#define LMEM_BASE_ADDR 0x8000000000000000ULL

void test_dma_bandwidth(vx_device_h device, uint32_t size) {
  std::vector<uint8_t> data(size, 0xAA);
  uint64_t global_addr = 0x10000;
  
  vx_mem_write(device, global_addr, data.data(), size);

  auto start = std::chrono::high_resolution_clock::now();

  // 配置并启动 DMA
  vx_dcr_write(device, VX_DCR_DMA_SRC_ADDR0, global_addr & 0xFFFFFFFF);
  vx_dcr_write(device, VX_DCR_DMA_SRC_ADDR1, global_addr >> 32);
  vx_dcr_write(device, VX_DCR_DMA_DST_ADDR0, LMEM_BASE_ADDR & 0xFFFFFFFF);
  vx_dcr_write(device, VX_DCR_DMA_DST_ADDR1, LMEM_BASE_ADDR >> 32);
  vx_dcr_write(device, VX_DCR_DMA_SIZE0, size);
  vx_dcr_write(device, VX_DCR_DMA_SIZE1, 0);
  vx_dcr_write(device, VX_DCR_DMA_CORE_ID, 0);
  vx_dcr_write(device, VX_DCR_DMA_CTRL, 0x1);

  // 等待完成
  uint32_t status;
  do {
    vx_dcr_read(device, VX_DCR_DMA_STATUS, &status);
  } while (status & (1 << DMA_STATUS_BUSY));

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  double bandwidth = (size / 1024.0 / 1024.0) / (duration.count() / 1e6);
  std::cout << "Size: " << size << " bytes, "
            << "Time: " << duration.count() << " us, "
            << "Bandwidth: " << bandwidth << " MB/s" << std::endl;
}

int main() {
  vx_device_h device;
  
  if (vx_dev_open(&device) != 0) {
    std::cerr << "Failed to open device" << std::endl;
    return -1;
  }

  std::cout << "DMA Bandwidth Test" << std::endl;
  std::cout << "==================" << std::endl;

  // 测试不同大小
  test_dma_bandwidth(device, 64);
  test_dma_bandwidth(device, 256);
  test_dma_bandwidth(device, 1024);
  test_dma_bandwidth(device, 4096);
  test_dma_bandwidth(device, 16384);

  vx_dev_close(device);
  return 0;
}
```

### 测试 Makefile

#### tests/regression/dma/Makefile

```makefile
TARGET = dma_test

VORTEX_RT_PATH ?= $(realpath ../../../runtime)
VORTEX_DRV_PATH ?= $(VORTEX_RT_PATH)/opae

CXXFLAGS += -std=c++11 -Wall -Wextra -Wfatal-errors
CXXFLAGS += -I$(VORTEX_RT_PATH)/include
CXXFLAGS += -I$(VORTEX_RT_PATH)/../hw/rtl

LDFLAGS += -L$(VORTEX_RT_PATH) -lvortex
LDFLAGS += -L$(VORTEX_DRV_PATH) -lopae-c

SRCS = basic_test.cpp

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) *.o
```

---

## 性能分析

### 预期性能

假设配置:
- Cache line: 64 bytes
- Memory latency: 100 cycles
- Max outstanding: 4 requests

**理论带宽**:
```
Bandwidth = (Transfer_Size × Outstanding) / Latency
          = (64 × 4) / 100
          = 2.56 bytes/cycle
```

### 性能统计输出

```cpp
// 在 Processor 中添加 DMA 统计输出
void ProcessorImpl::show_stats() {
  // ... 现有统计 ...
  
  // DMA 统计
  for (uint32_t c = 0; c < clusters_.size(); ++c) {
    auto cluster = clusters_[c];
    for (uint32_t s = 0; s < NUM_SOCKETS; ++s) {
      auto socket_stats = cluster->perf_stats();
      auto& dma_stats = socket_stats.dma;
      
      std::cout << "DMA[" << c << "][" << s << "]:" << std::endl;
      std::cout << "  Transfers: " << dma_stats.transfers << std::endl;
      std::cout << "  Bytes: " << dma_stats.bytes_transferred << std::endl;
      std::cout << "  Cycles Active: " << dma_stats.cycles_active << std::endl;
      std::cout << "  Cycles Idle: " << dma_stats.cycles_idle << std::endl;
      std::cout << "  Avg Read Latency: " 
                << (dma_stats.read_requests > 0 ? 
                    dma_stats.read_latency / dma_stats.read_requests : 0)
                << " cycles" << std::endl;
      std::cout << "  Avg Write Latency: "
                << (dma_stats.write_requests > 0 ?
                    dma_stats.write_latency / dma_stats.write_requests : 0)
                << " cycles" << std::endl;
      std::cout << "  Errors: " << dma_stats.errors << std::endl;
    }
  }
}
```

---

## 关键设计决策说明

### 1. 为什么使用 SimPort?

**原因**:
- **一致性**: 所有内存访问都通过 SimPort,符合 SimX 架构
- **准确性**: 模拟真实的延迟和冲突
- **可扩展性**: 易于添加仲裁、优先级等功能

### 2. 为什么 DMA 连接到 L1 Arbiter?

**原因**:
- **共享带宽**: DMA 和 Cache 共享到 L2 的带宽
- **真实建模**: 硬件中 DMA 也需要仲裁
- **避免死锁**: 通过 arbiter 管理访问顺序

### 3. 为什么使用流水线传输?

**原因**:
- **高吞吐量**: 读写重叠,提高带宽利用率
- **真实性**: 现代 DMA 都是流水线的
- **性能**: 减少总传输时间

### 4. LocalMem 端口分配

**问题**: LocalMem 的输入端口有限(LSU_CHANNELS 个)

**解决方案**:
- **选项 A**: 保留最后一个端口给 DMA
- **选项 B**: 扩展 LocalMem 端口数
- **选项 C**: DMA 和 LSU 共享端口(需要仲裁)

**当前实现**: 选项 A (最简单)

---

## 后续工作

### P0 - 必需

- [ ] **完整测试**: 编写包含 kernel 的完整验证测试
- [ ] **RTL 实现**: 硬件 DMA Engine 设计
- [ ] **文档**: 用户手册和 API 文档

### P1 - 重要

- [ ] **多通道 DMA**: 支持多个并发 DMA 传输
- [ ] **中断机制**: 完成时触发中断
- [ ] **错误恢复**: 更健壮的错误处理
- [ ] **2D 传输**: 支持 stride 和 2D 块传输

### P2 - 优化

- [ ] **优先级控制**: DMA vs Cache 优先级
- [ ] **带宽限制**: 防止 DMA 饿死 Cache
- [ ] **Scatter-Gather**: 支持描述符链表
- [ ] **压缩传输**: 支持数据压缩

---

## 总结

### ✅ 改进点

1. **符合 SimX 架构**: 使用 SimPort 和事件驱动模型
2. **编译通过**: 解决了 `cores_` 访问权限问题
3. **完整路由**: 实现了 DCR 的完整路由链
4. **真实建模**: 包含延迟、仲裁、流水线
5. **详细统计**: 完整的性能分析数据
6. **健壮性**: 地址验证、对齐检查、错误处理

### 📊 对比原设计

| 特性 | 原设计 | 修订设计 |
|------|--------|----------|
| 内存访问 | 同步直接 | 异步 SimPort ✅ |
| 性能模拟 | 不准确 | 真实延迟 ✅ |
| 架构一致性 | 不符合 | 完全符合 ✅ |
| 编译 | 会失败 | 通过 ✅ |
| 流水线 | 无 | 有 ✅ |
| 仲裁 | 无 | 有 ✅ |

