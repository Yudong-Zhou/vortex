#ifndef _COMMON_H_
#define _COMMON_H_

#define DMA_DIR_G2L 0  // Global to Local Memory
#define DMA_DIR_L2G 1  // Local to Global Memory

typedef struct {
  uint32_t src_addr;
  uint32_t dst_addr;
  uint32_t size;
} kernel_arg_t;

#endif // _COMMON_H_

