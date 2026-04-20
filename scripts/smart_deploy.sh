#!/bin/bash

# ============================================
# Smart Deploy - Deploy Disaggregated LLM System
# ============================================
# Builds and deploys the system to CloudLab nodes

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

# ============================================
# Configuration
# ============================================

NUM_NODES="${1:-5}"  # Default: 1 orchestrator + 2 prefill + 2 decode

if [ $NUM_NODES -gt ${#CLOUDLAB_NODES[@]} ]; then
    log_error "Requested $NUM_NODES nodes, but only ${#CLOUDLAB_NODES[@]} available"
    exit 1
fi

DEPLOY_NODES=("${CLOUDLAB_NODES[@]:0:$NUM_NODES}")

log_info "=========================================="
log_info "Deploying Disaggregated LLM System"
log_info "=========================================="
echo ""
log_info "Deploying to ${NUM_NODES} nodes:"
for i in "${!DEPLOY_NODES[@]}"; do
    echo "  [$i] ${DEPLOY_NODES[$i]}"
done
echo ""

# ============================================
# Step 1: Build Locally (Optional)
# ============================================

if [ "${BUILD_LOCAL:-1}" == "1" ]; then
    log_step "Building locally..."
    cd "${LOCAL_DIR}"

    if [ ! -d "build" ]; then
        mkdir -p build
    fi

    cd build
    cmake .. && make -j$(nproc)

    if [ $? -eq 0 ]; then
        log_success "Local build completed"
    else
        log_error "Local build failed"
        exit 1
    fi

    cd "${LOCAL_DIR}"
    echo ""
fi

# ============================================
# Step 2: Sync Code to Remote Nodes
# ============================================

log_step "Syncing code to remote nodes..."

SYNC_PIDS=()

for NODE in "${DEPLOY_NODES[@]}"; do
    HOST=$(get_full_hostname "$NODE")

    log_info "Syncing to ${HOST}..."

    # Create remote directory
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "mkdir -p ${REMOTE_DIR}" &

    SYNC_PIDS+=($!)
done

# Wait for directory creation
for pid in "${SYNC_PIDS[@]}"; do
    wait $pid
done

# Rsync code
RSYNC_PIDS=()

for NODE in "${DEPLOY_NODES[@]}"; do
    HOST=$(get_full_hostname "$NODE")

    rsync -avz ${RSYNC_EXCLUDE} \
        -e "ssh ${SSH_OPTS[*]}" \
        "${LOCAL_DIR}/" \
        "${USERNAME}@${HOST}:${REMOTE_DIR}/" &

    RSYNC_PIDS+=($!)
done

# Wait for all rsync operations
for pid in "${RSYNC_PIDS[@]}"; do
    wait $pid
done

log_success "Code synced to all nodes"
echo ""

# ============================================
# Step 3: Build on Remote Nodes
# ============================================

log_step "Building on remote nodes..."

BUILD_PIDS=()

for NODE in "${DEPLOY_NODES[@]}"; do
    HOST=$(get_full_hostname "$NODE")

    log_info "Building on ${HOST}..."

    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "${REMOTE_BUILD_CMD}" > /dev/null 2>&1 &

    BUILD_PIDS+=($!)
done

# Wait for all builds
for i in "${!BUILD_PIDS[@]}"; do
    pid="${BUILD_PIDS[$i]}"
    HOST=$(get_full_hostname "${DEPLOY_NODES[$i]}")

    if wait $pid; then
        log_success "Build completed on ${HOST}"
    else
        log_error "Build failed on ${HOST}"
    fi
done

echo ""

# ============================================
# Step 4: Install Python Bindings
# ============================================

log_step "Installing Python RDMA bindings..."

INSTALL_PIDS=()

for NODE in "${DEPLOY_NODES[@]}"; do
    HOST=$(get_full_hostname "$NODE")

    log_info "Installing bindings on ${HOST}..."

    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "cd ${REMOTE_DIR}/build && \
         cmake --install . --prefix ${REMOTE_DIR}/install && \
         export PYTHONPATH=${REMOTE_DIR}/install/python:\${PYTHONPATH}" > /dev/null 2>&1 &

    INSTALL_PIDS+=($!)
done

for pid in "${INSTALL_PIDS[@]}"; do
    wait $pid
done

log_success "Python bindings installed"
echo ""

# ============================================
# Step 5: Setup vLLM with RDMAConnector
# ============================================

log_step "Setting up vLLM connector..."

for NODE in "${DEPLOY_NODES[@]}"; do
    HOST=$(get_full_hostname "$NODE")

    log_info "Installing RDMAConnector on ${HOST}..."

    # Copy RDMAConnector to vLLM
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "mkdir -p ${VLLM_PATH}/vllm/distributed/kv_transfer/kv_connector/v1 && \
         cp ${REMOTE_DIR}/python/rdma_connector.py \
            ${VLLM_PATH}/vllm/distributed/kv_transfer/kv_connector/v1/" &
done

wait

log_success "vLLM connector installed"
echo ""

# ============================================
# Summary
# ============================================

log_success "=========================================="
log_success "Deployment completed successfully!"
log_success "=========================================="
echo ""
echo "Next steps:"
echo "  1. Start the system:  ./scripts/smart_run.sh [num_prefill] [num_decode]"
echo "  2. Test the system:   ./scripts/test_request.sh \"Your prompt here\""
echo "  3. Stop the system:   ./scripts/smart_stop.sh"
echo ""
echo "Example:"
echo "  ./scripts/smart_run.sh 2 2  # Start with 2 prefill nodes and 2 decode nodes"
echo ""
