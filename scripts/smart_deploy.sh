#!/bin/bash

# ============================================
# Smart Deploy - Deploy Disaggregated LLM System
# ============================================
# Builds and deploys the system to CloudLab nodes

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

# ============================================
# Interactive Configuration
# ============================================

echo ""
log_info "=========================================="
log_info "Disaggregated LLM System - Deployment Setup"
log_info "=========================================="
echo ""

# Show available nodes
log_info "Available CloudLab nodes:"
for i in "${!CLOUDLAB_NODES[@]}"; do
    echo "  [$i] ${CLOUDLAB_NODES[$i]}"
done
echo ""

# Get number of prefill nodes
if [ -z "$1" ]; then
    read -p "$(echo -e "${COLOR_CYAN}Number of prefill nodes [default: 2]: ${COLOR_RESET}")" NUM_PREFILL_NODES
    NUM_PREFILL_NODES=${NUM_PREFILL_NODES:-2}
else
    NUM_PREFILL_NODES=$1
fi

# Get number of decode nodes
if [ -z "$2" ]; then
    read -p "$(echo -e "${COLOR_CYAN}Number of decode nodes [default: 2]: ${COLOR_RESET}")" NUM_DECODE_NODES
    NUM_DECODE_NODES=${NUM_DECODE_NODES:-2}
else
    NUM_DECODE_NODES=$2
fi

# Calculate total nodes needed
TOTAL_NODES=$((1 + NUM_PREFILL_NODES + NUM_DECODE_NODES))

# Validate we have enough nodes
if [ $TOTAL_NODES -gt ${#CLOUDLAB_NODES[@]} ]; then
    log_error "Not enough CloudLab nodes. Need $TOTAL_NODES (1 orchestrator + $NUM_PREFILL_NODES prefill + $NUM_DECODE_NODES decode), have ${#CLOUDLAB_NODES[@]}"
    exit 1
fi

# Assign nodes
ORCHESTRATOR_NODE="${CLOUDLAB_NODES[0]}"
PREFILL_NODES=("${CLOUDLAB_NODES[@]:1:$NUM_PREFILL_NODES}")
DECODE_NODES=("${CLOUDLAB_NODES[@]:$((1+NUM_PREFILL_NODES)):$NUM_DECODE_NODES}")

DEPLOY_NODES=("${CLOUDLAB_NODES[@]:0:$TOTAL_NODES}")

echo ""
log_info "Deployment Configuration:"
echo "  Orchestrator: ${ORCHESTRATOR_NODE}"
echo "  Prefill nodes (${NUM_PREFILL_NODES}):"
for i in "${!PREFILL_NODES[@]}"; do
    echo "    [prefill-$(printf '%02d' $i)] ${PREFILL_NODES[$i]}"
done
echo "  Decode nodes (${NUM_DECODE_NODES}):"
for i in "${!DECODE_NODES[@]}"; do
    echo "    [decode-$(printf '%02d' $i)] ${DECODE_NODES[$i]}"
done
echo ""

# Confirm deployment
if [ -z "$3" ]; then
    read -p "$(echo -e "${COLOR_YELLOW}Proceed with deployment? [y/N]: ${COLOR_RESET}")" CONFIRM
    if [[ ! "$CONFIRM" =~ ^[Yy]$ ]]; then
        log_warning "Deployment cancelled"
        exit 0
    fi
else
    log_info "Auto-confirming deployment (non-interactive mode)"
fi

echo ""

# ============================================
# Step 0: Generate Configuration Files
# ============================================

log_step "Generating YAML configuration files..."

"${SCRIPT_DIR}/generate_configs.sh" $NUM_PREFILL_NODES $NUM_DECODE_NODES

if [ $? -eq 0 ]; then
    log_success "Configuration files generated"
else
    log_error "Failed to generate configuration files"
    exit 1
fi

echo ""

# ============================================
# Step 1: Sync Code to Remote Nodes
# ============================================

log_step "Syncing code and configs to remote nodes..."

SYNC_PIDS=()

for NODE in "${DEPLOY_NODES[@]}"; do
    HOST=$(get_full_hostname "$NODE")

    log_info "Syncing to ${HOST}..."

    # Create remote directory
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "mkdir -p ${REMOTE_DIR} ${REMOTE_DIR}/configs" &

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

log_success "Code and configs synced to all nodes"
echo ""

# ============================================
# Step 2: Build on Remote Nodes
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
# Step 3: Install Python Bindings
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
# Step 4: Setup vLLM with RDMAConnector
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
