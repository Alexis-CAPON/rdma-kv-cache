#!/bin/bash

# ============================================
# Install Dependencies on All CloudLab Nodes
# ============================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

log_info "=========================================="
log_info "Installing Dependencies on All Nodes"
log_info "=========================================="
echo ""
log_info "Target nodes: ${#CLOUDLAB_NODES[@]}"
for i in "${!CLOUDLAB_NODES[@]}"; do
    echo "  [$i] ${CLOUDLAB_NODES[$i]}"
done
echo ""

read -p "Install dependencies on all nodes? (y/n) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    log_error "Installation cancelled"
    exit 1
fi

# ============================================
# Step 1: Copy install script to all nodes
# ============================================

log_step "Copying install script to all nodes..."

COPY_PIDS=()

for NODE in "${CLOUDLAB_NODES[@]}"; do
    HOST=$(get_full_hostname "$NODE")

    log_info "Copying to ${HOST}..."

    scp "${SSH_OPTS[@]}" \
        "${SCRIPT_DIR}/install_dependencies.sh" \
        "${USERNAME}@${HOST}:/tmp/install_dependencies.sh" &

    COPY_PIDS+=($!)
done

# Wait for all copies
for pid in "${COPY_PIDS[@]}"; do
    wait $pid
done

log_success "Install script copied to all nodes"
echo ""

# ============================================
# Step 2: Run installation on all nodes
# ============================================

log_step "Running installation on all nodes (this may take 15-30 minutes)..."
echo ""

INSTALL_PIDS=()
LOG_FILES=()

for i in "${!CLOUDLAB_NODES[@]}"; do
    NODE="${CLOUDLAB_NODES[$i]}"
    HOST=$(get_full_hostname "$NODE")
    LOG_FILE="/tmp/install_${NODE//[.:]/_}.log"
    LOG_FILES+=("$LOG_FILE")

    log_info "Starting installation on ${HOST}..."
    log_info "  Log: $LOG_FILE"

    # Run installation in background, redirect output to log file
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "chmod +x /tmp/install_dependencies.sh && \
         bash /tmp/install_dependencies.sh 2>&1" \
        > "$LOG_FILE" 2>&1 &

    INSTALL_PIDS+=($!)

    # Small delay to stagger connections
    sleep 2
done

echo ""
log_info "All installations running in parallel..."
log_info "Monitoring progress (Ctrl+C to stop monitoring, installations will continue)..."
echo ""

# Monitor progress with detailed status
TOTAL=${#INSTALL_PIDS[@]}
START_TIME=$(date +%s)

while true; do
    COMPLETED=0
    RUNNING=0

    # Clear screen and show status
    clear
    echo "=========================================="
    echo "Installation Progress"
    echo "=========================================="
    echo ""

    # Check status for each node
    for i in "${!INSTALL_PIDS[@]}"; do
        pid="${INSTALL_PIDS[$i]}"
        NODE="${CLOUDLAB_NODES[$i]}"
        HOST=$(get_full_hostname "$NODE")
        LOG_FILE="${LOG_FILES[$i]}"

        if ! kill -0 $pid 2>/dev/null; then
            # Process finished - check if successful
            if grep -q "Installation Complete!" "$LOG_FILE" 2>/dev/null; then
                STATUS="✓ COMPLETED"
            else
                STATUS="✗ FAILED"
            fi
            COMPLETED=$((COMPLETED + 1))
        else
            # Still running - show latest activity
            RUNNING=$((RUNNING + 1))
            if [ -f "$LOG_FILE" ]; then
                LAST_LINE=$(tail -1 "$LOG_FILE" 2>/dev/null | sed 's/^[[:space:]]*//' | cut -c1-45)
                if [ -n "$LAST_LINE" ]; then
                    STATUS="⏳ ${LAST_LINE}..."
                else
                    STATUS="⏳ RUNNING"
                fi
            else
                STATUS="⏳ STARTING"
            fi
        fi

        printf "  [%d] %-35s %s\n" "$i" "$HOST" "$STATUS"
    done

    echo ""
    echo "=========================================="

    # Calculate elapsed time
    ELAPSED=$(($(date +%s) - START_TIME))
    MINS=$((ELAPSED / 60))
    SECS=$((ELAPSED % 60))

    printf "Status: %d/%d completed, %d running | Elapsed: %dm %ds\n" \
        "$COMPLETED" "$TOTAL" "$RUNNING" "$MINS" "$SECS"

    # Break if all completed
    if [ $COMPLETED -ge $TOTAL ]; then
        break
    fi

    sleep 3
done

echo ""

# ============================================
# Step 3: Check results
# ============================================

log_step "Checking installation results..."
echo ""

ALL_SUCCESS=true

for i in "${!CLOUDLAB_NODES[@]}"; do
    NODE="${CLOUDLAB_NODES[$i]}"
    HOST=$(get_full_hostname "$NODE")
    LOG_FILE="${LOG_FILES[$i]}"

    echo "=========================================="
    echo "Node: ${HOST}"
    echo "=========================================="

    # Check if installation succeeded
    if grep -q "Installation Complete!" "$LOG_FILE"; then
        log_success "Installation SUCCEEDED"

        # Show summary
        echo ""
        echo "Summary:"
        grep -A 10 "System Information:" "$LOG_FILE" | head -15 || true

    else
        log_error "Installation FAILED or INCOMPLETE"
        ALL_SUCCESS=false

        echo ""
        echo "Last 20 lines of log:"
        tail -20 "$LOG_FILE"
    fi

    echo ""
    echo "Full log: $LOG_FILE"
    echo ""
done

# ============================================
# Step 4: Summary and next steps
# ============================================

echo "=========================================="
if [ "$ALL_SUCCESS" = true ]; then
    log_success "All installations completed successfully!"
else
    log_warning "Some installations failed. Check logs above."
fi
echo "=========================================="
echo ""
echo "Next steps:"
echo "  1. Review logs: ${LOG_FILES[@]}"
echo "  2. REBOOT all nodes if CUDA was installed:"
echo "     ./scripts/reboot_all_nodes.sh"
echo "  3. After reboot, verify GPUDirect RDMA:"
echo "     ./scripts/verify_gpudirect.sh"
echo "  4. Deploy and build code:"
echo "     ./scripts/smart_deploy.sh"
echo ""
echo "Individual node verification:"
for NODE in "${CLOUDLAB_NODES[@]}"; do
    HOST=$(get_full_hostname "$NODE")
    echo "  ssh ${USERNAME}@${HOST}"
    echo "    nvidia-smi"
    echo "    ibv_devices"
    echo "    lsmod | grep nvidia_peermem"
    echo ""
done
