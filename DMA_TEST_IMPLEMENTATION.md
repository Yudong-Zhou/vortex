# DMA 测试实现文档

## 概述

本文档描述了 Vortex GPU DMA 引擎的完整测试实现。测试程序遵循 Vortex 标准测试结构，包含主机端代码、GPU 内核代码和共享配置。

## 文件结构

```
tests/regression/dma/
├── main.cpp        # 主机端测试代码
├── kernel.cpp      # GPU 内核代码
├── common.h        # 共享头文件（DCR 定义、配置参数）
├── Makefile        # 构建脚本
└── README.md       # 测试文档
```

## 设计思路

### 1. 测试架构

测试程序采用标准的 Vortex 测试架构：

- **主机端 (main.cpp)**:
  - 初始化 Vortex 设备
  - 分配全局内存缓冲区（源和目标）
  - 生成测试数据
  - 上传内核和参数
  - 启动内核执行
  - 验证结果

- **内核端 (kernel.cpp)**:
  - 配置 DMA 寄存器
  - 启动 DMA 传输
  - 等待传输完成
  - 验证数据正确性

- **共享定义 (common.h)**:
  - DMA DCR 地址定义
  - DMA 控制/状态位定义
  - 内核参数结构
  - 测试配置参数

### 2. 测试场景

#### 测试 1: Global to Local (G2L) DMA

**目的**: 验证从全局内存到本地内存的 DMA 传输

**流程**:
1. 主机端在全局内存中准备测试数据（0, 1, 2, ..., n-1）
2. 内核中 Core 0 配置 DMA:
   - 源地址: 全局内存缓冲区
   - 目标地址: 本地内存 (`local_buffer`)
   - 传输大小: n * sizeof(TYPE) 字节
   - 方向: G2L
3. 启动 DMA 传输并等待完成
4. 所有线程并行验证本地内存中的数据
5. 主机端验证全局内存数据未被修改

**验证点**:
- DMA 传输是否正确完成
- 本地内存数据是否与源数据一致
- 全局内存数据是否保持不变

#### 测试 2: Local to Global (L2G) DMA

**目的**: 验证从本地内存到全局内存的 DMA 传输

**流程**:
1. 主机端在全局内存中准备测试数据（0, 1, 2, ..., n-1）
2. 内核中 Core 0 先使用 G2L DMA 将数据加载到本地内存
3. 所有线程并行修改本地内存数据: `data[i] = data[i] * 2 + 1`
4. Core 0 配置 DMA:
   - 源地址: 本地内存 (`local_buffer`)
   - 目标地址: 全局内存缓冲区
   - 传输大小: n * sizeof(TYPE) 字节
   - 方向: L2G
5. 启动 DMA 传输并等待完成
6. 主机端验证全局内存中的数据是否正确修改

**验证点**:
- G2L DMA 是否正确工作
- 数据修改是否正确
- L2G DMA 是否正确将修改后的数据写回
- 最终结果是否符合预期（`expected[i] = i * 2 + 1`）

### 3. DCR 配置流程

DMA 引擎通过 DCR (Device Control Registers) 进行配置：

```cpp
// 1. 配置源地址（64位，分两次写入）
write_dcr(VX_DCR_DMA_SRC_ADDR0, src_addr_low);
write_dcr(VX_DCR_DMA_SRC_ADDR1, src_addr_high);

// 2. 配置目标地址（64位，分两次写入）
write_dcr(VX_DCR_DMA_DST_ADDR0, dst_addr_low);
write_dcr(VX_DCR_DMA_DST_ADDR1, dst_addr_high);

// 3. 配置传输大小（64位，分两次写入）
write_dcr(VX_DCR_DMA_SIZE0, size_low);
write_dcr(VX_DCR_DMA_SIZE1, size_high);

// 4. 配置核心 ID
write_dcr(VX_DCR_DMA_CORE_ID, core_id);

// 5. 启动传输（设置方向和启动位）
write_dcr(VX_DCR_DMA_CTRL, DMA_CTRL_START | direction);

// 6. 轮询状态寄存器等待完成
do {
    status = read_dcr(VX_DCR_DMA_STATUS);
} while (status & DMA_STATUS_BUSY);

// 7. 检查错误
if (status & DMA_STATUS_ERROR) {
    // 处理错误
}
```

### 4. 同步机制

测试程序使用多层同步确保正确性：

1. **DMA 完成同步**: 通过轮询 `VX_DCR_DMA_STATUS` 寄存器
2. **核心间同步**: 使用 `vx_barrier()` 确保 DMA 完成后再进行验证
3. **线程间同步**: 使用 `vx_barrier()` 确保所有线程完成数据修改后再启动 L2G 传输

### 5. 并行验证

为了提高效率，数据验证采用并行方式：

```cpp
uint32_t num_threads = vx_num_threads();
uint32_t thread_id = vx_thread_id();

for (uint32_t i = thread_id; i < size; i += num_threads) {
    // 每个线程验证一部分数据
    if (local_buffer[i] != expected[i]) {
        errors++;
    }
}
```

## 关键设计决策

### 1. 为什么只有 Core 0 执行 DMA？

- **简化设计**: 避免多核同时访问 DMA 引擎的竞争
- **符合实际场景**: 通常由一个核心负责数据传输，其他核心等待
- **易于调试**: 单一控制点便于追踪问题

### 2. 为什么使用 Barrier？

- **确保数据一致性**: DMA 完成后才能访问数据
- **避免竞争条件**: 确保所有线程完成修改后才启动回写
- **符合 CUDA/OpenCL 模型**: 与主流 GPU 编程模型一致

### 3. 为什么 L2G 测试要先执行 G2L？

- **完整性**: 测试完整的数据流（加载 -> 处理 -> 存储）
- **真实性**: 模拟实际应用场景
- **依赖性**: 验证 G2L 和 L2G 的组合使用

### 4. 为什么使用 64 位地址？

- **兼容性**: 与 SimX 实现保持一致
- **扩展性**: 支持未来可能的大地址空间
- **安全性**: 在 32 位系统中，高 32 位写 0 即可

## 编译和运行

### 编译

```bash
cd tests/regression/dma
make
```

生成文件：
- `dma`: 主机端可执行文件
- `kernel.vxbin`: GPU 内核二进制文件

### 运行

```bash
# SimX 仿真器
make run-simx

# RTL 仿真器
make run-rtlsim

# OPAE (FPGA)
make run-opae

# 自定义参数
./dma -n 128  # 测试 128 个元素
```

## 预期输出

```
=== Vortex DMA Test ===
Test size: 256 elements
Opening device...
Device info:
  Cores: 1
  Warps: 4
  Threads: 4

=== Test 1: Global to Local DMA ===
Allocating device buffers...
  src_buffer=0x...
  dst_buffer=0x...
Uploading source data...
Uploading kernel...
Uploading kernel arguments...
Starting device...
Waiting for completion...
Downloading results...
Verifying results...
PASSED!

=== Test 2: Local to Global DMA ===
Allocating device buffers...
  src_buffer=0x...
  dst_buffer=0x...
Uploading source data...
Uploading kernel...
Uploading kernel arguments...
Starting device...
Waiting for completion...
Downloading results...
Verifying results...
PASSED!

=== ALL TESTS PASSED ===
```

## 错误处理

### DMA 传输错误

如果 DMA 传输失败，内核会打印错误信息：

```
DMA Error: status=0x4
```

状态位含义：
- `0x1`: BUSY - 传输中
- `0x2`: DONE - 传输完成
- `0x4`: ERROR - 传输错误

### 数据验证错误

如果数据验证失败，会打印详细的错误信息：

```
G2L Mismatch at index 10: expected=10, got=0
Error at index 10: expected=21, got=10
```

## 性能考虑

### 数据大小

默认测试大小为 256 个元素（1KB for int32_t）。可以通过 `-n` 参数调整：

- 小数据集（< 64）: 测试基本功能
- 中等数据集（256-1024）: 测试正常场景
- 大数据集（> 1024）: 测试性能和边界条件

### 本地内存限制

本地内存大小有限，测试大小不应超过：
- `DMA_TEST_SIZE` (默认 256)
- 实际本地内存容量

### 并行度

- 线程数: 由 Vortex 配置决定（通常 4-32）
- 核心数: 通常为 1-4
- 数据分配: 每个线程处理 `size / num_threads` 个元素

## 扩展测试

### 1. 边界条件测试

```cpp
// 测试 1: 最小传输（1 字节）
test_size = 1;

// 测试 2: 非对齐地址
src_addr = base_addr + 1;  // 非 4 字节对齐

// 测试 3: 零大小传输
size = 0;
```

### 2. 压力测试

```cpp
// 测试 1: 连续多次传输
for (int i = 0; i < 100; i++) {
    dma_transfer();
}

// 测试 2: 大数据传输
size = LOCAL_MEM_SIZE;  // 填满本地内存
```

### 3. 多核测试

```cpp
// 每个核心独立执行 DMA
uint32_t core_id = vx_core_id();
uint32_t offset = core_id * chunk_size;
dma_transfer(src + offset, dst + offset, chunk_size);
```

## 已知限制

1. **本地内存大小**: 测试大小受本地内存容量限制
2. **单核 DMA**: 当前只支持单核执行 DMA
3. **同步开销**: Barrier 可能引入额外延迟
4. **错误恢复**: 当前没有 DMA 错误恢复机制

## 与其他测试的对比

| 特性 | vecadd | diverge | dma |
|------|--------|---------|-----|
| 主机端 | main.cpp | main.cpp | main.cpp |
| 内核端 | kernel.cpp | kernel.cpp | kernel.cpp |
| 共享头 | common.h | common.h | common.h |
| 本地内存 | ❌ | ❌ | ✅ |
| DCR 访问 | ❌ | ❌ | ✅ |
| Barrier | ❌ | ❌ | ✅ |
| 多测试 | ❌ | ❌ | ✅ (G2L + L2G) |

## 总结

DMA 测试实现遵循 Vortex 标准测试结构，提供了完整的 DMA 功能验证：

✅ **结构标准化**: 符合 Vortex 测试规范
✅ **功能完整**: 覆盖 G2L 和 L2G 两种传输方向
✅ **验证严格**: 多层次数据验证
✅ **文档完善**: 详细的 README 和注释
✅ **易于扩展**: 支持参数化配置和功能扩展

测试程序可以作为 DMA 引擎开发和调试的基础，也可以作为用户使用 DMA 功能的参考示例。

