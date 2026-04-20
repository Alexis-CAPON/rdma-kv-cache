#!/bin/bash

# ============================================
# Reboot All CloudLab Nodes
# ============================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

log_warning "=========================================="
log_warning "REBOOT ALL NODES"
log_warning "=========================================="
echo ""
log_info "This will reboot ${#CLOUDLAB_NODES[@]} nodes:"
for NODE in "${CLOUDLAB_NODES[@]}"; do
    echo "  - ${NODE}"
done
echo ""

read -p "Are you sure you want to reboot all nodes? (y/n) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    log_error "Reboot cancelled"
    exit 1
fi

log_step "Rebooting all nodes..."

for NODE in "${CLOUDLAB_NODES[@]}"; do
    HOST=$(get_full_hostname "$NODE")
    log_info "Rebooting ${HOST}..."

    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "sudo reboot" &
done

wait

log_success "Reboot commands sent to all nodes"
echo ""
log_info "Nodes are rebooting... wait ~3-5 minutes before reconnecting"
echo ""
