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

# Default: 1 orchestrator, 2 prefill, 2 decode
NUM_PREFILL_NODES=${1:-2}
NUM_DECODE_NODES=${2:-2}

# Validate we have enough nodes
TOTAL_NODES=$((1 + NUM_PREFILL_NODES + NUM_DECODE_NODES))
if [ $TOTAL_NODES -gt ${#CLOUDLAB_NODES[@]} ]; then
    log_error "Not enough CloudLab nodes. Need $TOTAL_NODES, have ${#CLOUDLAB_NODES[@]}"
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
    nohup ./build/bin/orchestrator --config configs/orchestrator.yaml \
    > logs/orchestrator.log 2>&1 < /dev/null &"

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

    log_info "Starting ${NODE_ID} on ${HOST} (vLLM:${VLLM_PORT}, TCP:${NODE_TCP_PORT})..."

    # Build vLLM connector arguments based on USE_GPU flag
    if [ "${USE_GPU:-true}" = "true" ]; then
        # GPUDirect RDMA path: use our custom RDMAConnector
        KV_CONNECTOR_ARGS="--kv-connector rdma_connector --kv-role send"
        GPU_ARGS="--gpu-memory-utilization ${GPU_MEMORY_UTILIZATION}"
        MOONCAKE_ENV=""
    else
        # CPU/standard RDMA path: use MooncakeConnector (no GPU required)
        NODE_IP=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" "hostname -I | awk '{print \$1}'" 2>&1) \
            || { log_error "Failed to resolve IP for ${HOST}"; exit 1; }
        if [ -z "$NODE_IP" ]; then
            log_error "Could not determine IP address of ${HOST} — cannot generate Mooncake config"
            exit 1
        fi
        MOONCAKE_CFG="/tmp/mooncake-${NODE_ID}.json"
        ORCH_IP=$(get_full_hostname "$ORCHESTRATOR_NODE")
        ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
            "sed 's/<node_ip>/${NODE_IP}/g; s/<orchestrator_ip>/${ORCH_IP}/g' \
            ${REMOTE_DIR}/configs/mooncake.json > ${MOONCAKE_CFG}"
        KV_CONNECTOR_ARGS="--kv-connector MooncakeConnector --kv-role send --device cpu"
        GPU_ARGS=""
        MOONCAKE_ENV="export MOONCAKE_CONFIG_PATH=${MOONCAKE_CFG} &&"
    fi

    # Start vLLM server (with disaggregated prefill mode)
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "cd ${REMOTE_DIR} && mkdir -p logs && \
        export PYTHONPATH=${VLLM_PATH}:\${PYTHONPATH} && \
        ${MOONCAKE_ENV} \
        nohup python -m vllm.entrypoints.openai.api_server \
            --model ${MODEL_NAME} \
            --host 0.0.0.0 \
            --port ${VLLM_PORT} \
            --max-model-len ${MODEL_MAX_LEN} \
            ${GPU_ARGS} \
            --tensor-parallel-size ${TENSOR_PARALLEL_SIZE} \
            --disable-log-requests \
            ${KV_CONNECTOR_ARGS} \
            > logs/${NODE_ID}-vllm.log 2>&1 < /dev/null &"

    # Give vLLM time to start
    sleep 5

    # Start C++ prefill node with node-specific config
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "cd ${REMOTE_DIR} && \
        nohup ./build/bin/prefill_node --config configs/${NODE_ID}.yaml \
            > logs/${NODE_ID}-node.log 2>&1 < /dev/null &"

    sleep 2

    # Verify both processes started
    VLLM_RUNNING=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "pgrep -f 'vllm.*--port ${VLLM_PORT}' > /dev/null && echo 'yes' || echo 'no'" 2>/dev/null)
    NODE_RUNNING=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "pgrep -f 'prefill_node --config' > /dev/null && echo 'yes' || echo 'no'" 2>/dev/null)

    if [ "$VLLM_RUNNING" == "yes" ] && [ "$NODE_RUNNING" == "yes" ]; then
        log_success "${NODE_ID} started (vLLM + C++ node)"
    else
        log_error "${NODE_ID} failed to start (vLLM:${VLLM_RUNNING}, Node:${NODE_RUNNING})"
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

    log_info "Starting ${NODE_ID} on ${HOST} (vLLM:${VLLM_PORT}, TCP:${NODE_TCP_PORT})..."

    # Build vLLM connector arguments based on USE_GPU flag
    if [ "${USE_GPU:-true}" = "true" ]; then
        # GPUDirect RDMA path: use our custom RDMAConnector
        KV_CONNECTOR_ARGS="--kv-connector rdma_connector --kv-role recv"
        GPU_ARGS="--gpu-memory-utilization ${GPU_MEMORY_UTILIZATION}"
        MOONCAKE_ENV=""
    else
        # CPU/standard RDMA path: use MooncakeConnector (no GPU required)
        NODE_IP=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" "hostname -I | awk '{print \$1}'" 2>&1) \
            || { log_error "Failed to resolve IP for ${HOST}"; exit 1; }
        if [ -z "$NODE_IP" ]; then
            log_error "Could not determine IP address of ${HOST} — cannot generate Mooncake config"
            exit 1
        fi
        MOONCAKE_CFG="/tmp/mooncake-${NODE_ID}.json"
        ORCH_IP=$(get_full_hostname "$ORCHESTRATOR_NODE")
        ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
            "sed 's/<node_ip>/${NODE_IP}/g; s/<orchestrator_ip>/${ORCH_IP}/g' \
            ${REMOTE_DIR}/configs/mooncake.json > ${MOONCAKE_CFG}"
        KV_CONNECTOR_ARGS="--kv-connector MooncakeConnector --kv-role recv --device cpu"
        GPU_ARGS=""
        MOONCAKE_ENV="export MOONCAKE_CONFIG_PATH=${MOONCAKE_CFG} &&"
    fi

    # Start vLLM server (with disaggregated decode mode)
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "cd ${REMOTE_DIR} && mkdir -p logs && \
        export PYTHONPATH=${VLLM_PATH}:\${PYTHONPATH} && \
        ${MOONCAKE_ENV} \
        nohup python -m vllm.entrypoints.openai.api_server \
            --model ${MODEL_NAME} \
            --host 0.0.0.0 \
            --port ${VLLM_PORT} \
            --max-model-len ${MODEL_MAX_LEN} \
            ${GPU_ARGS} \
            --tensor-parallel-size ${TENSOR_PARALLEL_SIZE} \
            --disable-log-requests \
            ${KV_CONNECTOR_ARGS} \
            > logs/${NODE_ID}-vllm.log 2>&1 < /dev/null &"

    # Give vLLM time to start
    sleep 5

    # Start C++ decode node with node-specific config
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "cd ${REMOTE_DIR} && \
        nohup ./build/bin/decode_node --config configs/${NODE_ID}.yaml \
            > logs/${NODE_ID}-node.log 2>&1 < /dev/null &"

    sleep 2

    # Verify both processes started
    VLLM_RUNNING=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "pgrep -f 'vllm.*--port ${VLLM_PORT}' > /dev/null && echo 'yes' || echo 'no'" 2>/dev/null)
    NODE_RUNNING=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "pgrep -f 'decode_node --config' > /dev/null && echo 'yes' || echo 'no'" 2>/dev/null)

    if [ "$VLLM_RUNNING" == "yes" ] && [ "$NODE_RUNNING" == "yes" ]; then
        log_success "${NODE_ID} started (vLLM + C++ node)"
    else
        log_error "${NODE_ID} failed to start (vLLM:${VLLM_RUNNING}, Node:${NODE_RUNNING})"
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
