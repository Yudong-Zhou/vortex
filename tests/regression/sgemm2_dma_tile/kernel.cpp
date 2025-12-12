/**
 * SGEMM2 with DMA - Tile Layout Version
 * 
 * This version uses pre-converted tile-layout data for efficient DMA transfers.
 * Instead of transferring tile_size rows separately, we transfer the entire tile
 * in a single DMA operation.
 * 
 * Memory Layout (Tile Layout):
 * - Each tile is stored contiguously in memory
 * - Tile[ti][tj] is at offset: (ti * tiles_per_row + tj) * tile_elements
 * - Within a tile, elements are stored in row-major order
 * 
 * Advantages:
 * - Only 2 DMA transfers per tile iteration (vs. 2 * tile_size)
 * - Larger transfer sizes improve DMA efficiency
 * - Reduced DMA startup overhead
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
    auto tiles_per_row = size / tile_size;
    auto tile_elements = tile_size * tile_size;

    // Determine global row and column indices
    auto g_row = blockIdx.x * blockDim.x + threadIdx.x;
    auto g_col = blockIdx.y * blockDim.y + threadIdx.y;

    // Determine local row and column indices
    auto l_row = threadIdx.x;
    auto l_col = threadIdx.y;

    // Only thread 0 issues DMA requests
    bool is_dma_thread = (threadIdx.x == 0 && threadIdx.y == 0);
    
    // DMA IDs
    dma_id_t dma_id_A = 0, dma_id_B = 0;

    TYPE sum(0);

    // Loop over tiles (k is the tile index along the shared dimension)
    for (uint32_t k = 0; k < tiles_per_row; ++k) {
        
        // === Single DMA transfer for entire tile ===
        // In tile layout, each tile is stored contiguously
        // Only thread 0 issues both DMA requests
        
        if (is_dma_thread) {
            // Tile index for A: (blockIdx.x, k)
            uint32_t tile_idx_A = blockIdx.x * tiles_per_row + k;
            void* src_A = (void*)(A_ptr + tile_idx_A * tile_elements);
            dma_id_A = vx_dma_g2l(local_A, src_A, tile_elements * sizeof(TYPE));
            
            // Tile index for B: (k, blockIdx.y)
            uint32_t tile_idx_B = k * tiles_per_row + blockIdx.y;
            void* src_B = (void*)(B_ptr + tile_idx_B * tile_elements);
            dma_id_B = vx_dma_g2l(local_B, src_B, tile_elements * sizeof(TYPE));
            
            // Wait for both DMAs to complete
            vx_dma_wait(dma_id_A);
            vx_dma_wait(dma_id_B);
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

