#include <stdint.h>
#include <vx_intrinsics.h>
#include <vx_spawn.h>
#include "common.h"

void kernel_body(kernel_arg_t* __UNIFORM__ arg) {
    uint32_t core_id = vx_core_id();
    
    uint32_t src_addr = arg->src_addr;
    uint32_t dst_addr = arg->dst_addr;
    uint32_t size = arg->size;
    
    // Only thread 0 performs DMA
    if (blockIdx.x == 0) {
        // Allocate local memory buffer
        uint32_t* local_buf = (uint32_t*)__local_mem(size);
        
        // Test 1: DMA from global to local
        vx_dma_g2l(local_buf, (void*)src_addr, size);
        
        // Test 2: DMA from local to global
        vx_dma_l2g((void*)dst_addr, local_buf, size);
    }
}

int main() {
    kernel_arg_t* arg = (kernel_arg_t*)csr_read(VX_CSR_MSCRATCH);
    uint32_t num_tasks = 1;
    return vx_spawn_threads(1, &num_tasks, nullptr, (vx_kernel_func_cb)kernel_body, arg);
}

