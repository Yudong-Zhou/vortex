#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

// DMA DCR addresses (must match VX_types.vh)
#define VX_DCR_DMA_SRC_ADDR0    0x010
#define VX_DCR_DMA_SRC_ADDR1    0x011
#define VX_DCR_DMA_DST_ADDR0    0x012
#define VX_DCR_DMA_DST_ADDR1    0x013
#define VX_DCR_DMA_SIZE0        0x014
#define VX_DCR_DMA_SIZE1        0x015
#define VX_DCR_DMA_CORE_ID      0x016
#define VX_DCR_DMA_CTRL         0x017
#define VX_DCR_DMA_STATUS       0x018

// DMA control bits
#define DMA_CTRL_START          0x1
#define DMA_CTRL_DIR_G2L        0x0  // Global to Local
#define DMA_CTRL_DIR_L2G        0x2  // Local to Global

// DMA status bits
#define DMA_STATUS_IDLE         0x0
#define DMA_STATUS_BUSY         0x1
#define DMA_STATUS_DONE         0x2
#define DMA_STATUS_ERROR        0x4

// Test configuration
#ifndef TYPE
#define TYPE                    int32_t
#endif
#define DMA_TEST_SIZE           256  // Number of elements

// Kernel argument structure
typedef struct {
  uint32_t task_id;    // 0=G2L test, 1=L2G test
  uint64_t src_addr;   // Source address
  uint64_t dst_addr;   // Destination address
  uint32_t size;       // Number of elements
  uint64_t ref_addr;   // Reference data address (for verification)
} kernel_arg_t;

#endif // _COMMON_H_

