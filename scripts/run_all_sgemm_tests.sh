#!/bin/bash

# SGEMM Benchmark Script
# test sgemm2 and sgemm2_dma_tile under different configurations
# usage: ./scripts/run_all_sgemm_tests.sh <output_folder_name>
# example: ./scripts/run_all_sgemm_tests.sh benchmark_results

set -e

VORTEX_HOME="/mnt/f/UCLA/UCLA-2025-Fall/GPU/vortex_dma"
BUILD_DIR="${VORTEX_HOME}/build"
RESULTS_DIR="${VORTEX_HOME}/test_results"

# get output folder name from parameter
OUTPUT_NAME=${1:-"run"}

# test configurations
# format: "cores,threads,warps"
CONFIGS=(
    "1,1,16"
    "1,2,8"
    "1,4,4"
    "4,1,16"
    "4,2,8"
    "4,4,4"
    "4,4,16"
    "4,8,8"
)

# matrix sizes
MATRIX_SIZES=("16" "64" "256")

# tile sizes
TILE_SIZES=("2" "4" "8")

# create output directory
OUTPUT_DIR="${RESULTS_DIR}/${OUTPUT_NAME}"
mkdir -p "${OUTPUT_DIR}"

echo "=============================================="
echo "SGEMM Benchmark Suite"
echo "=============================================="
echo "Test cases: sgemm2, sgemm2_dma_tile"
echo "Configs: ${CONFIGS[*]}"
echo "Matrix sizes: ${MATRIX_SIZES[*]}"
echo "Tile sizes: ${TILE_SIZES[*]}"
echo "Output directory: ${OUTPUT_DIR}"
echo "=============================================="
echo ""

cd "${BUILD_DIR}"

# iterate over all configuration combinations
for config in "${CONFIGS[@]}"; do
    # parse cores, threads, warps
    IFS=',' read -r cores threads warps <<< "$config"
    
    config_name="c${cores}_t${threads}_w${warps}"
    config_dir="${OUTPUT_DIR}/${config_name}"
    mkdir -p "${config_dir}"/{sgemm2,sgemm2_dma_tile}
    
    # calculate threads_per_core
    threads_per_core=$((threads * warps))
    
    echo "=============================================="
    echo "Testing config: cores=${cores}, threads=${threads}, warps=${warps}"
    echo "  threads_per_core=${threads_per_core}"
    echo "=============================================="
    
    for n in "${MATRIX_SIZES[@]}"; do
        # skip n=256 for core=1 or threads=1 (too slow)
        if [ "$n" -eq 256 ] && { [ "$cores" -eq 1 ] || [ "$threads" -eq 1 ]; }; then
            echo "  Skipping n=${n} for cores=1 or threads=1 (too slow)"
            continue
        fi
        
        # only test n=256 for high-parallelism configurations (threads_per_core >= 64)
        if [ "$threads_per_core" -ge 64 ] && [ "$n" -lt 256 ]; then
            echo "  Skipping n=${n} for high-parallelism config (only testing n=256)"
            continue
        fi
        
        for t in "${TILE_SIZES[@]}"; do
            # skip n < t or n is not a multiple of t
            if [ "$n" -lt "$t" ] || [ $((n % t)) -ne 0 ]; then
                continue
            fi
            
            # skip t*t > threads_per_core (group_size > threads_per_core)
            group_size=$((t * t))
            if [ "$group_size" -gt "$threads_per_core" ]; then
                echo "  n=${n} t=${t}... SKIP (group_size ${group_size} > threads_per_core ${threads_per_core})"
                continue
            fi
            
            # === test sgemm2 ===
            echo "  sgemm2 n=${n} t=${t}..."
            OUTPUT_FILE="${config_dir}/sgemm2/n${n}_t${t}.txt"
            ./ci/blackbox.sh --cores=${cores} --threads=${threads} --warps=${warps} \
                --app=sgemm2 --args="-n${n} -t${t}" > "${OUTPUT_FILE}" 2>&1 || true
            
            # show key results
            if grep -q "PASSED" "${OUTPUT_FILE}"; then
                cycles=$(grep "^PERF:.*cycles=" "${OUTPUT_FILE}" | tail -1 | sed 's/.*cycles=\([0-9]*\).*/\1/')
                echo "    PASSED - cycles: ${cycles}"
            else
                echo "    FAILED or ERROR"
            fi
            
            # === test sgemm2_dma_tile ===
            echo "  sgemm2_dma_tile n=${n} t=${t}..."
            OUTPUT_FILE="${config_dir}/sgemm2_dma_tile/n${n}_t${t}.txt"
            ./ci/blackbox.sh --cores=${cores} --threads=${threads} --warps=${warps} \
                --app=sgemm2_dma_tile --args="-n${n} -t${t}" > "${OUTPUT_FILE}" 2>&1 || true
            
            # show key results
            if grep -q "PASSED" "${OUTPUT_FILE}"; then
                cycles=$(grep "^PERF:.*cycles=" "${OUTPUT_FILE}" | tail -1 | sed 's/.*cycles=\([0-9]*\).*/\1/')
                echo "    PASSED - cycles: ${cycles}"
            else
                echo "    FAILED or ERROR"
            fi
            
            echo ""
        done
    done
done

echo "=============================================="
echo "All tests completed!"
echo "Results saved to: ${OUTPUT_DIR}"
echo "=============================================="

# generate simple result summary
echo ""
echo "Generating summary..."
SUMMARY_FILE="${OUTPUT_DIR}/summary.txt"
echo "SGEMM Benchmark Summary" > "${SUMMARY_FILE}"
echo "========================" >> "${SUMMARY_FILE}"
echo "" >> "${SUMMARY_FILE}"

for config in "${CONFIGS[@]}"; do
    IFS=',' read -r cores threads warps <<< "$config"
    config_name="c${cores}_t${threads}_w${warps}"
    config_dir="${OUTPUT_DIR}/${config_name}"
    threads_per_core=$((threads * warps))
    
    echo "Config: cores=${cores}, threads=${threads}, warps=${warps} (threads_per_core=${threads_per_core})" >> "${SUMMARY_FILE}"
    echo "-------------------------------------------" >> "${SUMMARY_FILE}"
    
    for n in "${MATRIX_SIZES[@]}"; do
        # skip unsuitable configurations
        if [ "$n" -eq 256 ] && { [ "$cores" -eq 1 ] || [ "$threads" -eq 1 ]; }; then
            continue
        fi
        if [ "$threads_per_core" -ge 64 ] && [ "$n" -lt 256 ]; then
            continue
        fi
        
        for t in "${TILE_SIZES[@]}"; do
            group_size=$((t * t))
            if [ "$group_size" -gt "$threads_per_core" ]; then
                continue
            fi
            if [ "$n" -lt "$t" ] || [ $((n % t)) -ne 0 ]; then
                continue
            fi
            
            # sgemm2 results
            sgemm2_file="${config_dir}/sgemm2/n${n}_t${t}.txt"
            if [ -f "${sgemm2_file}" ] && grep -q "PASSED" "${sgemm2_file}"; then
                cycles=$(grep "^PERF:.*cycles=" "${sgemm2_file}" | tail -1 | sed 's/.*cycles=\([0-9]*\).*/\1/')
                echo "  sgemm2          n=${n} t=${t}: ${cycles} cycles" >> "${SUMMARY_FILE}"
            fi
            
            # sgemm2_dma_tile results
            tile_file="${config_dir}/sgemm2_dma_tile/n${n}_t${t}.txt"
            if [ -f "${tile_file}" ] && grep -q "PASSED" "${tile_file}"; then
                cycles=$(grep "^PERF:.*cycles=" "${tile_file}" | tail -1 | sed 's/.*cycles=\([0-9]*\).*/\1/')
                echo "  sgemm2_dma_tile n=${n} t=${t}: ${cycles} cycles" >> "${SUMMARY_FILE}"
            fi
        done
    done
    echo "" >> "${SUMMARY_FILE}"
done

echo "Summary saved to: ${SUMMARY_FILE}"
cat "${SUMMARY_FILE}"
