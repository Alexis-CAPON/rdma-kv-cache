#!/bin/bash

# ============================================
# Benchmark Suite - Run Multiple Tests
# ============================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

log_info "=========================================="
log_info "Benchmark Suite for Disaggregated LLM"
log_info "=========================================="
echo ""

# Create results directory
RESULTS_DIR="${LOCAL_DIR}/benchmark_results/$(date +%Y%m%d_%H%M%S)"
mkdir -p "${RESULTS_DIR}"

log_info "Results will be saved to: ${RESULTS_DIR}"
echo ""

# ============================================
# Test 1: Single Request Latency
# ============================================

log_step "Test 1: Single Request Latency (Cold Start)"
echo ""

./scripts/benchmark.sh \
    --mode single \
    --prompt "Once upon a time, in a distant galaxy" \
    --tokens 50 \
    2>&1 | tee "${RESULTS_DIR}/01_single_request_cold.log"

echo ""
sleep 5

# ============================================
# Test 2: Single Request Latency (Warm)
# ============================================

log_step "Test 2: Single Request Latency (Warm)"
echo ""

./scripts/benchmark.sh \
    --mode single \
    --prompt "The quick brown fox jumps over the lazy dog" \
    --tokens 50 \
    2>&1 | tee "${RESULTS_DIR}/02_single_request_warm.log"

echo ""
sleep 5

# ============================================
# Test 3: Throughput - Small Load (10 requests)
# ============================================

log_step "Test 3: Throughput - Small Load (10 requests)"
echo ""

./scripts/benchmark.sh \
    --mode throughput \
    --num 10 \
    --prompt "Hello world" \
    --tokens 50 \
    2>&1 | tee "${RESULTS_DIR}/03_throughput_10req.log"

echo ""
sleep 5

# ============================================
# Test 4: Throughput - Medium Load (50 requests)
# ============================================

log_step "Test 4: Throughput - Medium Load (50 requests)"
echo ""

./scripts/benchmark.sh \
    --mode throughput \
    --num 50 \
    --prompt "Tell me a story about" \
    --tokens 50 \
    2>&1 | tee "${RESULTS_DIR}/04_throughput_50req.log"

echo ""
sleep 5

# ============================================
# Test 5: Throughput - Large Load (100 requests)
# ============================================

log_step "Test 5: Throughput - Large Load (100 requests)"
echo ""

./scripts/benchmark.sh \
    --mode throughput \
    --num 100 \
    --prompt "Once upon a time" \
    --tokens 50 \
    2>&1 | tee "${RESULTS_DIR}/05_throughput_100req.log"

echo ""
sleep 5

# ============================================
# Test 6: Variable Token Generation
# ============================================

log_step "Test 6: Variable Token Generation"
echo ""

for TOKENS in 10 50 100 200; do
    log_info "Testing with ${TOKENS} tokens..."

    ./scripts/benchmark.sh \
        --mode throughput \
        --num 20 \
        --prompt "Explain quantum computing in simple terms" \
        --tokens ${TOKENS} \
        2>&1 | tee "${RESULTS_DIR}/06_tokens_${TOKENS}.log"

    sleep 3
done

echo ""

# ============================================
# Test 7: Different Prompt Lengths
# ============================================

log_step "Test 7: Different Prompt Lengths"
echo ""

# Short prompt
log_info "Testing short prompt..."
./scripts/benchmark.sh \
    --mode throughput \
    --num 20 \
    --prompt "Hi" \
    --tokens 50 \
    2>&1 | tee "${RESULTS_DIR}/07_prompt_short.log"

sleep 3

# Medium prompt
log_info "Testing medium prompt..."
./scripts/benchmark.sh \
    --mode throughput \
    --num 20 \
    --prompt "Once upon a time, in a land far away, there lived a young princess who dreamed of adventure" \
    --tokens 50 \
    2>&1 | tee "${RESULTS_DIR}/07_prompt_medium.log"

sleep 3

# Long prompt
log_info "Testing long prompt..."
LONG_PROMPT="In the vast expanse of the cosmos, where stars twinkle like diamonds scattered across an infinite velvet canvas, there exists a tale of extraordinary courage and determination. This is a story about a young explorer who dared to venture beyond the known boundaries of their world, seeking answers to questions that had puzzled generations before them."

./scripts/benchmark.sh \
    --mode throughput \
    --num 20 \
    --prompt "${LONG_PROMPT}" \
    --tokens 50 \
    2>&1 | tee "${RESULTS_DIR}/07_prompt_long.log"

echo ""

# ============================================
# Generate Summary Report
# ============================================

log_step "Generating summary report..."

SUMMARY_FILE="${RESULTS_DIR}/SUMMARY.txt"

cat > "${SUMMARY_FILE}" <<EOF
========================================
Benchmark Suite Summary
========================================
Date: $(date)
Orchestrator: ${CLOUDLAB_NODES[0]}
Results Directory: ${RESULTS_DIR}

========================================
Tests Executed:
========================================

1. Single Request Latency (Cold Start)
2. Single Request Latency (Warm)
3. Throughput - Small Load (10 requests)
4. Throughput - Medium Load (50 requests)
5. Throughput - Large Load (100 requests)
6. Variable Token Generation (10, 50, 100, 200 tokens)
7. Different Prompt Lengths (short, medium, long)

========================================
Key Metrics Extracted:
========================================

EOF

# Extract key metrics from each log file
for LOG in "${RESULTS_DIR}"/*.log; do
    echo "" >> "${SUMMARY_FILE}"
    echo "--- $(basename ${LOG}) ---" >> "${SUMMARY_FILE}"

    # Extract latency if present
    if grep -q "Average latency:" "${LOG}"; then
        grep "Average latency:" "${LOG}" >> "${SUMMARY_FILE}"
        grep "Min latency:" "${LOG}" >> "${SUMMARY_FILE}"
        grep "Max latency:" "${LOG}" >> "${SUMMARY_FILE}"
    fi

    # Extract throughput if present
    if grep -q "Throughput:" "${LOG}"; then
        grep "Throughput:" "${LOG}" >> "${SUMMARY_FILE}"
    fi

    # Extract success rate
    if grep -q "Success rate:" "${LOG}"; then
        grep "Success rate:" "${LOG}" >> "${SUMMARY_FILE}"
    fi
done

echo "" >> "${SUMMARY_FILE}"
echo "========================================" >> "${SUMMARY_FILE}"
echo "End of Summary" >> "${SUMMARY_FILE}"
echo "========================================" >> "${SUMMARY_FILE}"

cat "${SUMMARY_FILE}"

echo ""
log_success "=========================================="
log_success "Benchmark Suite Complete!"
log_success "=========================================="
echo ""
echo "Results saved to: ${RESULTS_DIR}"
echo "Summary: ${SUMMARY_FILE}"
echo ""
echo "View detailed logs:"
echo "  ls ${RESULTS_DIR}/"
echo ""
