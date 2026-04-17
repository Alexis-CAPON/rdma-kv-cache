#!/bin/bash

# Smart monitor script - monitors RHT cluster health on CloudLab
# Collects data from each node separately (no shared filesystem)

source deploy_config.sh

if [ ! -f "$SELECTED_SERVERS_FILE" ]; then
    log_error "No servers selected. Run ./smart_deploy.sh first."
    exit 1
fi

SELECTED_NODES=($(cat "$SELECTED_SERVERS_FILE"))
NUM_NODES=${#SELECTED_NODES[@]}

# Monitoring interval (seconds)
INTERVAL=${1:-5}

echo "=========================================="
echo "RHT Fault-Tolerant Cluster Monitor"
echo "=========================================="
echo "Nodes: $NUM_NODES"
echo "Refresh interval: ${INTERVAL}s"
echo "Press Ctrl+C to stop"
echo ""

while true; do
    clear
    echo "=========================================="
    echo "RHT Cluster Status - $(date)"
    echo "=========================================="
    echo ""

    printf "%-30s %-12s %-15s %-15s %-10s\n" "NODE" "STATUS" "CLIENT PORT" "SERVER PORT" "PID"
    echo "--------------------------------------------------------------------------------------------"

    # Collect status from all nodes in parallel
    # Launch status checks in background
    PIDS=()
    for i in $(seq 0 $((NUM_NODES - 1))); do
        (
            NODE="${SELECTED_NODES[$i]}"
            FULL_HOST=$(get_full_hostname "$NODE")

            # Check if rht_node is running and get PID
            PID=$(ssh "${SSH_OPTS[@]}" -o ConnectTimeout=2 "${USERNAME}@${FULL_HOST}" \
                "ps aux | grep -E '[r]ht_node.*--config' | grep -v grep | awk '{print \$2}' | head -1" 2>/dev/null)

            if [ -n "$PID" ]; then
                STATUS="RUNNING"
            else
                STATUS="STOPPED"
            fi

            # Check if ports are listening
            CLIENT_PORT=$(ssh "${SSH_OPTS[@]}" -o ConnectTimeout=2 "${USERNAME}@${FULL_HOST}" \
                "netstat -tuln 2>/dev/null | grep ':${BASE_CLIENT_PORT} ' > /dev/null && echo 'LISTENING' || echo 'CLOSED'" 2>/dev/null)

            SERVER_PORT=$(ssh "${SSH_OPTS[@]}" -o ConnectTimeout=2 "${USERNAME}@${FULL_HOST}" \
                "netstat -tuln 2>/dev/null | grep ':${BASE_SERVER_PORT} ' > /dev/null && echo 'LISTENING' || echo 'CLOSED'" 2>/dev/null)

            # Write to temp file (can't use associative arrays across subshells easily)
            echo "$STATUS" > "/tmp/rht_mon_${i}_status"
            echo "$CLIENT_PORT" > "/tmp/rht_mon_${i}_client"
            echo "$SERVER_PORT" > "/tmp/rht_mon_${i}_server"
            echo "$PID" > "/tmp/rht_mon_${i}_pid"
        ) &
        PIDS+=($!)
    done

    # Wait for all status checks
    for pid in "${PIDS[@]}"; do
        wait $pid 2>/dev/null
    done

    # Display results
    for i in $(seq 0 $((NUM_NODES - 1))); do
        NODE="${SELECTED_NODES[$i]}"
        FULL_HOST=$(get_full_hostname "$NODE")

        # Read from temp files
        STATUS=$(cat "/tmp/rht_mon_${i}_status" 2>/dev/null || echo "UNKNOWN")
        CLIENT_PORT=$(cat "/tmp/rht_mon_${i}_client" 2>/dev/null || echo "UNKNOWN")
        SERVER_PORT=$(cat "/tmp/rht_mon_${i}_server" 2>/dev/null || echo "UNKNOWN")
        PID=$(cat "/tmp/rht_mon_${i}_pid" 2>/dev/null || echo "-")

        # Color code status
        if [ "$STATUS" == "RUNNING" ]; then
            STATUS_COLOR="${COLOR_GREEN}RUNNING${COLOR_RESET}"
        elif [ "$STATUS" == "STOPPED" ]; then
            STATUS_COLOR="${COLOR_RED}STOPPED${COLOR_RESET}"
        else
            STATUS_COLOR="${COLOR_YELLOW}UNKNOWN${COLOR_RESET}"
        fi

        printf "%-40s %-22s %-15s %-15s %-10s\n" "$FULL_HOST" "$STATUS_COLOR" "$CLIENT_PORT" "$SERVER_PORT" "$PID"
    done

    echo ""
    echo "=========================================="
    echo "Recent Log Entries (last 3 lines per node)"
    echo "=========================================="

    for i in $(seq 0 $((NUM_NODES - 1))); do
        NODE="${SELECTED_NODES[$i]}"
        FULL_HOST=$(get_full_hostname "$NODE")

        echo ""
        echo -e "${COLOR_CYAN}[$FULL_HOST]${COLOR_RESET}"

        # Get last 3 lines from log
        ssh "${SSH_OPTS[@]}" -o ConnectTimeout=2 "${USERNAME}@${FULL_HOST}" \
            "tail -n 3 ${REMOTE_DIR}/logs/${NODE}.log 2>/dev/null || echo '  No log file'" 2>/dev/null | \
            sed 's/^/  /'
    done

    echo ""
    echo "--------------------------------------------------------------------------------------------"
    echo "Refreshing in ${INTERVAL}s... (Ctrl+C to stop)"

    # Clean up temp files
    rm -f /tmp/rht_mon_*_status /tmp/rht_mon_*_client /tmp/rht_mon_*_server /tmp/rht_mon_*_pid 2>/dev/null

    sleep $INTERVAL
done
