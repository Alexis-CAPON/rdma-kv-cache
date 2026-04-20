#!/bin/bash

# ============================================
# Smart Stop - Stop Disaggregated LLM System
# ============================================
# Stops orchestrator, prefill nodes, decode nodes, and vLLM instances

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

log_info "=========================================="
log_info "Stopping Disaggregated LLM Inference System"
log_info "=========================================="
echo ""

# ============================================
# Get node list from CloudLab
# ============================================

ALL_NODES=("${CLOUDLAB_NODES[@]}")

log_info "Stopping all processes on ${#ALL_NODES[@]} nodes..."
echo ""

# ============================================
# Stop all processes on all nodes
# ============================================

STOP_PIDS=()

for NODE in "${ALL_NODES[@]}"; do
    HOST=$(get_full_hostname "$NODE")

    log_info "Stopping processes on ${HOST}..."

    # Kill all related processes:
    # - orchestrator
    # - prefill_node
    # - decode_node
    # - vllm python processes
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "pkill -f 'orchestrator --config' 2>/dev/null; \
         pkill -f 'prefill_node --config' 2>/dev/null; \
         pkill -f 'decode_node --config' 2>/dev/null; \
         pkill -f 'vllm.entrypoints.openai.api_server' 2>/dev/null; \
         echo 'Stopped'" &

    STOP_PIDS+=($!)
done

# Wait for all stop commands to complete
for pid in "${STOP_PIDS[@]}"; do
    wait $pid 2>/dev/null
done

echo ""
log_info "Waiting for processes to terminate..."
sleep 3
echo ""

# ============================================
# Verify all processes are stopped
# ============================================

log_info "Verifying all processes are stopped..."
echo ""

ALL_STOPPED=true

for NODE in "${ALL_NODES[@]}"; do
    HOST=$(get_full_hostname "$NODE")

    # Check for any remaining processes
    ORCH_RUNNING=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "pgrep -f 'orchestrator --config' > /dev/null && echo 'yes' || echo 'no'" 2>/dev/null)

    PREFILL_RUNNING=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "pgrep -f 'prefill_node --config' > /dev/null && echo 'yes' || echo 'no'" 2>/dev/null)

    DECODE_RUNNING=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "pgrep -f 'decode_node --config' > /dev/null && echo 'yes' || echo 'no'" 2>/dev/null)

    VLLM_RUNNING=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "pgrep -f 'vllm.entrypoints.openai.api_server' > /dev/null && echo 'yes' || echo 'no'" 2>/dev/null)

    if [ "$ORCH_RUNNING" == "no" ] && [ "$PREFILL_RUNNING" == "no" ] && \
       [ "$DECODE_RUNNING" == "no" ] && [ "$VLLM_RUNNING" == "no" ]; then
        log_success "${HOST}: All processes stopped"
    else
        RUNNING_PROCS=""
        [ "$ORCH_RUNNING" == "yes" ] && RUNNING_PROCS+="orchestrator "
        [ "$PREFILL_RUNNING" == "yes" ] && RUNNING_PROCS+="prefill_node "
        [ "$DECODE_RUNNING" == "yes" ] && RUNNING_PROCS+="decode_node "
        [ "$VLLM_RUNNING" == "yes" ] && RUNNING_PROCS+="vllm "
        log_error "${HOST}: Still running: ${RUNNING_PROCS}"
        ALL_STOPPED=false
    fi
done

echo ""

# ============================================
# Final Status
# ============================================

if [ "$ALL_STOPPED" = true ]; then
    log_success "All components stopped successfully!"
else
    log_warning "Some processes are still running."
    log_info "Force kill all user processes? (y/n)"
    read -r FORCE_KILL

    if [ "$FORCE_KILL" == "y" ]; then
        log_info "Force killing all processes..."

        for NODE in "${ALL_NODES[@]}"; do
            HOST=$(get_full_hostname "$NODE")
            ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
                "pkill -9 -f 'orchestrator' 2>/dev/null; \
                 pkill -9 -f 'prefill_node' 2>/dev/null; \
                 pkill -9 -f 'decode_node' 2>/dev/null; \
                 pkill -9 -f 'vllm' 2>/dev/null" &
        done

        wait
        sleep 2
        log_success "Force kill completed"
    fi
fi

echo ""
log_info "Logs are preserved in ${REMOTE_DIR}/logs/ on each node"
echo ""
