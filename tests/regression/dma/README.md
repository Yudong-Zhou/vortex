# DMA 测试

这是 Vortex GPU DMA 引擎的综合测试程序。

## 测试结构

测试程序遵循标准的 Vortex 测试结构：

- **main.cpp**: 主机端代码，负责：
  - 初始化设备
  - 分配和管理缓冲区
  - 上传内核和数据
  - 启动内核执行
  - 验证结果

- **kernel.cpp**: GPU 内核代码，包含：
  - DMA 配置和控制函数
  - Global-to-Local (G2L) DMA 测试内核
  - Local-to-Global (L2G) DMA 测试内核
  - 数据验证和修改内核

- **common.h**: 共享头文件，定义：
  - DMA DCR 地址
  - DMA 控制和状态位
  - 测试配置参数
  - 内核参数结构

- **Makefile**: 构建脚本，支持：
  - 主机程序编译
  - 内核程序编译
  - 多种运行目标 (simx, rtlsim, opae)

## 测试场景

### 测试 1: Global to Local DMA (G2L)
1. 在全局内存中准备测试数据
2. 配置 DMA 将数据从全局内存传输到本地内存
3. 启动 DMA 传输
4. 等待传输完成
5. 在内核中验证本地内存中的数据

### 测试 2: Local to Global DMA (L2G)
1. 在全局内存中准备测试数据
2. 使用 DMA 将数据传输到本地内存
3. 在本地内存中修改数据 (val * 2 + 1)
4. 配置 DMA 将修改后的数据从本地内存传输回全局内存
5. 启动 DMA 传输
6. 等待传输完成
7. 在主机端验证全局内存中的数据

## 编译

```bash
make
```

这将生成：
- `dma`: 主机端可执行文件
- `kernel.vxbin`: GPU 内核二进制文件

## 运行

### SimX 仿真器
```bash
make run-simx
```

### RTL 仿真器
```bash
make run-rtlsim
```

### OPAE (FPGA)
```bash
make run-opae
```

### 自定义参数
```bash
./dma -n <num_elements> -k <kernel_file>
```

参数说明：
- `-n`: 测试元素数量 (默认: 256)
- `-k`: 内核文件路径 (默认: kernel.vxbin)
- `-h`: 显示帮助信息

## DMA 配置

DMA 引擎通过 DCR (Device Control Registers) 进行配置：

1. **源地址** (`VX_DCR_DMA_SRC_ADDR0/1`): 64位源地址
2. **目标地址** (`VX_DCR_DMA_DST_ADDR0/1`): 64位目标地址
3. **传输大小** (`VX_DCR_DMA_SIZE0/1`): 64位字节数
4. **核心ID** (`VX_DCR_DMA_CORE_ID`): 目标核心ID
5. **控制寄存器** (`VX_DCR_DMA_CTRL`): 启动传输和设置方向
   - Bit 0: START - 启动传输
   - Bit 1: DIR - 传输方向 (0=G2L, 1=L2G)
6. **状态寄存器** (`VX_DCR_DMA_STATUS`): 查询传输状态
   - Bit 0: IDLE - 空闲状态
   - Bit 1: BUSY - 传输中
   - Bit 2: DONE - 传输完成
   - Bit 3: ERROR - 传输错误

## 预期输出

成功运行时，应该看到类似以下的输出：

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

## 故障排除

### 编译错误
- 确保 `VORTEX_RT_PATH` 和 `VORTEX_DRV_PATH` 正确设置
- 检查 RISC-V 工具链是否正确安装

### 运行时错误
- 检查 DMA 引擎是否正确实现
- 验证 DCR 地址定义是否与 `VX_types.vh` 一致
- 使用调试模式编译: `make DEBUG=1`

### DMA 传输失败
- 检查地址对齐 (应该是 4 字节对齐)
- 验证传输大小是否合理
- 检查本地内存容量是否足够

## 性能统计

DMA 引擎会收集以下性能统计信息：
- 总传输次数
- 总传输字节数
- 读请求数
- 写请求数
- 读延迟
- 写延迟
- 错误计数

这些统计信息可以通过 SimX 的性能分析工具查看。

## 扩展测试

可以通过以下方式扩展测试：

1. **不同数据大小**: 修改 `DMA_TEST_SIZE` 或使用 `-n` 参数
2. **不同数据类型**: 修改 `common.h` 中的 `TYPE` 定义
3. **多核测试**: 修改内核以支持多核并发 DMA 传输
4. **性能测试**: 添加计时代码以测量 DMA 吞吐量
5. **边界条件**: 测试非对齐地址、零大小传输等

## 相关文件

- `sim/simx/dma_engine.h/cpp`: DMA 引擎实现
- `hw/rtl/VX_types.vh`: DCR 地址定义
- `sim/simx/socket.cpp`: DMA 引擎集成
- `DMA_REVISED_DESIGN.md`: DMA 设计文档
- `DMA_IMPLEMENTATION_COMPLETE.md`: 实现总结
