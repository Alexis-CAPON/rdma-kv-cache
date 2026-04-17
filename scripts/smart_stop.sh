#!/bin/bash

# Smart stop script - stops all RHT nodes on CloudLab

source deploy_config.sh

if [ ! -f "$SELECTED_SERVERS_FILE" ]; then
    log_error "No servers selected. Run ./smart_deploy.sh first."
    exit 1
fi

SELECTED_NODES=($(cat "$SELECTED_SERVERS_FILE"))
NUM_NODES=${#SELECTED_NODES[@]}

# Determine coordinator node
COORDINATOR_NODE="${CLOUDLAB_NODES[$NUM_NODES]}"
COORDINATOR_FULL_HOST=$(get_full_hostname "$COORDINATOR_NODE")

echo "=========================================="
echo "Stopping RHT Fault-Tolerant Cluster"
echo "=========================================="
echo "RHT Nodes: $NUM_NODES"
echo "Coordinator: $COORDINATOR_FULL_HOST"
echo ""

# Stop coordinator first
log_info "Stopping coordinator on $COORDINATOR_FULL_HOST..."

# Kill all processes for user (except SSH)
ssh "${SSH_OPTS[@]}" "${USERNAME}@${COORDINATOR_FULL_HOST}" \
    "pkill -u ${USERNAME} -9 2>/dev/null; killall -u ${USERNAME} -9 2>/dev/null; echo 'Coordinator stopped'" &

COORD_STOP_PID=$!

# Stop all RHT nodes in parallel
STOP_PIDS=()
for i in $(seq 0 $((NUM_NODES - 1))); do
    NODE="${SELECTED_NODES[$i]}"
    FULL_HOST=$(get_full_hostname "$NODE")

    echo "Stopping node $i on $FULL_HOST..."

    # Kill all processes for user Alexis (except SSH)
    ssh "${SSH_OPTS[@]}" "${USERNAME}@${FULL_HOST}" \
        "pkill -u ${USERNAME} -9 2>/dev/null; killall -u ${USERNAME} -9 2>/dev/null; echo 'Stopped'" &

    STOP_PIDS+=($!)
done

# Wait for all to complete
wait $COORD_STOP_PID
wait

# Verify nodes are stopped
echo ""
log_info "Verifying cluster is stopped..."
echo ""

ALL_STOPPED=true

# Check coordinator
COORD_RUNNING=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${COORDINATOR_FULL_HOST}" \
    "ps aux | grep -E '[r]ht_coordinator.*--config' > /dev/null && echo 'yes' || echo 'no'" 2>/dev/null)

if [ "$COORD_RUNNING" == "no" ]; then
    log_success "Coordinator ($COORDINATOR_NODE) is STOPPED"
else
    log_error "Coordinator ($COORDINATOR_NODE) is STILL RUNNING"
    ALL_STOPPED=false
fi

# Check RHT nodes and benchmark clients
for i in $(seq 0 $((NUM_NODES - 1))); do
    NODE="${SELECTED_NODES[$i]}"
    FULL_HOST=$(get_full_hostname "$NODE")

    # Check if rht_node is still running
    NODE_RUNNING=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${FULL_HOST}" \
        "ps aux | grep -E '[r]ht_node.*--config' > /dev/null && echo 'yes' || echo 'no'" 2>/dev/null)

    # Check if benchmark client is still running
    CLIENT_RUNNING=$(ssh "${SSH_OPTS[@]}" "${USERNAME}@${FULL_HOST}" \
        "ps aux | grep -E '[r]ht_benchmark' > /dev/null && echo 'yes' || echo 'no'" 2>/dev/null)

    if [ "$NODE_RUNNING" == "no" ] && [ "$CLIENT_RUNNING" == "no" ]; then
        log_success "Node $i ($NODE) is STOPPED (server + client)"
    else
        if [ "$NODE_RUNNING" != "no" ]; then
            log_error "Node $i ($NODE) server is STILL RUNNING"
            ALL_STOPPED=false
        fi
        if [ "$CLIENT_RUNNING" != "no" ]; then
            log_error "Node $i ($NODE) client is STILL RUNNING"
            ALL_STOPPED=false
        fi
    fi
done

echo ""

if [ "$ALL_STOPPED" = true ]; then
    log_success "All nodes and coordinator stopped successfully!"
else
    log_warning "Some nodes are still running. Try running this script again."
fi
