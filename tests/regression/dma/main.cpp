#include <iostream>
#include <unistd.h>
#include <string.h>
#include <vortex.h>
#include <vector>
#include "common.h"

#define RT_CHECK(_expr)                                         \
   do {                                                         \
     int _ret = _expr;                                          \
     if (0 == _ret)                                             \
       break;                                                   \
     printf("Error: '%s' returned %d!\n", #_expr, (int)_ret);  \
     cleanup();                                                 \
     exit(-1);                                                  \
   } while (false)

///////////////////////////////////////////////////////////////////////////////

const char* kernel_file = "kernel.vxbin";
uint32_t count = 0;

vx_device_h device = nullptr;
vx_buffer_h src_buffer = nullptr;
vx_buffer_h dst_buffer = nullptr;
vx_buffer_h krnl_buffer = nullptr;
vx_buffer_h args_buffer = nullptr;

kernel_arg_t kernel_arg = {};

static void show_usage() {
   std::cout << "Vortex DMA Test." << std::endl;
   std::cout << "Usage: [-k: kernel] [-n words] [-h: help]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "n:k:h?")) != -1) {
    switch (c) {
    case 'n':
      count = atoi(optarg);
      break;
    case 'k':
      kernel_file = optarg;
      break;
    case 'h':
    case '?': {
      show_usage();
      exit(0);
    } break;
    default:
      show_usage();
      exit(-1);
    }
  }
  
  if (count == 0) {
    count = DMA_TEST_SIZE;
  }
}

void cleanup() {
  if (src_buffer) {
    vx_buf_free(src_buffer);
  }
  if (dst_buffer) {
    vx_buf_free(dst_buffer);
  }
  if (krnl_buffer) {
    vx_buf_free(krnl_buffer);
  }
  if (args_buffer) {
    vx_buf_free(args_buffer);
  }
  if (device) {
    vx_dev_close(device);
  }
}

void gen_test_data(std::vector<TYPE>& data, uint32_t size) {
  for (uint32_t i = 0; i < size; ++i) {
    data[i] = static_cast<TYPE>(i);
  }
}

int run_test(const kernel_arg_t& kernel_arg, uint32_t buf_size, uint32_t num_points) {
  // Allocate device buffers
  std::cout << "Allocating device buffers..." << std::endl;
  
  RT_CHECK(vx_buf_alloc(device, buf_size, &src_buffer));
  RT_CHECK(vx_buf_alloc(device, buf_size, &dst_buffer));
  RT_CHECK(vx_buf_alloc(device, sizeof(kernel_arg_t), &args_buffer));
  
  std::cout << "  src_buffer=0x" << std::hex << vx_buf_addr(src_buffer) << std::dec << std::endl;
  std::cout << "  dst_buffer=0x" << std::hex << vx_buf_addr(dst_buffer) << std::dec << std::endl;
  
  // Generate test data
  std::vector<TYPE> h_src(num_points);
  gen_test_data(h_src, num_points);
  
  std::cout << "Uploading source data..." << std::endl;
  RT_CHECK(vx_copy_to_dev(src_buffer, h_src.data(), 0, buf_size));
  
  // Clear destination buffer
  std::vector<TYPE> h_dst(num_points, 0);
  RT_CHECK(vx_copy_to_dev(dst_buffer, h_dst.data(), 0, buf_size));
  
  // Upload kernel file
  std::cout << "Uploading kernel..." << std::endl;
  RT_CHECK(vx_upload_kernel_file(device, kernel_file, &krnl_buffer));
  
  // Update kernel arguments
  kernel_arg_t args = kernel_arg;
  args.src_addr = vx_buf_addr(src_buffer);
  args.dst_addr = vx_buf_addr(dst_buffer);
  args.size = num_points;
  args.ref_addr = (uint64_t)h_src.data(); // For verification in kernel
  
  std::cout << "Uploading kernel arguments..." << std::endl;
  RT_CHECK(vx_copy_to_dev(args_buffer, &args, 0, sizeof(kernel_arg_t)));
  
  // Start device
  std::cout << "Starting device..." << std::endl;
  RT_CHECK(vx_start(device, krnl_buffer, args_buffer));
  
  // Wait for completion
  std::cout << "Waiting for completion..." << std::endl;
  RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));
  
  // Download results
  std::cout << "Downloading results..." << std::endl;
  RT_CHECK(vx_copy_from_dev(h_dst.data(), dst_buffer, 0, buf_size));
  
  // Verify results
  std::cout << "Verifying results..." << std::endl;
  int errors = 0;
  
  if (args.task_id == 0) {
    // For G2L test, data should be unchanged in global memory
    for (uint32_t i = 0; i < num_points; ++i) {
      TYPE expected = h_src[i];
      if (h_dst[i] != expected) {
        if (errors < 10) {
          std::cout << "  Error at index " << i << ": expected=" << expected 
                    << ", got=" << h_dst[i] << std::endl;
        }
        errors++;
      }
    }
  } else if (args.task_id == 1) {
    // For L2G test, data should be modified (val * 2 + 1)
    for (uint32_t i = 0; i < num_points; ++i) {
      TYPE expected = h_src[i] * 2 + 1;
      if (h_dst[i] != expected) {
        if (errors < 10) {
          std::cout << "  Error at index " << i << ": expected=" << expected 
                    << ", got=" << h_dst[i] << std::endl;
        }
        errors++;
      }
    }
  }
  
  if (errors == 0) {
    std::cout << "PASSED!" << std::endl;
  } else {
    std::cout << "FAILED! " << errors << " errors found." << std::endl;
  }
  
  return errors;
}

int main(int argc, char *argv[]) {
  // Parse command line arguments
  parse_args(argc, argv);
  
  std::cout << "=== Vortex DMA Test ===" << std::endl;
  std::cout << "Test size: " << count << " elements" << std::endl;
  
  // Open device connection
  std::cout << "Opening device..." << std::endl;
  RT_CHECK(vx_dev_open(&device));
  
  uint64_t num_cores, num_warps, num_threads;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_CORES, &num_cores));
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_WARPS, &num_warps));
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_THREADS, &num_threads));
  
  std::cout << "Device info:" << std::endl;
  std::cout << "  Cores: " << num_cores << std::endl;
  std::cout << "  Warps: " << num_warps << std::endl;
  std::cout << "  Threads: " << num_threads << std::endl;
  
  uint32_t buf_size = count * sizeof(TYPE);
  int total_errors = 0;
  
  // Test 1: Global to Local DMA
  std::cout << "\n=== Test 1: Global to Local DMA ===" << std::endl;
  kernel_arg.task_id = 0;
  total_errors += run_test(kernel_arg, buf_size, count);
  
  // Clean up buffers for next test
  if (src_buffer) vx_buf_free(src_buffer);
  if (dst_buffer) vx_buf_free(dst_buffer);
  if (krnl_buffer) vx_buf_free(krnl_buffer);
  if (args_buffer) vx_buf_free(args_buffer);
  src_buffer = dst_buffer = krnl_buffer = args_buffer = nullptr;
  
  // Test 2: Local to Global DMA
  std::cout << "\n=== Test 2: Local to Global DMA ===" << std::endl;
  kernel_arg.task_id = 1;
  total_errors += run_test(kernel_arg, buf_size, count);
  
  // Cleanup
  cleanup();
  
  if (total_errors == 0) {
    std::cout << "\n=== ALL TESTS PASSED ===" << std::endl;
    return 0;
  } else {
    std::cout << "\n=== TESTS FAILED ===" << std::endl;
    return -1;
  }
}

