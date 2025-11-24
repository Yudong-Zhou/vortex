# DMA 指令冲突检查报告

## 📊 现有 Custom-0 (0x0B) 指令占用情况

基于对 `vx_intrinsics.h` 和 `decode.cpp` 的分析：

### **已占用的 funct7 和 funct3 组合**

| funct7 | funct3 | 指令名称 | 功能 | 文件位置 |
|--------|--------|---------|------|---------|
| **0** | 0 | **TMC** | Thread Mask Control | decode.cpp:1005 |
| **0** | 1 | **WSPAWN** | Warp Spawn | decode.cpp:1009 |
| **0** | 2 | **SPLIT** | Split on predicate | decode.cpp:1014 |
| **0** | 3 | **JOIN** | Join | decode.cpp:1020 |
| **0** | 4 | **BAR** | Barrier | decode.cpp:1024 |
| **0** | 5 | **PRED** | Predicate | decode.cpp:1029 |
| **1** | 0 | **VOTE.ALL** | Vote all | decode.cpp:1046 |
| **1** | 1 | **VOTE.ANY** | Vote any | decode.cpp:1049 |
| **1** | 2 | **VOTE.UNI** | Vote uniform | decode.cpp:1052 |
| **1** | 3 | **VOTE.BAL** | Vote ballot | decode.cpp:1055 |
| **1** | 4 | **SHFL.UP** | Shuffle up | decode.cpp:1058 |
| **1** | 5 | **SHFL.DOWN** | Shuffle down | decode.cpp:1062 |
| **1** | 6 | **SHFL.BFLY** | Shuffle butterfly | decode.cpp:1066 |
| **1** | 7 | **SHFL.IDX** | Shuffle index | decode.cpp:1070 |
| **2** | 0 | **WMMA** | Tensor Core (TCU) | decode.cpp:1082 |

### **可用的 funct7 值**

| funct7 | 状态 | 备注 |
|--------|------|------|
| 0 | ❌ 已占用 | Warp控制指令 (TMC, WSPAWN, SPLIT, JOIN, BAR, PRED) |
| 1 | ❌ 已占用 | Vote和Shuffle指令 |
| 2 | ❌ 已占用 | Tensor Core指令 (TCU扩展) |
| **3** | ✅ **可用** | **推荐用于DMA** |
| 4 | ✅ 可用 | |
| 5 | ✅ 可用 | |
| 6 | ✅ 可用 | |
| 7 | ✅ 可用 | |
| ... | ✅ 可用 | 8-127 都可用 |

## ✅ **DMA 指令定义（无冲突）**

### **推荐方案**

```
Opcode:  0x0B (RISCV_CUSTOM0)
funct7:  3 (未被占用)
funct3:  0 (DMA transfer)
```

### **完整指令格式**

```
VX_DMA dst, src, size_dir

| funct7 | rs2 (size_dir) | rs1 (src) | funct3 | rd (dst) | opcode |
|   3    |    5 bits      |  5 bits   |   0    |  5 bits  |  0x0B  |
```

### **参数说明**

- `rd` (dst): 目标地址寄存器
- `rs1` (src): 源地址寄存器
- `rs2` (size_dir): 传输大小 + 方向标志
  - bits[30:0]: 传输大小（字节数）
  - bit[31]: 方向标志 (0=G2L, 1=L2G)

### **汇编格式**

```c
// 在 vx_intrinsics.h 中定义
inline void vx_dma_transfer(void* dst, void* src, size_t size, int direction) {
    asm volatile (
        ".insn r 0x0B, 0x0, 0x3, %0, %1, %2"
        : 
        : "r"(dst), "r"(src), "r"(size | ((size_t)direction << 31))
        : "memory"
    );
}

// 便捷函数
inline void vx_dma_g2l(void* local_dst, void* global_src, size_t size) {
    vx_dma_transfer(local_dst, global_src, size, 0);
}

inline void vx_dma_l2g(void* global_dst, void* local_src, size_t size) {
    vx_dma_transfer(global_dst, local_src, size, 1);
}
```

## 🔍 **冲突检查结果**

### ✅ **无冲突确认**

1. ✅ **opcode 0x0B**: 正确使用 RISC-V custom-0 扩展空间
2. ✅ **funct7 = 3**: 未被任何现有指令占用
3. ✅ **funct3 = 0**: 在 funct7=3 的命名空间下可用
4. ✅ **与现有指令完全独立**: 不会与 TMC, WSPAWN, SPLIT, VOTE, SHFL, WMMA 冲突

### 📋 **验证清单**

- [x] 检查 `vx_intrinsics.h` 中所有 `.insn r 0x0B` 使用
- [x] 检查 `decode.cpp` 中 `case Opcode::EXT1` 的所有分支
- [x] 确认 funct7=3 未被使用
- [x] 确认与 Tensor Core (funct7=2) 不冲突
- [x] 确认与 Warp Control (funct7=0) 不冲突
- [x] 确认与 Vote/Shuffle (funct7=1) 不冲突

## 🎯 **推荐的实现步骤**

### Step 1: 添加指令定义

```c
// kernel/include/vx_intrinsics.h (在文件末尾，#endif 之前)

// DMA Transfer
#define DMA_DIR_G2L 0  // Global to Local
#define DMA_DIR_L2G 1  // Local to Global

inline void vx_dma_transfer(void* dst, void* src, size_t size, int direction) {
    asm volatile (
        ".insn r 0x0B, 0x0, 0x3, %0, %1, %2"
        : 
        : "r"(dst), "r"(src), "r"(size | ((size_t)direction << 31))
        : "memory"
    );
}

inline void vx_dma_g2l(void* local_dst, void* global_src, size_t size) {
    vx_dma_transfer(local_dst, global_src, size, DMA_DIR_G2L);
}

inline void vx_dma_l2g(void* global_dst, void* local_src, size_t size) {
    vx_dma_transfer(global_dst, local_src, size, DMA_DIR_L2G);
}
```

### Step 2: 添加 SimX 解码

```cpp
// sim/simx/decode.cpp (在 case Opcode::EXT1 中添加)

case 3: { // DMA
  switch (funct3) {
  case 0: { // DMA Transfer
    auto instr = std::allocate_shared<Instr>(instr_pool_, uuid, FUType::SFU);
    instr->setOpType(DmaType::TRANSFER);
    instr->setArgs(IntrDmaArgs{});
    instr->setSrcReg(0, rs1, RegType::Integer); // src
    instr->setSrcReg(1, rs2, RegType::Integer); // size + direction
    instr->setDestReg(rd, RegType::Integer);    // dst
    ibuffer.push_back(instr);
  } break;
  default:
    std::abort();
  }
} break;
```

## 📝 **总结**

**✅ DMA 指令定义安全，无冲突！**

- **Opcode**: 0x0B (RISCV_CUSTOM0)
- **funct7**: 3 ✅ (未被占用)
- **funct3**: 0 ✅ (DMA transfer)
- **冲突检查**: ✅ 通过

可以安全地使用此指令编码进行 DMA 扩展实现。

