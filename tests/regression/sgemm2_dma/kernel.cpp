/**
 * SGEMM2 with DMA - Matrix multiplication using DMA for tile loading
 * 
 * Parallel DMA version: Multiple threads issue DMA requests simultaneously
 * 
 * Key optimizations:
 * - Different threads issue different DMA requests IN PARALLEL
 * - This fully utilizes the multi-channel DMA engine
 * - Each thread is responsible for one row of tile A or B
 */

#include <vx_spawn.h>
#include <vx_intrinsics.h>
#include "common.h"

void kernel_body(kernel_arg_t *arg) {
    // Setup buffer arguments
    auto A_ptr = reinterpret_cast<TYPE*>(arg->A_addr);
    auto B_ptr = reinterpret_cast<TYPE*>(arg->B_addr);
    auto C_ptr = reinterpret_cast<TYPE*>(arg->C_addr);

    // Allocate local memory for the tile of matrix A & B
    auto local_ptr = __local_mem(2 * blockDim.x * blockDim.y * sizeof(TYPE));
    auto local_A = (TYPE*)local_ptr;
    auto local_B = (TYPE*)local_ptr + blockDim.x * blockDim.y;

    auto size = arg->size;
    auto tile_size = arg->tile_size;

    // Determine global row and column indices
    auto g_row = blockIdx.x * blockDim.x + threadIdx.x;
    auto g_col = blockIdx.y * blockDim.y + threadIdx.y;

    // Determine local row and column indices
    auto l_row = threadIdx.x;
    auto l_col = threadIdx.y;

    // Flatten thread ID within the block
    uint32_t flat_tid = threadIdx.x * blockDim.y + threadIdx.y;
    uint32_t total_dma_requests = 2 * tile_size;  // tile_size for A + tile_size for B
    
    // Each thread stores its own DMA ID (if it issues a DMA)
    dma_id_t my_dma_id = 0;
    bool issued_dma = false;

    TYPE sum(0);

    // Loop over tiles
    for (uint32_t k = 0; k < size; k += tile_size) {
        
        // === Parallel DMA Phase ===
        // Each thread with flat_tid < total_dma_requests issues ONE DMA request
        // This allows multiple DMA requests to be issued SIMULTANEOUSLY!
        
        if (flat_tid < total_dma_requests) {
            issued_dma = true;
            
            if (flat_tid < tile_size) {
                // Threads 0 to tile_size-1: Load tile A row by row
                uint32_t row = flat_tid;
                uint32_t global_row = blockIdx.x * tile_size + row;
                void* src = (void*)(A_ptr + global_row * size + k);
                void* dst = (void*)(local_A + row * tile_size);
                my_dma_id = vx_dma_g2l(dst, src, tile_size * sizeof(TYPE));
            } else {
                // Threads tile_size to 2*tile_size-1: Load tile B row by row
                uint32_t row = flat_tid - tile_size;
                uint32_t global_row = k + row;
                void* src = (void*)(B_ptr + global_row * size + blockIdx.y * tile_size);
                void* dst = (void*)(local_B + row * tile_size);
                my_dma_id = vx_dma_g2l(dst, src, tile_size * sizeof(TYPE));
            }
        }
        
        // Each thread waits for its own DMA to complete
        if (issued_dma) {
            vx_dma_wait(my_dma_id);
        }

        // Synchronize all threads to ensure DMA data is visible
        __syncthreads();

        // === Compute Phase: Partial sum for the local tile ===
        for (uint32_t j = 0; j < tile_size; ++j) {
            sum += local_A[l_row * tile_size + j] * local_B[j * tile_size + l_col];
        }

        // Synchronize before next tile iteration
        __syncthreads();
    }

    // Store the computed sum into the result matrix C
    C_ptr[g_row * size + g_col] = sum;
}

int main() {
    auto arg = (kernel_arg_t*)csr_read(VX_CSR_MSCRATCH);
    return vx_spawn_threads(2, arg->grid_dim, arg->block_dim, (vx_kernel_func_cb)kernel_body, arg);
}
