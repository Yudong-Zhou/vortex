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

// DMA DCR 地址定义 (与 VX_types.vh 保持一致)
#define VX_DCR_DMA_SRC_ADDR0     0x006
#define VX_DCR_DMA_SRC_ADDR1     0x007
#define VX_DCR_DMA_DST_ADDR0     0x008
#define VX_DCR_DMA_DST_ADDR1     0x009
#define VX_DCR_DMA_SIZE0         0x00A
#define VX_DCR_DMA_SIZE1         0x00B
#define VX_DCR_DMA_CORE_ID       0x00C
#define VX_DCR_DMA_CTRL          0x00D
#define VX_DCR_DMA_STATUS        0x00E

// DMA 控制寄存器位定义
#define DMA_CTRL_START           0
#define DMA_CTRL_DIR             1
#define DMA_CTRL_IRQ_EN          2

// DMA 状态寄存器位定义
#define DMA_STATUS_IDLE          0
#define DMA_STATUS_BUSY          1
#define DMA_STATUS_DONE          2
#define DMA_STATUS_ERROR         3

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
      
      uint32_t transfer_size = std::min((uint64_t)config_.transfer_size, remaining_size_);
      
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

