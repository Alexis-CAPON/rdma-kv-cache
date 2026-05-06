#!/bin/bash

# ============================================
# Kill All Processes - Quick shutdown script
# ============================================
# Kills all user processes on all CloudLab nodes

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

log_info "=========================================="
log_info "Killing All Processes on CloudLab Nodes"
log_info "=========================================="
echo ""

ALL_NODES=("${CLOUDLAB_NODES[@]}")

log_info "Killing processes on ${#ALL_NODES[@]} nodes..."
echo ""

# Kill all processes in parallel
KILL_PIDS=()

for NODE in "${ALL_NODES[@]}"; do
    HOST=$(get_full_hostname "$NODE")

    log_info "Killing all processes on ${HOST}..."

    # Kill all processes belonging to the user
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "pkill -9 -u ${USERNAME}" &

    KILL_PIDS+=($!)
done

# Wait for all kill commands to complete
for pid in "${KILL_PIDS[@]}"; do
    wait $pid 2>/dev/null
done

echo ""
log_success "All processes killed on all nodes!"
echo ""
