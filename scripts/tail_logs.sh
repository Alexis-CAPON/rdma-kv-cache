#!/bin/bash

# ============================================
# Tail Logs - Open terminals for all node logs
# ============================================
# Opens terminal windows with tail -f for orchestrator, prefill, and decode logs

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

log_info "=========================================="
log_info "Opening Log Terminals"
log_info "=========================================="
echo ""

ALL_NODES=("${CLOUDLAB_NODES[@]}")

# Detect terminal emulator
if command -v osascript &> /dev/null && [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS with iTerm2 or Terminal.app
    TERMINAL_TYPE="macos"

    # Check if iTerm2 is available
    if osascript -e 'application "iTerm" version' &> /dev/null; then
        USE_ITERM=true
        log_info "Using iTerm2 for terminal windows"
    else
        USE_ITERM=false
        log_info "Using Terminal.app for terminal windows"
    fi
elif command -v gnome-terminal &> /dev/null; then
    TERMINAL_TYPE="gnome"
    log_info "Using GNOME Terminal for terminal windows"
elif command -v xterm &> /dev/null; then
    TERMINAL_TYPE="xterm"
    log_info "Using xterm for terminal windows"
else
    log_error "No supported terminal emulator found!"
    log_info "Please use tmux/screen or open terminals manually."
    exit 1
fi

echo ""
log_info "Opening terminals for ${#ALL_NODES[@]} nodes..."
echo ""

# Function to open terminal on macOS
open_macos_terminal() {
    local title=$1
    local ssh_cmd=$2

    if [ "$USE_ITERM" = true ]; then
        # iTerm2
        osascript <<EOF
tell application "iTerm"
    activate
    set newWindow to (create window with default profile)
    tell current session of newWindow
        set name to "${title}"
        write text "${ssh_cmd}"
    end tell
end tell
EOF
    else
        # Terminal.app
        osascript <<EOF
tell application "Terminal"
    activate
    do script "${ssh_cmd}"
    set custom title of front window to "${title}"
end tell
EOF
    fi
}

# Function to open terminal on Linux (GNOME)
open_gnome_terminal() {
    local title=$1
    local ssh_cmd=$2

    gnome-terminal --title="${title}" -- bash -c "${ssh_cmd}; exec bash" &
}

# Function to open terminal on Linux (xterm)
open_xterm_terminal() {
    local title=$1
    local ssh_cmd=$2

    xterm -title "${title}" -e "${ssh_cmd}; exec bash" &
}

# Build SSH options string for commands
SSH_OPTS_STR=""
for opt in "${SSH_OPTS[@]}"; do
    SSH_OPTS_STR+="$opt "
done

# Open terminals for each node
for i in "${!ALL_NODES[@]}"; do
    NODE="${ALL_NODES[$i]}"
    HOST=$(get_full_hostname "$NODE")

    # Orchestrator log (only on first node)
    if [ $i -eq 0 ]; then
        TITLE="[${NODE}] Orchestrator"
        SSH_CMD="ssh ${SSH_OPTS_STR} ${USERNAME}@${HOST} 'tail -f ${REMOTE_DIR}/logs/orchestrator.log'"

        log_info "Opening: ${TITLE}"

        case $TERMINAL_TYPE in
            macos)
                open_macos_terminal "$TITLE" "$SSH_CMD"
                ;;
            gnome)
                open_gnome_terminal "$TITLE" "$SSH_CMD"
                ;;
            xterm)
                open_xterm_terminal "$TITLE" "$SSH_CMD"
                ;;
        esac

        sleep 0.5
    fi

    # Prefill node log
    TITLE="[${NODE}] Prefill-$(printf '%02d' $i)"
    SSH_CMD="ssh ${SSH_OPTS_STR} ${USERNAME}@${HOST} 'tail -f ${REMOTE_DIR}/logs/prefill-$(printf '%02d' $i)-node.log'"

    log_info "Opening: ${TITLE}"

    case $TERMINAL_TYPE in
        macos)
            open_macos_terminal "$TITLE" "$SSH_CMD"
            ;;
        gnome)
            open_gnome_terminal "$TITLE" "$SSH_CMD"
            ;;
        xterm)
            open_xterm_terminal "$TITLE" "$SSH_CMD"
            ;;
    esac

    sleep 0.5

    # Decode node log
    TITLE="[${NODE}] Decode-$(printf '%02d' $i)"
    SSH_CMD="ssh ${SSH_OPTS_STR} ${USERNAME}@${HOST} 'tail -f ${REMOTE_DIR}/logs/decode-$(printf '%02d' $i)-node.log'"

    log_info "Opening: ${TITLE}"

    case $TERMINAL_TYPE in
        macos)
            open_macos_terminal "$TITLE" "$SSH_CMD"
            ;;
        gnome)
            open_gnome_terminal "$TITLE" "$SSH_CMD"
            ;;
        xterm)
            open_xterm_terminal "$TITLE" "$SSH_CMD"
            ;;
    esac

    sleep 0.5
done

echo ""
log_success "All terminal windows opened!"
log_info "You should see $(( ${#ALL_NODES[@]} * 2 + 1 )) terminal windows:"
log_info "  - 1 orchestrator log"
log_info "  - ${#ALL_NODES[@]} prefill node logs"
log_info "  - ${#ALL_NODES[@]} decode node logs"
echo ""
