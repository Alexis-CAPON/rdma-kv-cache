#!/bin/bash

# ============================================
# Benchmark Script for Disaggregated LLM System
# ============================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

# Get orchestrator address
ORCHESTRATOR_NODE="${CLOUDLAB_NODES[0]}"
ORCHESTRATOR_HOST=$(get_full_hostname "$ORCHESTRATOR_NODE")

# Default parameters
MODE="single"
NUM_REQUESTS=100
MAX_TOKENS=50
PROMPT="Once upon a time"
CLIENT_NODE=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --mode)
            MODE="$2"
            shift 2
            ;;
        --num)
            NUM_REQUESTS="$2"
            shift 2
            ;;
        --tokens)
            MAX_TOKENS="$2"
            shift 2
            ;;
        --prompt)
            PROMPT="$2"
            shift 2
            ;;
        --client-node)
            CLIENT_NODE="$2"
            shift 2
            ;;
        --help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --mode <single|throughput>   Benchmark mode (default: single)"
            echo "  --num <num>                  Number of requests for throughput (default: 100)"
            echo "  --tokens <num>               Max tokens to generate (default: 50)"
            echo "  --prompt <text>              Prompt text (default: 'Once upon a time')"
            echo "  --client-node <node>         Run client on specific node (default: local)"
            echo ""
            echo "Examples:"
            echo "  # Single request from local machine"
            echo "  $0 --prompt \"Tell me a story\""
            echo ""
            echo "  # Throughput benchmark with 500 requests"
            echo "  $0 --mode throughput --num 500 --tokens 100"
            echo ""
            echo "  # Run client from a CloudLab node"
            echo "  $0 --client-node clgpu021.clemson.cloudlab.us --mode throughput --num 100"
            echo ""
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            exit 1
            ;;
    esac
done

log_info "=========================================="
log_info "Benchmark Configuration"
log_info "=========================================="
echo "Orchestrator: ${ORCHESTRATOR_HOST}:${ORCHESTRATOR_PORT}"
echo "Mode:         ${MODE}"
echo "Prompt:       ${PROMPT}"
echo "Max tokens:   ${MAX_TOKENS}"
if [ "$MODE" == "throughput" ]; then
    echo "Num requests: ${NUM_REQUESTS}"
fi
if [ -n "$CLIENT_NODE" ]; then
    echo "Client node:  ${CLIENT_NODE}"
fi
echo ""

# Build command
BENCHMARK_CMD="./build/bin/benchmark_client \
    --host ${ORCHESTRATOR_HOST} \
    --port ${ORCHESTRATOR_PORT} \
    --mode ${MODE} \
    --num ${NUM_REQUESTS} \
    --tokens ${MAX_TOKENS} \
    --prompt \"${PROMPT}\""

# Run benchmark
if [ -n "$CLIENT_NODE" ]; then
    # Run on remote CloudLab node
    log_step "Running benchmark client on ${CLIENT_NODE}..."
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${CLIENT_NODE}" \
        "cd ${REMOTE_DIR} && ${BENCHMARK_CMD}"
else
    # Run locally
    log_step "Running benchmark client locally..."
    cd "${LOCAL_DIR}"
    eval "${BENCHMARK_CMD}"
fi

echo ""
log_success "Benchmark complete!"
