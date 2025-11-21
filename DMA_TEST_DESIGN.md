# DMA 测试重新设计方案

## 🔍 现状分析

### 当前 DMA 测试结构
```
tests/regression/dma/
├── dma_test.cpp  ❌ 单文件,不符合标准
├── Makefile      ❌ 简化版,不完整
└── README.md
```

### 标准 Vortex 测试结构
```
tests/regression/<test_name>/
├── main.cpp      ✅ Host 端代码
├── kernel.cpp    ✅ GPU Kernel 代码
├── common.h      ✅ 共享数据结构
├── Makefile      ✅ 标准编译配置
└── (可选) 其他头文件
```

---

## 📋 设计思路

### 核心概念

DMA 测试应该验证 **Host → Global Memory → Shared Memory → Kernel** 的完整数据流:

```
┌─────────┐     ┌──────────────┐     ┌──────────────┐     ┌────────┐
│  Host   │────▶│Global Memory │────▶│Shared Memory │────▶│ Kernel │
│(main.cpp)│     │  (DRAM)      │     │  (LocalMem)  │     │(GPU)   │
└─────────┘     └──────────────┘     └──────────────┘     └────────┘
                      ▲                      ▲
                      │                      │
                      └──────────DMA─────────┘
```

### 测试流程

#### 阶段 1: Host 准备数据
1. 生成测试数据
2. 上传到 Global Memory

#### 阶段 2: DMA 传输 (G→S)
1. 配置 DMA (源地址、目标地址、大小、Core ID)
2. 启动 DMA 传输
3. 等待完成

#### 阶段 3: Kernel 验证
1. Kernel 从 Shared Memory 读取数据
2. 执行简单计算 (如 +1)
3. 写回 Global Memory

#### 阶段 4: Host 验证
1. 从 Global Memory 下载结果
2. 与预期值比较
3. 报告测试结果

### 可选: 双向测试

**测试 1**: Global → Shared (主要测试)
**测试 2**: Shared → Global (反向测试)

---

## 📁 文件设计

### 1. common.h - 共享数据结构

```cpp
#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

// DMA DCR 寄存器地址
#define VX_DCR_DMA_SRC_ADDR0     0x006
#define VX_DCR_DMA_SRC_ADDR1     0x007
#define VX_DCR_DMA_DST_ADDR0     0x008
#define VX_DCR_DMA_DST_ADDR1     0x009
#define VX_DCR_DMA_SIZE0         0x00A
#define VX_DCR_DMA_SIZE1         0x00B
#define VX_DCR_DMA_CORE_ID       0x00C
#define VX_DCR_DMA_CTRL          0x00D
#define VX_DCR_DMA_STATUS        0x00E

// DMA 控制位
#define DMA_CTRL_START           0
#define DMA_CTRL_DIR             1

// DMA 状态位
#define DMA_STATUS_IDLE          0
#define DMA_STATUS_BUSY          1
#define DMA_STATUS_DONE          2
#define DMA_STATUS_ERROR         3

// Shared Memory 基地址 (需要从 VX_config.h 获取)
#ifndef LMEM_BASE_ADDR
#define LMEM_BASE_ADDR           0x80000000  // 32-bit 系统
#endif

// Kernel 参数结构
typedef struct {
  uint32_t num_points;      // 数据点数量
  uint64_t src_addr;        // Global Memory 源地址
  uint64_t shared_addr;     // Shared Memory 地址
  uint64_t dst_addr;        // Global Memory 目标地址
  uint32_t use_dma;         // 是否使用 DMA (1=是, 0=否,用于对比)
} kernel_arg_t;

#endif // _COMMON_H_
```

**设计要点**:
- 包含 DMA DCR 定义
- 定义 kernel 参数结构
- 支持 DMA 和非 DMA 模式对比

---

### 2. main.cpp - Host 端代码

```cpp
#include <iostream>
#include <unistd.h>
#include <string.h>
#include <vector>
#include <vortex.h>
#include "common.h"

#define RT_CHECK(_expr)                                         \
   do {                                                         \
     int _ret = _expr;                                          \
     if (0 == _ret)                                             \
       break;                                                   \
     printf("Error: '%s' returned %d!\n", #_expr, (int)_ret);  \
     cleanup();                                                 \
     exit(-1);                                                  \
   } while (false)

///////////////////////////////////////////////////////////////////////////////

const char* kernel_file = "kernel.vxbin";
uint32_t count = 16;        // 数据点数量
uint32_t core_id = 0;       // 目标 Core ID
bool use_dma = true;        // 是否使用 DMA

vx_device_h device = nullptr;
vx_buffer_h src_buffer = nullptr;
vx_buffer_h dst_buffer = nullptr;
vx_buffer_h krnl_buffer = nullptr;
vx_buffer_h args_buffer = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
   std::cout << "Vortex DMA Test." << std::endl;
   std::cout << "Usage: [-k kernel] [-n count] [-c core_id] [-d use_dma] [-h help]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "n:c:d:k:h")) != -1) {
    switch (c) {
    case 'n':
      count = atoi(optarg);
      break;
    case 'c':
      core_id = atoi(optarg);
      break;
    case 'd':
      use_dma = (atoi(optarg) != 0);
      break;
    case 'k':
      kernel_file = optarg;
      break;
    case 'h':
      show_usage();
      exit(0);
      break;
    default:
      show_usage();
      exit(-1);
    }
  }
}

void cleanup() {
  if (device) {
    vx_mem_free(src_buffer);
    vx_mem_free(dst_buffer);
    vx_mem_free(krnl_buffer);
    vx_mem_free(args_buffer);
    vx_dev_close(device);
  }
}

int configure_dma(uint64_t src_addr, uint64_t dst_addr, uint32_t size, uint32_t core_id) {
  std::cout << "Configuring DMA:" << std::endl;
  std::cout << "  src=0x" << std::hex << src_addr << std::endl;
  std::cout << "  dst=0x" << std::hex << dst_addr << std::endl;
  std::cout << "  size=" << std::dec << size << " bytes" << std::endl;
  std::cout << "  core=" << core_id << std::endl;

  // 配置源地址
  RT_CHECK(vx_dcr_write(device, VX_DCR_DMA_SRC_ADDR0, src_addr & 0xFFFFFFFF));
  RT_CHECK(vx_dcr_write(device, VX_DCR_DMA_SRC_ADDR1, (src_addr >> 32) & 0xFFFFFFFF));

  // 配置目标地址
  RT_CHECK(vx_dcr_write(device, VX_DCR_DMA_DST_ADDR0, dst_addr & 0xFFFFFFFF));
  RT_CHECK(vx_dcr_write(device, VX_DCR_DMA_DST_ADDR1, (dst_addr >> 32) & 0xFFFFFFFF));

  // 配置传输大小
  RT_CHECK(vx_dcr_write(device, VX_DCR_DMA_SIZE0, size));
  RT_CHECK(vx_dcr_write(device, VX_DCR_DMA_SIZE1, 0));

  // 配置目标 Core
  RT_CHECK(vx_dcr_write(device, VX_DCR_DMA_CORE_ID, core_id));

  // 启动传输 (DIR=0: Global→Shared)
  RT_CHECK(vx_dcr_write(device, VX_DCR_DMA_CTRL, (1 << DMA_CTRL_START)));

  // 等待完成
  std::cout << "Waiting for DMA completion..." << std::endl;
  uint32_t status;
  uint32_t timeout = 100000;
  while (timeout--) {
    RT_CHECK(vx_dcr_read(device, VX_DCR_DMA_STATUS, &status));
    
    if (status & (1 << DMA_STATUS_DONE)) {
      std::cout << "DMA completed successfully!" << std::endl;
      return 0;
    }
    
    if (status & (1 << DMA_STATUS_ERROR)) {
      std::cerr << "DMA transfer failed!" << std::endl;
      return -1;
    }
  }

  std::cerr << "DMA timeout!" << std::endl;
  return -1;
}

int main(int argc, char *argv[]) {
  // 解析参数
  parse_args(argc, argv);

  // 对齐到 64 字节
  count = (count + 15) & ~15;

  std::cout << "========================================" << std::endl;
  std::cout << "Vortex DMA Test" << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << "Data points: " << count << std::endl;
  std::cout << "Target core: " << core_id << std::endl;
  std::cout << "Use DMA: " << (use_dma ? "Yes" : "No") << std::endl;
  std::cout << "========================================" << std::endl;

  // 打开设备
  std::cout << "Opening device..." << std::endl;
  RT_CHECK(vx_dev_open(&device));

  uint64_t num_cores, num_warps, num_threads;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_CORES, &num_cores));
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_WARPS, &num_warps));
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_THREADS, &num_threads));

  std::cout << "Device info: " << num_cores << " cores, " 
            << num_warps << " warps, " << num_threads << " threads" << std::endl;

  // 验证 core_id
  if (core_id >= num_cores) {
    std::cerr << "Error: core_id " << core_id << " >= num_cores " << num_cores << std::endl;
    cleanup();
    return -1;
  }

  uint32_t buf_size = count * sizeof(int32_t);

  // 分配设备内存
  std::cout << "Allocating device memory..." << std::endl;
  RT_CHECK(vx_mem_alloc(device, buf_size, VX_MEM_READ, &src_buffer));
  RT_CHECK(vx_mem_address(src_buffer, &kernel_arg.src_addr));
  RT_CHECK(vx_mem_alloc(device, buf_size, VX_MEM_WRITE, &dst_buffer));
  RT_CHECK(vx_mem_address(dst_buffer, &kernel_arg.dst_addr));

  std::cout << "  src_addr=0x" << std::hex << kernel_arg.src_addr << std::endl;
  std::cout << "  dst_addr=0x" << std::hex << kernel_arg.dst_addr << std::endl;

  // 设置 Shared Memory 地址
  kernel_arg.shared_addr = LMEM_BASE_ADDR;
  kernel_arg.num_points = count;
  kernel_arg.use_dma = use_dma ? 1 : 0;

  // 生成测试数据
  std::cout << "Generating test data..." << std::endl;
  std::vector<int32_t> h_src(count);
  for (uint32_t i = 0; i < count; ++i) {
    h_src[i] = i + 1;  // 简单的递增序列
  }

  // 上传源数据
  std::cout << "Uploading source data..." << std::endl;
  RT_CHECK(vx_copy_to_dev(src_buffer, h_src.data(), 0, buf_size));

  // 如果使用 DMA,先执行 DMA 传输
  if (use_dma) {
    std::cout << "========================================" << std::endl;
    std::cout << "Phase 1: DMA Transfer (Global→Shared)" << std::endl;
    std::cout << "========================================" << std::endl;
    
    if (configure_dma(kernel_arg.src_addr, kernel_arg.shared_addr, 
                      buf_size, core_id) != 0) {
      cleanup();
      return -1;
    }
  }

  // 上传 kernel
  std::cout << "========================================" << std::endl;
  std::cout << "Phase 2: Kernel Execution" << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << "Uploading kernel..." << std::endl;
  RT_CHECK(vx_upload_kernel_file(device, kernel_file, &krnl_buffer));

  // 上传 kernel 参数
  std::cout << "Uploading kernel arguments..." << std::endl;
  RT_CHECK(vx_upload_bytes(device, &kernel_arg, sizeof(kernel_arg_t), &args_buffer));

  // 启动 kernel
  std::cout << "Starting kernel..." << std::endl;
  RT_CHECK(vx_start(device, krnl_buffer, args_buffer));

  // 等待完成
  std::cout << "Waiting for kernel completion..." << std::endl;
  RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));

  // 下载结果
  std::cout << "========================================" << std::endl;
  std::cout << "Phase 3: Verification" << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << "Downloading results..." << std::endl;
  std::vector<int32_t> h_dst(count);
  RT_CHECK(vx_copy_from_dev(h_dst.data(), dst_buffer, 0, buf_size));

  // 验证结果
  std::cout << "Verifying results..." << std::endl;
  int errors = 0;
  for (uint32_t i = 0; i < count; ++i) {
    // Kernel 应该对每个元素 +1
    int32_t expected = h_src[i] + 1;
    int32_t actual = h_dst[i];
    
    if (actual != expected) {
      if (errors < 10) {
        std::cout << "Error at [" << i << "]: expected=" << expected 
                  << ", actual=" << actual << std::endl;
      }
      ++errors;
    }
  }

  // 清理
  std::cout << "Cleaning up..." << std::endl;
  cleanup();

  // 报告结果
  std::cout << "========================================" << std::endl;
  if (errors != 0) {
    std::cout << "FAILED! Found " << errors << " errors." << std::endl;
    std::cout << "========================================" << std::endl;
    return -1;
  }

  std::cout << "PASSED! All " << count << " values correct." << std::endl;
  std::cout << "========================================" << std::endl;
  return 0;
}
```

**设计要点**:
- 完整的错误处理 (RT_CHECK)
- 支持 DMA 和非 DMA 模式对比
- 清晰的阶段划分
- 详细的日志输出
- 标准的 cleanup 机制

---

### 3. kernel.cpp - GPU Kernel 代码

```cpp
#include <vx_spawn.h>
#include <vx_print.h>
#include "common.h"

// Kernel 函数: 从 Shared Memory 读取,处理,写回 Global Memory
void kernel_body(kernel_arg_t* __UNIFORM__ arg) {
  uint32_t task_id = blockIdx.x;
  
  if (task_id >= arg->num_points)
    return;

  int32_t* src_ptr;
  int32_t* dst_ptr = (int32_t*)arg->dst_addr;

  if (arg->use_dma) {
    // 使用 DMA: 从 Shared Memory 读取
    src_ptr = (int32_t*)arg->shared_addr;
    
    // 调试输出 (可选)
    if (task_id == 0) {
      vx_printf("Kernel: Reading from Shared Memory at 0x%x\n", arg->shared_addr);
    }
  } else {
    // 不使用 DMA: 从 Global Memory 读取
    src_ptr = (int32_t*)arg->src_addr;
    
    if (task_id == 0) {
      vx_printf("Kernel: Reading from Global Memory at 0x%x\n", arg->src_addr);
    }
  }

  // 简单的处理: 读取值并 +1
  int32_t value = src_ptr[task_id];
  value += 1;

  // 写回 Global Memory
  dst_ptr[task_id] = value;

  // 调试输出前几个值
  if (task_id < 4) {
    vx_printf("Kernel[%d]: input=%d, output=%d\n", task_id, src_ptr[task_id], value);
  }
}

int main() {
  kernel_arg_t* arg = (kernel_arg_t*)csr_read(VX_CSR_MSCRATCH);
  return vx_spawn_threads(1, &arg->num_points, nullptr, (vx_kernel_func_cb)kernel_body, arg);
}
```

**设计要点**:
- 支持从 Shared Memory 或 Global Memory 读取
- 简单的数据处理 (+1)
- 调试输出 (可选)
- 标准的 vx_spawn_threads 调用

---

### 4. Makefile - 编译配置

```makefile
ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := dma

SRC_DIR := $(VORTEX_HOME)/tests/regression/$(PROJECT)

SRCS := $(SRC_DIR)/main.cpp

VX_SRCS := $(SRC_DIR)/kernel.cpp

# 默认参数: 16 个数据点, Core 0, 使用 DMA
OPTS ?= -n16 -c0 -d1

include ../common.mk
```

**设计要点**:
- 遵循标准 Makefile 模板
- 包含 main.cpp 和 kernel.cpp
- 设置默认测试参数

---

## 🔄 测试场景

### 场景 1: 基本功能测试 (使用 DMA)
```bash
make run OPTS="-n16 -c0 -d1"
```

**预期流程**:
1. Host 生成数据 [1, 2, 3, ..., 16]
2. 上传到 Global Memory
3. DMA 传输到 Shared Memory (Core 0)
4. Kernel 从 Shared Memory 读取,+1
5. Kernel 写回 Global Memory
6. Host 验证: [2, 3, 4, ..., 17]

### 场景 2: 对比测试 (不使用 DMA)
```bash
make run OPTS="-n16 -c0 -d0"
```

**预期流程**:
1. Host 生成数据
2. 上传到 Global Memory
3. **跳过 DMA**
4. Kernel 直接从 Global Memory 读取,+1
5. Kernel 写回 Global Memory
6. Host 验证结果

### 场景 3: 大数据测试
```bash
make run OPTS="-n1024 -c0 -d1"
```

### 场景 4: 多 Core 测试
```bash
make run OPTS="-n64 -c1 -d1"  # 使用 Core 1
make run OPTS="-n64 -c2 -d1"  # 使用 Core 2
```

---

## ✅ 优势

### 相比当前实现

| 方面 | 当前实现 | 新设计 |
|------|---------|--------|
| **文件结构** | 单文件 | 标准三文件 |
| **Kernel 验证** | 无 | 有 ✅ |
| **端到端测试** | 无 | 有 ✅ |
| **对比测试** | 无 | 支持 DMA/非DMA ✅ |
| **调试输出** | 有限 | 详细 ✅ |
| **错误处理** | 简单 | 完整 ✅ |
| **符合规范** | 否 | 是 ✅ |

### 测试覆盖

- ✅ DMA 配置正确性
- ✅ DMA 传输完成
- ✅ 数据完整性
- ✅ Shared Memory 访问
- ✅ Kernel 执行
- ✅ 端到端数据流

---

## 📝 实现步骤

### 步骤 1: 创建文件
1. 重命名 `dma_test.cpp` → `main.cpp`
2. 创建 `kernel.cpp`
3. 创建 `common.h`
4. 更新 `Makefile`

### 步骤 2: 实现代码
1. 实现 `main.cpp` (Host 端)
2. 实现 `kernel.cpp` (GPU 端)
3. 定义 `common.h` (共享结构)

### 步骤 3: 测试
1. 编译: `make`
2. 运行: `make run`
3. 验证结果

### 步骤 4: 文档
1. 更新 `README.md`
2. 添加使用示例
3. 说明测试场景

---

## 🎯 预期结果

### 成功输出示例

```
========================================
Vortex DMA Test
========================================
Data points: 16
Target core: 0
Use DMA: Yes
========================================
Opening device...
Device info: 4 cores, 4 warps, 4 threads
Allocating device memory...
  src_addr=0x10000
  dst_addr=0x20000
Generating test data...
Uploading source data...
========================================
Phase 1: DMA Transfer (Global→Shared)
========================================
Configuring DMA:
  src=0x10000
  dst=0x80000000
  size=64 bytes
  core=0
Waiting for DMA completion...
DMA completed successfully!
========================================
Phase 2: Kernel Execution
========================================
Uploading kernel...
Uploading kernel arguments...
Starting kernel...
Waiting for kernel completion...
Kernel: Reading from Shared Memory at 0x80000000
Kernel[0]: input=1, output=2
Kernel[1]: input=2, output=3
Kernel[2]: input=3, output=4
Kernel[3]: input=4, output=5
========================================
Phase 3: Verification
========================================
Downloading results...
Verifying results...
Cleaning up...
========================================
PASSED! All 16 values correct.
========================================
```

---

## 📊 总结

### 改进点

1. **标准化**: 符合 Vortex 测试规范
2. **完整性**: 端到端测试覆盖
3. **可维护性**: 清晰的文件结构
4. **可扩展性**: 易于添加新测试场景
5. **可调试性**: 详细的日志和错误处理

### 下一步

完成基本实现后,可以扩展:
- 双向 DMA 测试 (S→G)
- 性能基准测试
- 多 Core 并发测试
- 错误注入测试
- 压力测试

---

**设计完成**: 准备开始实现! 🚀

