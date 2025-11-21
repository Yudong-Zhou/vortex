#include <vx_spawn.h>
#include <vx_print.h>
#include "common.h"

// Shared memory buffer for DMA testing
__local__ TYPE local_buffer[DMA_TEST_SIZE];

// Helper function to write DCR
inline void write_dcr(uint32_t addr, uint32_t value) {
    asm volatile ("csrw %0, %1" : : "i"(addr), "r"(value));
}

// Helper function to read DCR
inline uint32_t read_dcr(uint32_t addr) {
    uint32_t value;
    asm volatile ("csrr %0, %1" : "=r"(value) : "i"(addr));
    return value;
}

// Kernel for testing Global to Local DMA
void kernel_dma_g2l(kernel_arg_t* __UNIFORM__ arg) {
    uint32_t core_id = vx_core_id();
    
    // Only core 0 performs DMA operations
    if (core_id == 0) {
        // Configure DMA source address (global memory)
        write_dcr(VX_DCR_DMA_SRC_ADDR0, (uint32_t)(arg->src_addr & 0xFFFFFFFF));
        write_dcr(VX_DCR_DMA_SRC_ADDR1, (uint32_t)(arg->src_addr >> 32));
        
        // Configure DMA destination address (local memory)
        uint64_t dst_addr = (uint64_t)local_buffer;
        write_dcr(VX_DCR_DMA_DST_ADDR0, (uint32_t)(dst_addr & 0xFFFFFFFF));
        write_dcr(VX_DCR_DMA_DST_ADDR1, (uint32_t)(dst_addr >> 32));
        
        // Configure DMA size (in bytes)
        uint64_t byte_size = arg->size * sizeof(TYPE);
        write_dcr(VX_DCR_DMA_SIZE0, (uint32_t)(byte_size & 0xFFFFFFFF));
        write_dcr(VX_DCR_DMA_SIZE1, (uint32_t)(byte_size >> 32));
        
        // Configure core ID
        write_dcr(VX_DCR_DMA_CORE_ID, core_id);
        
        // Start DMA transfer (Global to Local)
        write_dcr(VX_DCR_DMA_CTRL, DMA_CTRL_START | DMA_CTRL_DIR_G2L);
        
        // Wait for DMA completion
        uint32_t status;
        do {
            status = read_dcr(VX_DCR_DMA_STATUS);
        } while (status & DMA_STATUS_BUSY);
        
        // Check for errors
        if (status & DMA_STATUS_ERROR) {
            vx_printf("DMA Error: status=0x%x\n", status);
        }
    }
    
    // Memory fence to ensure DMA writes are visible
    vx_fence();
    
    // Barrier to ensure DMA completion before verification
    vx_barrier(0x80000000, vx_num_cores());
    
    // Verify data in local memory
    uint32_t num_threads = vx_num_threads();
    uint32_t thread_id = vx_thread_id();
    TYPE* src_ptr = (TYPE*)arg->src_addr;
    
    uint32_t errors = 0;
    for (uint32_t i = thread_id; i < arg->size; i += num_threads) {
        if (local_buffer[i] != src_ptr[i]) {
            if (errors < 10) {
                vx_printf("G2L Mismatch at index %d: expected=%d, got=%d\n", 
                         i, src_ptr[i], local_buffer[i]);
            }
            errors++;
        }
    }
    
    if (errors == 0 && thread_id == 0) {
        vx_printf("G2L test PASSED\n");
    }
}

// Kernel for testing Local to Global DMA
void kernel_dma_l2g(kernel_arg_t* __UNIFORM__ arg) {
    uint32_t core_id = vx_core_id();
    
    // First, load data into local memory using G2L DMA
    if (core_id == 0) {
        // Configure DMA source address (global memory)
        write_dcr(VX_DCR_DMA_SRC_ADDR0, (uint32_t)(arg->src_addr & 0xFFFFFFFF));
        write_dcr(VX_DCR_DMA_SRC_ADDR1, (uint32_t)(arg->src_addr >> 32));
        
        // Configure DMA destination address (local memory)
        uint64_t dst_addr = (uint64_t)local_buffer;
        write_dcr(VX_DCR_DMA_DST_ADDR0, (uint32_t)(dst_addr & 0xFFFFFFFF));
        write_dcr(VX_DCR_DMA_DST_ADDR1, (uint32_t)(dst_addr >> 32));
        
        // Configure DMA size (in bytes)
        uint64_t byte_size = arg->size * sizeof(TYPE);
        write_dcr(VX_DCR_DMA_SIZE0, (uint32_t)(byte_size & 0xFFFFFFFF));
        write_dcr(VX_DCR_DMA_SIZE1, (uint32_t)(byte_size >> 32));
        
        // Configure core ID
        write_dcr(VX_DCR_DMA_CORE_ID, core_id);
        
        // Start DMA transfer (Global to Local)
        write_dcr(VX_DCR_DMA_CTRL, DMA_CTRL_START | DMA_CTRL_DIR_G2L);
        
        // Wait for DMA completion
        uint32_t status;
        do {
            status = read_dcr(VX_DCR_DMA_STATUS);
        } while (status & DMA_STATUS_BUSY);
        
        // Check for errors
        if (status & DMA_STATUS_ERROR) {
            vx_printf("DMA G2L Error: status=0x%x\n", status);
        }
    }
    
    // Memory fence to ensure DMA writes are visible
    vx_fence();
    
    // Barrier to ensure DMA completion
    vx_barrier(0x80000000, vx_num_cores());
    
    // Modify data in local memory (all threads participate)
    uint32_t num_threads = vx_num_threads();
    uint32_t thread_id = vx_thread_id();
    
    for (uint32_t i = thread_id; i < arg->size; i += num_threads) {
        local_buffer[i] = local_buffer[i] * 2 + 1;
    }
    
    // Memory fence to ensure all writes are visible
    vx_fence();
    
    // Barrier to ensure all modifications are complete
    vx_barrier(0x80000000, vx_num_cores());
    
    // Transfer modified data back to global memory using L2G DMA
    if (core_id == 0) {
        // Configure DMA source address (local memory)
        uint64_t src_addr = (uint64_t)local_buffer;
        write_dcr(VX_DCR_DMA_SRC_ADDR0, (uint32_t)(src_addr & 0xFFFFFFFF));
        write_dcr(VX_DCR_DMA_SRC_ADDR1, (uint32_t)(src_addr >> 32));
        
        // Configure DMA destination address (global memory)
        write_dcr(VX_DCR_DMA_DST_ADDR0, (uint32_t)(arg->dst_addr & 0xFFFFFFFF));
        write_dcr(VX_DCR_DMA_DST_ADDR1, (uint32_t)(arg->dst_addr >> 32));
        
        // Configure DMA size (in bytes)
        uint64_t byte_size = arg->size * sizeof(TYPE);
        write_dcr(VX_DCR_DMA_SIZE0, (uint32_t)(byte_size & 0xFFFFFFFF));
        write_dcr(VX_DCR_DMA_SIZE1, (uint32_t)(byte_size >> 32));
        
        // Configure core ID
        write_dcr(VX_DCR_DMA_CORE_ID, core_id);
        
        // Start DMA transfer (Local to Global)
        write_dcr(VX_DCR_DMA_CTRL, DMA_CTRL_START | DMA_CTRL_DIR_L2G);
        
        // Wait for DMA completion
        uint32_t status;
        do {
            status = read_dcr(VX_DCR_DMA_STATUS);
        } while (status & DMA_STATUS_BUSY);
        
        // Check for errors
        if (status & DMA_STATUS_ERROR) {
            vx_printf("DMA L2G Error: status=0x%x\n", status);
        }
    }
    
    // Memory fence to ensure DMA reads are complete
    vx_fence();
    
    // Barrier to ensure DMA completion
    vx_barrier(0x80000000, vx_num_cores());
    
    if (thread_id == 0) {
        vx_printf("L2G test completed\n");
    }
}

// Main kernel entry point
int main() {
    kernel_arg_t* arg = (kernel_arg_t*)csr_read(VX_CSR_MSCRATCH);
    
    if (arg->task_id == 0) {
        // Test Global to Local DMA
        kernel_dma_g2l(arg);
    } else if (arg->task_id == 1) {
        // Test Local to Global DMA
        kernel_dma_l2g(arg);
    }
    
    return 0;
}
