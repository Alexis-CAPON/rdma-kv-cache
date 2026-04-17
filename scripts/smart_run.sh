#!/bin/bash

# Smart run script - starts all RHT nodes on CloudLab

source deploy_config.sh

if [ ! -f "$SELECTED_SERVERS_FILE" ]; then
    log_error "No servers selected. Run ./smart_deploy.sh first."
    exit 1
fi

SELECTED_NODES=($(cat "$SELECTED_SERVERS_FILE"))
NUM_NODES=${#SELECTED_NODES[@]}

# Determine coordinator node (same logic as deploy script)
MAX_RHT_NODES=$((${#CLOUDLAB_NODES[@]} - 1))
COORDINATOR_NODE="${CLOUDLAB_NODES[$NUM_NODES]}"
COORDINATOR_FULL_HOST=$(get_full_hostname "$COORDINATOR_NODE")

echo "=========================================="
echo "Starting RHT Fault-Tolerant Cluster"
echo "=========================================="
echo "RHT Nodes: $NUM_NODES"
echo "Coordinator: $COORDINATOR_FULL_HOST"
echo ""

# Start coordinator FIRST (so it's listening when nodes try to connect)
log_info "Starting coordinator on $COORDINATOR_FULL_HOST..."
COORDINATOR_CONFIG="${COORDINATOR_NODE}.yaml"

ssh "${SSH_OPTS[@]}" "${USERNAME}@${COORDINATOR_FULL_HOST}" \
    "cd ${REMOTE_DIR} && mkdir -p logs && bash -c 'nohup ./build/rht_coordinator --config config/${COORDINATOR_CONFIG} > logs/${COORDINATOR_NODE}.log 2>&1 < /dev/null &' && sleep 0.5 && pgrep -f rht_coordinator"

if [ $? -eq 0 ]; then
    log_success "Coordinator started on $COORDINATOR_FULL_HOST"
else
    log_error "Failed to start coordinator"
    exit 1
fi

# Give coordinator time to initialize and start listening
echo ""
log_info "Waiting for coordinator to be ready..."
sleep 3
echo ""

# Now start all RHT nodes (they will connect to the running coordinator)
log_info "Starting RHT nodes..."
START_PIDS=()
for i in $(seq 0 $((NUM_NODES - 1))); do
    NODE="${SELECTED_NODES[$i]}"
    FULL_HOST=$(get_full_hostname "$NODE")
    CONFIG_FILE="${NODE}.yaml"

    echo "Starting node $i on $FULL_HOST..."

    # Start node in background on remote server with proper detachment
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${FULL_HOST}" \
        "cd ${REMOTE_DIR} && mkdir -p logs && bash -c 'nohup ./build/rht_node --config config/${CONFIG_FILE} > logs/${NODE}.log 2>&1 < /dev/null &' && echo 'started'" > /dev/null 2>&1 &

    START_PIDS+=($!)
done

# Wait for all SSH commands to complete
echo ""
log_info "Waiting for all nodes to start..."
for pid in "${START_PIDS[@]}"; do
    wait $pid
done

# Give nodes a moment to initialize and connect to coordinator
sleep 3

# Verify nodes are running
echo ""
log_info "Verifying nodes are running..."
echo ""

ALL_RUNNING=true
for i in $(seq 0 $((NUM_NODES - 1))); do
    NODE="${SELECTED_NODES[$i]}"
    FULL_HOST=$(get_full_hostname "$NODE")

    # Check if rht_node is running
    IS_RUNNING=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${FULL_HOST}" \
        "ps aux | grep -E '[r]ht_node.*--config' > /dev/null && echo 'yes' || echo 'no'" 2>/dev/null)

    if [ "$IS_RUNNING" == "yes" ]; then
        log_success "Node $i ($NODE) is RUNNING"
    else
        log_error "Node $i ($NODE) is NOT RUNNING"
        ALL_RUNNING=false
    fi
done

echo ""

if [ "$ALL_RUNNING" = true ]; then
    log_success "All nodes started successfully!"
else
    log_warning "Some nodes failed to start. Check logs."
    exit 1
fi

echo ""
echo "=========================================="
echo "Cluster Information"
echo "=========================================="
echo ""
echo "Coordinator: $COORDINATOR_FULL_HOST"
echo "  Log: ssh ${USERNAME}@${COORDINATOR_FULL_HOST} 'tail -f ${REMOTE_DIR}/logs/${COORDINATOR_NODE}.log'"
echo ""
echo "RHT Nodes:"
for i in $(seq 0 $((NUM_NODES - 1))); do
    NODE="${SELECTED_NODES[$i]}"
    FULL_HOST=$(get_full_hostname "$NODE")
    echo ""
    echo "  Node $i: $FULL_HOST"
    echo "    Log: ssh ${USERNAME}@${FULL_HOST} 'tail -f ${REMOTE_DIR}/logs/${NODE}.log'"
    echo "    Connect client: ./build/rht_client ${FULL_HOST}:${BASE_CLIENT_PORT}"
done

echo ""
log_info "Useful commands:"
echo "  Monitor cluster:  ./smart_monitor.sh"
echo "  Stop cluster:     ./smart_stop.sh"
echo "  Collect data:     ./smart_collect.sh"
