# DMA 完整实现计划

## 📋 总体目标

基于指令扩展方式（方案B）实现 DMA Engine，参考 dot8 extension 的设计流程。

## 🎯 设计原则

1. ✅ **无冲突**: 使用 funct7=3, funct3=0，已验证无冲突
2. ✅ **参考 dot8**: 遵循 assignment5.md 的实现流程
3. ✅ **吸取教训**: 避免之前的 DCR 地址冲突、内存仲裁等问题
4. ✅ **逐步验证**: 每个阶段完成后进行测试
5. ✅ **可回退**: 所有修改都有清晰的注释和文档

---

## 📊 实现阶段概览

| 阶段 | 任务 | 预计文件数 | 关键点 |
|------|------|-----------|--------|
| **Phase 1** | ISA Extension | 1 | 指令定义 |
| **Phase 2** | SimX Types | 2 | 类型定义 |
| **Phase 3** | SimX Decode | 1 | 指令解码 |
| **Phase 4** | DMA Engine | 2 | 核心逻辑 |
| **Phase 5** | Socket Integration | 2 | 集成到 Socket |
| **Phase 6** | Execute Logic | 1 | 执行触发 |
| **Phase 7** | Test Program | 4 | 测试验证 |
| **Phase 8** | Testing & Debug | - | 功能验证 |

---

# Phase 1: ISA Extension (指令扩展)

## 目标
定义 VX_DMA 指令的用户接口

## 文件修改

### 1.1 `kernel/include/vx_intrinsics.h`

**位置**: 文件末尾，`#endif // __VX_INTRINSICS_H__` 之前

**添加内容**:
```c
///////////////////////////////////////////////////////////////////////////////
// DMA Transfer Instructions
///////////////////////////////////////////////////////////////////////////////

// DMA transfer directions
#define DMA_DIR_G2L 0  // Global to Local Memory
#define DMA_DIR_L2G 1  // Local to Global Memory

// DMA transfer (generic)
inline void vx_dma_transfer(void* dst, void* src, size_t size, int direction) {
    asm volatile (
        ".insn r 0x0B, 0x0, 0x3, %0, %1, %2"
        : 
        : "r"(dst), "r"(src), "r"(size | ((size_t)direction << 31))
        : "memory"
    );
}

// DMA Global to Local
inline void vx_dma_g2l(void* local_dst, void* global_src, size_t size) {
    vx_dma_transfer(local_dst, global_src, size, DMA_DIR_G2L);
}

// DMA Local to Global
inline void vx_dma_l2g(void* global_dst, void* local_src, size_t size) {
    vx_dma_transfer(global_dst, local_src, size, DMA_DIR_L2G);
}
```

**验证**:
- [ ] 编译 kernel 库: `cd kernel && make clean && make`
- [ ] 确认无编译错误

---

# Phase 2: SimX Types (类型定义)

## 目标
在 SimX 中定义 DMA 相关的类型和枚举

## 文件修改

### 2.1 `sim/simx/types.h`

**位置 1**: 在 `enum class WctlType` 之后添加 DMA 类型枚举

```cpp
// DMA types
enum class DmaType {
  TRANSFER,  // DMA transfer operation
};

inline std::ostream& operator<<(std::ostream& os, const DmaType& type) {
  switch (type) {
  case DmaType::TRANSFER: os << "TRANSFER"; break;
  default: os << "?"; break;
  }
  return os;
}
```

**位置 2**: 在 `struct IntrWctlArgs` 之后添加 DMA 参数结构

```cpp
// DMA instruction arguments
struct IntrDmaArgs {
  // No additional arguments needed
  // Direction and size are encoded in rs2
};
```

**位置 3**: 在 `using OpType = std::variant<...>` 中添加 DmaType

找到这一行：
```cpp
using OpType = std::variant<
  AluType,
  BrType,
  LsuType,
  // ... 其他类型
  WctlType
#ifdef EXT_V_ENABLE
  , VsetType
  , VlsType
  , VopType
#endif
#ifdef EXT_TCU_ENABLE
  , TcuType
#endif
>;
```

修改为：
```cpp
using OpType = std::variant<
  AluType,
  BrType,
  LsuType,
  // ... 其他类型
  WctlType,
  DmaType  // 添加 DMA 类型
#ifdef EXT_V_ENABLE
  , VsetType
  , VlsType
  , VopType
#endif
#ifdef EXT_TCU_ENABLE
  , TcuType
#endif
>;
```

**位置 4**: 在 `using IntrArgs = std::variant<...>` 中添加 IntrDmaArgs

找到这一行并添加 `IntrDmaArgs`：
```cpp
using IntrArgs = std::variant<
  IntrAluArgs,
  // ... 其他类型
  IntrWctlArgs,
  IntrDmaArgs  // 添加 DMA 参数
#ifdef EXT_V_ENABLE
  , IntrVsetArgs
  , IntrVlsArgs
  , IntrVopArgs
#endif
#ifdef EXT_TCU_ENABLE
  , IntrTcuArgs
#endif
>;
```

**验证**:
- [ ] 编译 SimX: `cd sim/simx && make clean && make`
- [ ] 确认 DmaType 和 IntrDmaArgs 正确定义

---

# Phase 3: SimX Decode (指令解码)

## 目标
在 SimX 中解码 VX_DMA 指令

## 文件修改

### 3.1 `sim/simx/decode.cpp`

**位置 1**: 在 `op_string()` 函数中添加 DMA 字符串输出

找到 `static op_string_t op_string(const Instr &instr)` 函数，在 `WctlType` 的 lambda 之后添加：

```cpp
[&](DmaType dma_type)-> op_string_t {
  switch (dma_type) {
  case DmaType::TRANSFER: return {"DMA.TRANSFER", ""};
  default:
    std::abort();
  }
},
```

**位置 2**: 在 `Emulator::decode()` 函数中添加 DMA 解码逻辑

找到 `case Opcode::EXT1:` 的 switch 语句，在 `case 2:` (TCU) 之后添加：

```cpp
case 3: { // DMA
  switch (funct3) {
  case 0: { // DMA Transfer
    auto instr = std::allocate_shared<Instr>(instr_pool_, uuid, FUType::SFU);
    instr->setOpType(DmaType::TRANSFER);
    instr->setArgs(IntrDmaArgs{});
    instr->setDestReg(rd, RegType::Integer);    // dst address
    instr->setSrcReg(0, rs1, RegType::Integer); // src address
    instr->setSrcReg(1, rs2, RegType::Integer); // size + direction
    ibuffer.push_back(instr);
  } break;
  default:
    std::abort();
  }
} break;
```

**验证**:
- [ ] 编译 SimX: `cd sim/simx && make clean && make`
- [ ] 确认解码逻辑正确添加

---

# Phase 4: DMA Engine Implementation (DMA 引擎实现)

## 目标
实现 DMA Engine 的核心逻辑

## 文件创建/修改

### 4.1 `sim/simx/dma_engine.h`

**创建新文件**，内容如下：

**⚠️ 关键修正点**：
1. ✅ 简化为同步模型，不需要复杂的状态机
2. ✅ 添加 RAM 指针用于直接访问
3. ✅ 参考 dot8 的简单接口设计

```cpp
// Copyright © 2019-2023
// DMA Engine for Vortex GPU

#pragma once

#include <simobject.h>
#include <vector>
#include <queue>
#include "types.h"

namespace vortex {

// Forward declarations
class Socket;
class RAM;

class DmaEngine : public SimObject<DmaEngine> {
public:
  struct Config {
    uint32_t socket_id;
    uint32_t num_cores;
    uint32_t max_outstanding_reads;
    uint32_t max_outstanding_writes;
    uint32_t transfer_size;  // bytes per transfer
  };

  struct PerfStats {
    uint64_t g2l_transfers;
    uint64_t l2g_transfers;
    uint64_t total_bytes;
    uint64_t total_cycles;
    
    PerfStats()
      : g2l_transfers(0)
      , l2g_transfers(0)
      , total_bytes(0)
      , total_cycles(0)
    {}
  };

  // DMA state machine (简化版)
  enum class DmaState {
    IDLE,
    BUSY,
    DONE
  };

  // Memory ports (保留用于未来扩展)
  SimPort<MemReq> mem_req_port;
  SimPort<MemRsp> mem_rsp_port;

  DmaEngine(const SimContext& ctx, const char* name, const Config& config);
  ~DmaEngine();

  void reset();
  void tick();
  
  void attach_ram(RAM* ram);

  void set_socket(Socket* socket) {
    socket_ = socket;
  }

  // 同步执行 DMA 传输（参考 dot8 的执行模型）
  void execute_transfer(uint64_t dst_addr, uint64_t src_addr, 
                       uint32_t size, uint32_t direction, uint32_t core_id);

  const PerfStats& perf_stats() const {
    return perf_stats_;
  }

private:
  struct TransferRequest {
    uint64_t dst_addr;
    uint64_t src_addr;
    uint32_t size;
    uint32_t direction;  // 0=G2L, 1=L2G
    uint32_t core_id;
    DmaState state;
    
    TransferRequest()
      : dst_addr(0), src_addr(0), size(0), direction(0)
      , core_id(0), state(DmaState::IDLE)
    {}
  };

  Config config_;
  Socket* socket_;
  RAM* ram_;  // 直接访问 RAM
  PerfStats perf_stats_;
  
  std::queue<TransferRequest> pending_requests_;
  TransferRequest current_transfer_;
  
  uint32_t outstanding_reads_;
  uint32_t outstanding_writes_;
  
  std::queue<uint64_t> read_data_queue_;

  bool is_local_address(uint64_t addr) const;
};

} // namespace vortex
```

### 4.2 `sim/simx/dma_engine.cpp`

**创建新文件**，实现 DMA Engine 逻辑：

**⚠️ 关键修正点**：
1. ✅ 使用 RAM 直接访问而非 SimPort（避免复杂的异步处理）
2. ✅ 同步执行（指令完成即传输完成）
3. ✅ 参考 dot8 的简单执行模型

```cpp
// Copyright © 2019-2023
// DMA Engine Implementation

#include "dma_engine.h"
#include "socket.h"
#include "core.h"
#include "debug.h"
#include "mem.h"
#include <VX_config.h>
#include <cstring>

using namespace vortex;

DmaEngine::DmaEngine(const SimContext& ctx, const char* name, const Config& config)
  : SimObject(ctx, name)
  , mem_req_port(this)
  , mem_rsp_port(this)
  , config_(config)
  , socket_(nullptr)
  , outstanding_reads_(0)
  , outstanding_writes_(0)
{
  current_transfer_.state = DmaState::IDLE;
}

DmaEngine::~DmaEngine() {}

void DmaEngine::reset() {
  while (!pending_requests_.empty()) {
    pending_requests_.pop();
  }
  while (!read_data_queue_.empty()) {
    read_data_queue_.pop();
  }
  current_transfer_.state = DmaState::IDLE;
  outstanding_reads_ = 0;
  outstanding_writes_ = 0;
}

void DmaEngine::attach_ram(RAM* ram) {
  ram_ = ram;
}

// 同步执行 DMA 传输（参考 dot8 的同步执行模型）
void DmaEngine::execute_transfer(uint64_t dst_addr, uint64_t src_addr, 
                                 uint32_t size, uint32_t direction, uint32_t core_id) {
  DT(3, this->name() << " DMA transfer: dst=0x" << std::hex << dst_addr 
     << ", src=0x" << src_addr << ", size=" << std::dec << size 
     << ", dir=" << direction << ", core=" << core_id);
  
  // Update stats
  if (direction == 0) {
    perf_stats_.g2l_transfers++;
  } else {
    perf_stats_.l2g_transfers++;
  }
  perf_stats_.total_bytes += size;
  
  // 使用临时缓冲区进行传输
  std::vector<uint8_t> buffer(size);
  
  // 读取源数据
  if (ram_) {
    ram_->read(buffer.data(), src_addr, size);
    DT(4, this->name() << " DMA read from 0x" << std::hex << src_addr);
  }
  
  // 写入目标地址
  if (ram_) {
    ram_->write(buffer.data(), dst_addr, size);
    DT(4, this->name() << " DMA write to 0x" << std::hex << dst_addr);
  }
  
  DT(3, this->name() << " DMA transfer complete");
}

void DmaEngine::tick() {
  perf_stats_.total_cycles++;
  // 同步模式下，tick 主要用于统计
}

bool DmaEngine::is_local_address(uint64_t addr) const {
  return get_addr_type(addr) == AddrType::Shared;
}
```

### 4.3 `sim/simx/Makefile` ⚠️ **关键修正 - 之前遗漏！**

**位置**: 在 SRCS 定义中添加

找到这一行：
```makefile
SRCS += $(SRC_DIR)/dcrs.cpp $(SRC_DIR)/types.cpp
```

在其后添加：
```makefile
SRCS += $(SRC_DIR)/dma_engine.cpp
```

**验证**:
- [ ] 编译 SimX: `cd sim/simx && make clean && make`
- [ ] 确认 DMA Engine 编译成功
- [ ] 确认 dma_engine.o 被生成

---

# Phase 5: Socket Integration (集成到 Socket)

## 目标
将 DMA Engine 集成到 Socket，连接内存仲裁器

## 文件修改

### 5.1 `sim/simx/socket.h`

**位置 1**: 添加头文件包含

在文件顶部的 include 部分添加：
```cpp
#include "dma_engine.h"
```

**位置 2**: 在 `PerfStats` 结构中添加 DMA 统计

```cpp
struct PerfStats {
  CacheSim::PerfStats icache;
  CacheSim::PerfStats dcache;
  DmaEngine::PerfStats dma;  // 添加 DMA 性能统计
};
```

**位置 3**: 在 private 成员中添加 DMA Engine

```cpp
private:
  // ... 其他成员
  DmaEngine::Ptr dma_engine_;
  
  friend class DmaEngine;  // Allow DMA to access Socket internals
```

**位置 4**: 添加公共方法供 execute.cpp 访问

**⚠️ 关键修正 - 解决访问权限问题！**

```cpp
public:
  // ... 其他公共方法
  
  // Get core pointer (for DMA Engine)
  Core* get_core(uint32_t index) const {
    if (index >= cores_.size()) return nullptr;
    return cores_[index].get();
  }
  
  // ⚠️ 新增：提供 DMA 访问接口（避免直接访问 private 成员）
  void trigger_dma_transfer(uint64_t dst_addr, uint64_t src_addr, 
                           uint32_t size, uint32_t direction, uint32_t core_id) {
    if (dma_engine_) {
      dma_engine_->execute_transfer(dst_addr, src_addr, size, direction, core_id);
    }
  }
```

### 5.2 `sim/simx/socket.cpp`

**位置 1**: 在构造函数中创建 DMA Engine

找到 Socket 构造函数，在创建 dcaches 之后添加：

```cpp
// Create DMA Engine
snprintf(sname, 100, "%s-dma", this->name().c_str());
DmaEngine::Config dma_cfg;
dma_cfg.socket_id = socket_id;
dma_cfg.num_cores = static_cast<uint32_t>(cores_.size());
dma_cfg.max_outstanding_reads = 4;
dma_cfg.max_outstanding_writes = 4;
dma_cfg.transfer_size = 64;  // 64 bytes per transfer

dma_engine_ = DmaEngine::Create(sname, dma_cfg);
dma_engine_->set_socket(this);
```

**位置 2**: 修改 L1 仲裁器以包含 DMA

**⚠️ 关键修正 - 避免之前的仲裁器绑定错误！**

找到创建 l1_arb 的代码，**只为第一个仲裁器扩展输入**：

```cpp
// connect l1 caches and DMA to outgoing memory interfaces
for (uint32_t i = 0; i < L1_MEM_PORTS; ++i) {
  snprintf(sname, 100, "%s-l1_arb%d", this->name().c_str(), i);
  
  // ⚠️ 关键修正：只有第一个仲裁器需要额外的 DMA 输入
  uint32_t num_inputs = (i == 0) ? (2 * overlap + 1) : (2 * overlap);
  auto l1_arb = MemArbiter::Create(sname, ArbiterType::RoundRobin, num_inputs, overlap);
  
  if (i < overlap) {
    icaches_->MemReqPorts.at(i).bind(&l1_arb->ReqIn.at(i));
    l1_arb->RspIn.at(i).bind(&icaches_->MemRspPorts.at(i));

    dcaches_->MemReqPorts.at(i).bind(&l1_arb->ReqIn.at(overlap + i));
    l1_arb->RspIn.at(overlap + i).bind(&dcaches_->MemRspPorts.at(i));
    
    // ⚠️ 只在第一个仲裁器绑定 DMA（避免未绑定的输入）
    if (i == 0) {
      dma_engine_->mem_req_port.bind(&l1_arb->ReqIn.at(2 * overlap));
      l1_arb->RspIn.at(2 * overlap).bind(&dma_engine_->mem_rsp_port);
    }

    l1_arb->ReqOut.at(i).bind(&this->mem_req_ports.at(i));
    this->mem_rsp_ports.at(i).bind(&l1_arb->RspOut.at(i));
  } else {
    // ⚠️ 保留原有的 else 分支逻辑
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
```

**位置 3**: 在 reset() 中重置 DMA

```cpp
void Socket::reset() {
  if (dma_engine_) {
    dma_engine_->reset();
  }
}
```

**位置 4**: 在 tick() 中更新 DMA

```cpp
void Socket::tick() {
  if (dma_engine_) {
    dma_engine_->tick();
  }
}
```

**位置 5**: 在 perf_stats() 中收集 DMA 统计

```cpp
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

**位置 6**: 在 attach_ram() 中连接 RAM 到 DMA

**⚠️ 关键修正 - DMA 需要访问 RAM！**

找到 `Socket::attach_ram(RAM* ram)` 方法，添加：

```cpp
void Socket::attach_ram(RAM* ram) {
  // ... 原有代码 ...
  
  // ⚠️ 新增：DMA Engine 也需要访问 RAM
  if (dma_engine_) {
    dma_engine_->attach_ram(ram);
  }
}
```

**验证**:
- [ ] 编译 SimX: `cd sim/simx && make clean && make`
- [ ] 确认 Socket 集成成功

---

# Phase 6: Execute Logic (执行逻辑)

## 目标
在 execute 阶段触发 DMA 传输

## 文件修改

### 6.1 `sim/simx/execute.cpp`

**位置**: 在 `Emulator::execute()` 函数的大 switch 语句中添加 DMA 处理

**⚠️ 关键修正 - 使用公共接口访问 DMA！**

找到处理 WctlType 的代码块，在其之后添加：

```cpp
[&](DmaType dma_type) {
  switch (dma_type) {
  case DmaType::TRANSFER: {
    // Extract parameters from registers (只从第一个线程获取参数)
    uint64_t dst_addr = rd_data[0].u;
    uint64_t src_addr = rs1_data[0].u;
    uint32_t size_dir = rs2_data[0].u;
    
    // Extract size and direction
    uint32_t size = size_dir & 0x7FFFFFFF;
    uint32_t direction = (size_dir >> 31) & 0x1;
    
    // ⚠️ 修正：通过 Socket 的公共接口触发 DMA（避免访问 private 成员）
    auto socket = core_->socket();
    socket->trigger_dma_transfer(dst_addr, src_addr, size, direction, core_->id());
    
    DP(3, "DMA.TRANSFER: dst=0x" << std::hex << dst_addr 
       << ", src=0x" << src_addr << ", size=" << std::dec << size 
       << ", dir=" << direction << ", cid=" << core_->id());
  } break;
  default:
    std::abort();
  }
},
```

**验证**:
- [ ] 编译 SimX: `cd sim/simx && make clean && make`
- [ ] 确认执行逻辑正确
- [ ] 确认无访问权限错误

---

# Phase 7: Test Program (测试程序)

## 目标
创建 DMA 测试程序

## 文件创建

### 7.1 目录结构

```
tests/regression/dma/
├── Makefile
├── main.cpp
├── kernel.cpp
└── common.h
```

### 7.2 `tests/regression/dma/Makefile`

```makefile
XLEN ?= 32
TOOLDIR ?= $(VORTEX_HOME)/toolchain/llvm-riscv/bin

PROJECT = dma

SRCS = main.cpp kernel.cpp

include ../common.mk
```

### 7.3 `tests/regression/dma/common.h`

```cpp
#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

#define DMA_DIR_G2L 0
#define DMA_DIR_L2G 1

typedef struct {
  uint32_t size;
  uint64_t src_addr;
  uint64_t dst_addr;
  uint64_t ref_addr;
} kernel_arg_t;

#endif
```

### 7.4 `tests/regression/dma/kernel.cpp`

```cpp
#include <stdint.h>
#include <vx_intrinsics.h>
#include <vx_spawn.h>
#include "common.h"

void kernel_body(int task_id, kernel_arg_t* arg) {
  int core_id = vx_core_id();
  
  // Only core 0 performs DMA
  if (core_id != 0)
    return;
  
  int8_t* global_src = (int8_t*)arg->src_addr;
  int8_t* local_mem = (int8_t*)__local_mem();
  int8_t* global_dst = (int8_t*)arg->dst_addr;
  uint32_t size = arg->size;
  
  vx_printf("Core %d: Starting DMA test, size=%d\n", core_id, size);
  
  // Test 1: Global to Local
  vx_printf("Core %d: DMA G2L transfer\n", core_id);
  vx_dma_g2l(local_mem, global_src, size);
  
  // Wait for DMA completion (simplified - in real implementation might need status check)
  vx_fence();
  
  // Verify data in local memory
  vx_printf("Core %d: Verifying local memory\n", core_id);
  for (uint32_t i = 0; i < size; ++i) {
    if (local_mem[i] != global_src[i]) {
      vx_printf("Error: local_mem[%d]=%d, expected=%d\n", 
                i, local_mem[i], global_src[i]);
    }
  }
  
  // Test 2: Local to Global
  vx_printf("Core %d: DMA L2G transfer\n", core_id);
  vx_dma_l2g(global_dst, local_mem, size);
  
  vx_fence();
  
  vx_printf("Core %d: DMA test complete\n", core_id);
}

int main() {
  kernel_arg_t* arg = (kernel_arg_t*)csr_read(VX_CSR_MSCRATCH);
  vx_spawn_tasks(1, (vx_spawn_tasks_cb)kernel_body, arg);
  return 0;
}
```

### 7.5 `tests/regression/dma/main.cpp`

```cpp
#include <iostream>
#include <vector>
#include <unistd.h>
#include <string.h>
#include <vortex.h>
#include "common.h"

#define RT_CHECK(__expr)                                         \
   do {                                                          \
     int __ret = __expr;                                         \
     if (__ret != 0) {                                           \
       printf("Error: '%s' returned %d!\n", #__expr, __ret);    \
       cleanup();                                                \
       exit(-1);                                                 \
     }                                                           \
   } while (false)

vx_device_h device = nullptr;
vx_buffer_h src_buffer = nullptr;
vx_buffer_h dst_buffer = nullptr;
vx_buffer_h krnl_buffer = nullptr;
vx_buffer_h args_buffer = nullptr;
kernel_arg_t kernel_arg = {};

static void cleanup() {
  if (src_buffer) vx_buf_release(src_buffer);
  if (dst_buffer) vx_buf_release(dst_buffer);
  if (krnl_buffer) vx_buf_release(krnl_buffer);
  if (args_buffer) vx_buf_release(args_buffer);
  if (device) vx_dev_close(device);
}

int main(int argc, char *argv[]) {
  // Parse arguments
  uint32_t size = 64;  // Default size
  if (argc > 1) {
    size = atoi(argv[1]);
  }
  
  std::cout << "DMA Test: size=" << size << " bytes" << std::endl;
  
  // Initialize device
  RT_CHECK(vx_dev_open(&device));
  
  // Allocate buffers
  std::vector<int8_t> h_src(size);
  std::vector<int8_t> h_dst(size, 0);
  
  // Initialize source data
  for (uint32_t i = 0; i < size; ++i) {
    h_src[i] = static_cast<int8_t>(i & 0xFF);
  }
  
  // Allocate device buffers
  RT_CHECK(vx_buf_alloc(device, size, &src_buffer));
  RT_CHECK(vx_buf_alloc(device, size, &dst_buffer));
  
  // Upload source data
  RT_CHECK(vx_copy_to_dev(src_buffer, h_src.data(), 0, size));
  
  // Setup kernel arguments
  kernel_arg.size = size;
  kernel_arg.src_addr = vx_buf_address(src_buffer);
  kernel_arg.dst_addr = vx_buf_address(dst_buffer);
  
  RT_CHECK(vx_buf_alloc(device, sizeof(kernel_arg_t), &args_buffer));
  RT_CHECK(vx_copy_to_dev(args_buffer, &kernel_arg, 0, sizeof(kernel_arg_t)));
  
  // Upload kernel
  RT_CHECK(vx_upload_kernel_file(device, "kernel.bin", &krnl_buffer));
  
  // Run kernel
  RT_CHECK(vx_start(device, krnl_buffer));
  RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));
  
  // Download results
  RT_CHECK(vx_copy_from_dev(h_dst.data(), dst_buffer, 0, size));
  
  // Verify results
  int errors = 0;
  for (uint32_t i = 0; i < size; ++i) {
    if (h_dst[i] != h_src[i]) {
      if (errors < 10) {
        std::cout << "Error at " << i << ": got " << (int)h_dst[i] 
                  << ", expected " << (int)h_src[i] << std::endl;
      }
      errors++;
    }
  }
  
  if (errors == 0) {
    std::cout << "PASSED!" << std::endl;
  } else {
    std::cout << "FAILED! " << errors << " errors" << std::endl;
  }
  
  cleanup();
  return errors;
}
```

**验证**:
- [ ] 编译测试: `cd tests/regression/dma && make`
- [ ] 确认测试程序编译成功

---

# Phase 8: Testing & Debugging (测试和调试)

## 测试步骤

### 8.1 编译完整系统

```bash
cd $VORTEX_HOME
make clean
make
```

### 8.2 运行 DMA 测试

```bash
cd tests/regression/dma
make run-simx
```

### 8.3 预期输出

```
DMA Test: size=64 bytes
Core 0: Starting DMA test, size=64
Core 0: DMA G2L transfer
Core 0: Verifying local memory
Core 0: DMA L2G transfer
Core 0: DMA test complete
PASSED!
```

### 8.4 调试检查点

如果测试失败，按以下顺序检查：

1. **指令解码**: 确认 VX_DMA 指令被正确解码
   - 检查 decode.cpp 的 debug 输出
   
2. **DMA Engine 创建**: 确认 DMA Engine 被正确创建
   - 检查 socket.cpp 的构造函数
   
3. **内存仲裁**: 确认 DMA 正确连接到仲裁器
   - 检查 l1_arb 的输入数量
   
4. **执行触发**: 确认 execute.cpp 正确调用 DMA
   - 添加 debug 输出查看参数
   
5. **数据传输**: 确认 DMA 正确发送内存请求
   - 检查 mem_req_port 和 mem_rsp_port

---

# 📊 实现检查清单

## Phase 1: ISA Extension
- [ ] vx_intrinsics.h 添加 DMA 指令定义
- [ ] 编译 kernel 库成功

## Phase 2: SimX Types
- [ ] types.h 添加 DmaType 枚举
- [ ] types.h 添加 IntrDmaArgs 结构
- [ ] types.h 更新 OpType variant
- [ ] types.h 更新 IntrArgs variant
- [ ] 编译 SimX 成功

## Phase 3: SimX Decode
- [ ] decode.cpp 添加 op_string 支持
- [ ] decode.cpp 添加解码逻辑
- [ ] 编译 SimX 成功

## Phase 4: DMA Engine
- [ ] 创建 dma_engine.h
- [ ] 创建 dma_engine.cpp
- [ ] 编译 SimX 成功

## Phase 5: Socket Integration
- [ ] socket.h 添加 DMA Engine 成员
- [ ] socket.cpp 创建 DMA Engine
- [ ] socket.cpp 连接内存仲裁器
- [ ] socket.cpp 实现 reset/tick/perf_stats
- [ ] 编译 SimX 成功

## Phase 6: Execute Logic
- [ ] execute.cpp 添加 DMA 执行逻辑
- [ ] 编译 SimX 成功

## Phase 7: Test Program
- [ ] 创建测试目录结构
- [ ] 创建 Makefile
- [ ] 创建 common.h
- [ ] 创建 kernel.cpp
- [ ] 创建 main.cpp
- [ ] 编译测试程序成功

## Phase 8: Testing
- [ ] 运行 SimX 测试
- [ ] 验证功能正确性
- [ ] 收集性能数据

---

# 🎯 成功标准

1. ✅ 所有代码编译无错误
2. ✅ DMA 测试通过 (PASSED)
3. ✅ 数据传输正确 (无验证错误)
4. ✅ 性能统计正确收集
5. ✅ 无内存泄漏或崩溃

---

# 📝 注意事项

1. **每个阶段完成后都要编译验证**
2. **添加充足的 debug 输出 (DT 宏)**
3. **保持代码风格与现有代码一致**
4. **所有修改都要添加注释说明**
5. **遇到问题及时记录和反馈**

---

# 🔄 回退计划

如果需要回退，按相反顺序撤销修改：
1. 删除测试程序
2. 移除 execute.cpp 中的 DMA 逻辑
3. 移除 socket 中的 DMA 集成
4. 删除 dma_engine.h/cpp
5. 移除 decode.cpp 中的 DMA 解码
6. 移除 types.h 中的 DMA 类型
7. 移除 vx_intrinsics.h 中的 DMA 指令

每个文件的修改都有明确的位置标记，便于回退。

