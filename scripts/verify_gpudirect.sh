#!/bin/bash

# ============================================
# Verify GPUDirect RDMA on All Nodes
# ============================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

log_info "=========================================="
log_info "Verifying GPUDirect RDMA on All Nodes"
log_info "=========================================="
echo ""

ALL_OK=true

for NODE in "${CLOUDLAB_NODES[@]}"; do
    HOST=$(get_full_hostname "$NODE")

    echo "=========================================="
    echo "Node: ${HOST}"
    echo "=========================================="

    # Check if node is reachable
    if ! ssh "${SSH_OPTS[@]}" -o ConnectTimeout=5 "${USERNAME}@${HOST}" "echo ok" &>/dev/null; then
        log_error "Node unreachable (may still be booting)"
        ALL_OK=false
        echo ""
        continue
    fi

    # Check NVIDIA driver
    echo -n "  NVIDIA driver: "
    if ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" "nvidia-smi -L" 2>/dev/null | head -1; then
        log_success "OK"
    else
        log_error "FAILED"
        ALL_OK=false
    fi

    # Check InfiniBand
    echo -n "  InfiniBand devices: "
    IB_DEVICES=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" "ibv_devices 2>/dev/null | grep -c mlx || echo 0")
    if [ "$IB_DEVICES" -gt 0 ]; then
        echo "$IB_DEVICES found"
    else
        log_error "NONE FOUND"
        ALL_OK=false
    fi

    # Check GPUDirect module
    echo -n "  nvidia-peermem module: "
    if ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" "lsmod | grep -q nvidia_peermem"; then
        log_success "LOADED"
    else
        log_error "NOT LOADED"
        ALL_OK=false
    fi

    # Check CUDA in PyTorch
    echo -n "  PyTorch CUDA: "
    if ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "python3 -c 'import torch; print(\"OK\" if torch.cuda.is_available() else \"NO\")' 2>/dev/null" | grep -q "OK"; then
        log_success "OK"
    else
        log_error "FAILED"
        ALL_OK=false
    fi

    # Show GPU info
    echo ""
    echo "  GPU Info:"
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "nvidia-smi --query-gpu=index,name,memory.total --format=csv,noheader" 2>/dev/null | \
        sed 's/^/    /'

    echo ""
done

echo "=========================================="
if [ "$ALL_OK" = true ]; then
    log_success "All nodes verified successfully!"
    echo ""
    echo "System is ready for deployment:"
    echo "  ./scripts/smart_deploy.sh"
else
    log_warning "Some checks failed. Review output above."
    echo ""
    echo "If nvidia-peermem is not loaded, try:"
    echo "  ssh <node> 'sudo modprobe nvidia-peermem'"
fi
echo "=========================================="
echo ""
