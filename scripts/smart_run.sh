#!/bin/bash

# ============================================
# Smart Run - Start Disaggregated LLM System
# ============================================
# Starts orchestrator, prefill nodes, decode nodes, and vLLM instances

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

# ============================================
# Configuration
# ============================================

# Calculate available nodes (excluding orchestrator)
AVAILABLE_NODES=$((${#CLOUDLAB_NODES[@]} - 1))

# If no arguments provided, auto-distribute available nodes
if [ -z "$1" ] && [ -z "$2" ]; then
    # Auto mode: split available nodes evenly between prefill and decode
    NUM_PREFILL_NODES=$((AVAILABLE_NODES / 2))
    NUM_DECODE_NODES=$((AVAILABLE_NODES - NUM_PREFILL_NODES))

    log_info "Auto-mode: Using all ${#CLOUDLAB_NODES[@]} nodes (1 orchestrator, ${NUM_PREFILL_NODES} prefill, ${NUM_DECODE_NODES} decode)"
else
    # Manual mode: use provided arguments (default to 1 each if not specified)
    NUM_PREFILL_NODES=${1:-1}
    NUM_DECODE_NODES=${2:-1}
fi

# Validate we have enough nodes
TOTAL_NODES=$((1 + NUM_PREFILL_NODES + NUM_DECODE_NODES))
if [ $TOTAL_NODES -gt ${#CLOUDLAB_NODES[@]} ]; then
    log_error "Not enough CloudLab nodes. Need $TOTAL_NODES (1 orchestrator + $NUM_PREFILL_NODES prefill + $NUM_DECODE_NODES decode), have ${#CLOUDLAB_NODES[@]}"
    log_error "Available nodes after orchestrator: $AVAILABLE_NODES"
    log_error "Usage: $0 [num_prefill] [num_decode]"
    log_error "Example: $0 1 1  # Use 1 prefill and 1 decode node"
    exit 1
fi

# Assign nodes
ORCHESTRATOR_NODE="${CLOUDLAB_NODES[0]}"
PREFILL_NODES=("${CLOUDLAB_NODES[@]:1:$NUM_PREFILL_NODES}")
DECODE_NODES=("${CLOUDLAB_NODES[@]:$((1+NUM_PREFILL_NODES)):$NUM_DECODE_NODES}")

log_info "=========================================="
log_info "Starting Disaggregated LLM Inference System"
log_info "=========================================="
echo ""
log_info "Orchestrator: ${ORCHESTRATOR_NODE}"
log_info "Prefill nodes: ${PREFILL_NODES[@]}"
log_info "Decode nodes: ${DECODE_NODES[@]}"
echo ""

# ============================================
# Step 1: Start Orchestrator
# ============================================

log_step "Starting orchestrator..."

ORCHESTRATOR_HOST=$(get_full_hostname "$ORCHESTRATOR_NODE")

ssh "${SSH_OPTS[@]}" "${USERNAME}@${ORCHESTRATOR_HOST}" \
    "cd ${REMOTE_DIR} && mkdir -p logs && \
    bash -c 'nohup ./build/cpp/orchestrator --config configs/orchestrator.yaml \
    > logs/orchestrator.log 2>&1 < /dev/null & disown'"

sleep 2

# Verify orchestrator started
ORCH_RUNNING=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${ORCHESTRATOR_HOST}" \
    "pgrep -f 'orchestrator --config' > /dev/null && echo 'yes' || echo 'no'" 2>/dev/null)

if [ "$ORCH_RUNNING" == "yes" ]; then
    log_success "Orchestrator started on ${ORCHESTRATOR_HOST}"
else
    log_error "Failed to start orchestrator"
    exit 1
fi

echo ""

# ============================================
# Step 2: Start Prefill Nodes (vLLM + C++ node)
# ============================================

log_step "Starting prefill nodes..."

for i in "${!PREFILL_NODES[@]}"; do
    NODE="${PREFILL_NODES[$i]}"
    HOST=$(get_full_hostname "$NODE")
    NODE_ID="prefill-$(printf '%02d' $i)"
    VLLM_PORT=$((VLLM_PREFILL_BASE_PORT + i))
    NODE_TCP_PORT=$((NODE_TCP_BASE_PORT + i))

    log_info "Starting ${NODE_ID} on ${HOST} (vLLM will be started by C++ node)..."

    # Start C++ prefill node with node-specific config
    # The C++ node will automatically start vLLM based on config (using venv Python path in node.cpp)
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "cd ${REMOTE_DIR} && mkdir -p logs && \
        bash -c 'nohup ./build/cpp/prefill_node --config configs/${NODE_ID}.yaml \
            > logs/${NODE_ID}-node.log 2>&1 < /dev/null & disown'"

    sleep 3

    # Verify C++ node started
    NODE_RUNNING=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "pgrep -f 'prefill_node --config' > /dev/null && echo 'yes' || echo 'no'" 2>/dev/null)

    if [ "$NODE_RUNNING" == "yes" ]; then
        log_success "${NODE_ID} C++ node started (will spawn vLLM)"
    else
        log_error "${NODE_ID} C++ node failed to start"
    fi
done

echo ""

# ============================================
# Step 3: Start Decode Nodes (vLLM + C++ node)
# ============================================

log_step "Starting decode nodes..."

for i in "${!DECODE_NODES[@]}"; do
    NODE="${DECODE_NODES[$i]}"
    HOST=$(get_full_hostname "$NODE")
    NODE_ID="decode-$(printf '%02d' $i)"
    VLLM_PORT=$((VLLM_DECODE_BASE_PORT + i))
    NODE_TCP_PORT=$((NODE_TCP_BASE_PORT + 100 + i))

    log_info "Starting ${NODE_ID} on ${HOST} (vLLM will be started by C++ node)..."

    # Start C++ decode node with node-specific config
    # The C++ node will automatically start vLLM based on config (using venv Python path in node.cpp)
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "cd ${REMOTE_DIR} && mkdir -p logs && \
        bash -c 'nohup ./build/cpp/decode_node --config configs/${NODE_ID}.yaml \
            > logs/${NODE_ID}-node.log 2>&1 < /dev/null & disown'"

    sleep 3

    # Verify C++ node started
    NODE_RUNNING=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "pgrep -f 'decode_node --config' > /dev/null && echo 'yes' || echo 'no'" 2>/dev/null)

    if [ "$NODE_RUNNING" == "yes" ]; then
        log_success "${NODE_ID} C++ node started (will spawn vLLM)"
    else
        log_error "${NODE_ID} C++ node failed to start"
    fi
done

echo ""

# ============================================
# Summary
# ============================================

log_success "=========================================="
log_success "All components started successfully!"
log_success "=========================================="
echo ""
echo "Orchestrator:"
echo "  Node: ${ORCHESTRATOR_HOST}"
echo "  Port: ${ORCHESTRATOR_PORT}"
echo "  Log:  ssh ${USERNAME}@${ORCHESTRATOR_HOST} 'tail -f ${REMOTE_DIR}/logs/orchestrator.log'"
echo ""
echo "Prefill Nodes:"
for i in "${!PREFILL_NODES[@]}"; do
    NODE="${PREFILL_NODES[$i]}"
    HOST=$(get_full_hostname "$NODE")
    NODE_ID="prefill-$(printf '%02d' $i)"
    VLLM_PORT=$((VLLM_PREFILL_BASE_PORT + i))
    echo "  ${NODE_ID}: ${HOST}:${VLLM_PORT}"
    echo "    vLLM log: ssh ${USERNAME}@${HOST} 'tail -f ${REMOTE_DIR}/logs/${NODE_ID}-vllm.log'"
    echo "    Node log: ssh ${USERNAME}@${HOST} 'tail -f ${REMOTE_DIR}/logs/${NODE_ID}-node.log'"
done
echo ""
echo "Decode Nodes:"
for i in "${!DECODE_NODES[@]}"; do
    NODE="${DECODE_NODES[$i]}"
    HOST=$(get_full_hostname "$NODE")
    NODE_ID="decode-$(printf '%02d' $i)"
    VLLM_PORT=$((VLLM_DECODE_BASE_PORT + i))
    echo "  ${NODE_ID}: ${HOST}:${VLLM_PORT}"
    echo "    vLLM log: ssh ${USERNAME}@${HOST} 'tail -f ${REMOTE_DIR}/logs/${NODE_ID}-vllm.log'"
    echo "    Node log: ssh ${USERNAME}@${HOST} 'tail -f ${REMOTE_DIR}/logs/${NODE_ID}-node.log'"
done
echo ""
echo "Test the system:"
echo "  ./scripts/test_request.sh \"Once upon a time\""
echo ""
echo "Stop the system:"
echo "  ./scripts/smart_stop.sh"
echo ""
