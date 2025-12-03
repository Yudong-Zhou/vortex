#include <vx_spawn.h>
#include <vx_intrinsics.h>
#include "common.h"

#define LOCAL_BUF_SIZE 256  // 256 elements

void kernel_body(kernel_arg_t* __UNIFORM__ arg) {
	// Use block-level thread index for cooperation
	int tid = threadIdx.x;  // Block-level thread ID
	int num_threads = blockDim.x;  // All threads in the block
	
	// Allocate shared local memory buffer
	TYPE* local_buf = (TYPE*)__local_mem(LOCAL_BUF_SIZE * sizeof(TYPE));
	
	// Calculate how many elements this thread block will process
	uint32_t elements_per_block = LOCAL_BUF_SIZE;
	uint32_t num_blocks = (arg->num_points + elements_per_block - 1) / elements_per_block;
	
	for (uint32_t block = blockIdx.x; block < num_blocks; block += gridDim.x) {
		uint32_t offset = block * elements_per_block;
		uint32_t count = (offset + elements_per_block > arg->num_points) 
		                 ? (arg->num_points - offset) 
		                 : elements_per_block;
		
		// Only the first thread in the block executes DMA
		bool is_first_thread = (threadIdx.x == 0);
		
		// DMA: Global to Local
		dma_id_t dma_in = 0;
		if (is_first_thread) {
			dma_in = vx_dma_g2l(
				local_buf,
				(void*)(arg->src_addr + offset * sizeof(TYPE)),
				count * sizeof(TYPE)
			);
			vx_dma_wait(dma_in);
		}
		
		// Barrier to ensure all threads see the data
		__syncthreads();
		
		// Process data: all threads participate (multiply by 2)
		for (uint32_t i = tid; i < count; i += num_threads) {
			local_buf[i] = local_buf[i] * 2.0f;
		}
		
		// Barrier before write back
		__syncthreads();
		
		// DMA: Local to Global
		dma_id_t dma_out = 0;
		if (is_first_thread) {
			dma_out = vx_dma_l2g(
				(void*)(arg->dst_addr + offset * sizeof(TYPE)),
				local_buf,
				count * sizeof(TYPE)
			);
			vx_dma_wait(dma_out);
		}
		
		// Final barrier
		__syncthreads();
	}
}

int main() {
	kernel_arg_t* arg = (kernel_arg_t*)csr_read(VX_CSR_MSCRATCH);
	// Use all warps in one block to share local memory
	uint32_t num_warps = vx_num_warps();
	uint32_t threads_per_warp = vx_num_threads();
	uint32_t block_size = num_warps * threads_per_warp;  // All threads in one block
	uint32_t grid_size = 1;  // Single block
	return vx_spawn_threads(1, &grid_size, &block_size, (vx_kernel_func_cb)kernel_body, arg);
}
