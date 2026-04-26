#!/bin/bash

# ============================================
# Disaggregated LLM Inference Deployment Configuration
# ============================================
# CloudLab deployment for RDMA-based KV cache transfer

# Load environment variables from .env file
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
ENV_FILE="${PROJECT_ROOT}/.env"

if [ -f "$ENV_FILE" ]; then
    echo "Loading configuration from .env file..."
    # Export variables from .env
    set -a
    source "$ENV_FILE"
    set +a
else
    echo "WARNING: .env file not found at ${ENV_FILE}"
    echo "Using default configuration. Copy .env.example to .env to customize."
fi

# ============================================
# Default Configuration (fallback if .env not present)
# ============================================

# Your CloudLab username
USERNAME="${CLOUDLAB_USERNAME:-Alexis}"

# Parse CLOUDLAB_NODES from .env (comma-separated) or use defaults
if [ -n "$CLOUDLAB_NODES" ]; then
    IFS=',' read -r -a NODE_LIST <<< "$CLOUDLAB_NODES"
    # Add domain to each node if not already present
    CLOUDLAB_NODES=()
    for node in "${NODE_LIST[@]}"; do
        node=$(echo "$node" | xargs)  # trim whitespace
        if [[ "$node" == *"."* ]]; then
            CLOUDLAB_NODES+=("$node")
        else
            CLOUDLAB_NODES+=("${node}.${CLOUDLAB_DOMAIN:-clemson.cloudlab.us}")
        fi
    done
else
    # Default node list
    CLOUDLAB_NODES=(
        "clgpu014.clemson.cloudlab.us"
        "clgpu012.clemson.cloudlab.us"
        "clgpu015.clemson.cloudlab.us"
        "clgpu013.clemson.cloudlab.us"
        "clgpu021.clemson.cloudlab.us"
    )
fi

# SSH configuration
SSH_KEY="${SSH_KEY_PATH:-$HOME/.ssh/id_cloudlab_rh}"
SSH_OPTS=()
if [ "${SSH_STRICT_HOST_CHECKING:-no}" = "no" ]; then
    SSH_OPTS+=(-o StrictHostKeyChecking=no)
fi
if [ -n "${SSH_USER_KNOWN_HOSTS_FILE}" ]; then
    SSH_OPTS+=(-o UserKnownHostsFile="${SSH_USER_KNOWN_HOSTS_FILE}")
fi
if [ -f "$SSH_KEY" ]; then
    SSH_OPTS+=(-i "$SSH_KEY")
fi

# Remote directory on CloudLab nodes
REMOTE_DIR="${REMOTE_DIR:-/users/${USERNAME}/rdma-kv-cache}"

# Local directory
LOCAL_DIR="${LOCAL_DIR:-$PROJECT_ROOT}"

# Port configuration
ORCHESTRATOR_PORT=9000
VLLM_PREFILL_BASE_PORT=8100  # Prefill nodes: 8100, 8101, 8102...
VLLM_DECODE_BASE_PORT=8200   # Decode nodes: 8200, 8201, 8202...
NODE_TCP_BASE_PORT=18500     # C++ node TCP: 18500, 18501, 18502...

# Model configuration
MODEL_NAME="meta-llama/Llama-2-7b-hf"
MODEL_MAX_LEN=4096
GPU_MEMORY_UTILIZATION=0.9
TENSOR_PARALLEL_SIZE=1

# Transfer backend selection
# USE_GPU=true  → GPUDirect RDMA (RDMAConnector, requires nvidia-peermem + A100-class GPU)
# USE_GPU=false → Standard RDMA via MooncakeConnector (no GPU required, works on CPU-only servers)
USE_GPU="${USE_GPU:-true}"

# vLLM installation path
VLLM_PATH="/users/${USERNAME}/vllm"

# Deployment settings
RSYNC_EXCLUDE="--exclude='build/' --exclude='logs/' --exclude='.git/' --exclude='vllm/' --exclude='*.o' --exclude='*.a'"

# Build command on remote server
REMOTE_BUILD_CMD="cd ${REMOTE_DIR} && mkdir -p build && cd build && cmake .. && make -j\$(nproc)"

# vLLM build command (if needed)
VLLM_BUILD_CMD="cd ${VLLM_PATH} && pip install -e . --no-build-isolation"

# Dependency installation
INSTALL_DEPS_SCRIPT="install_dependencies.sh"

# Colors for output
COLOR_RESET='\033[0m'
COLOR_GREEN='\033[0;32m'
COLOR_RED='\033[0;31m'
COLOR_YELLOW='\033[1;33m'
COLOR_BLUE='\033[0;34m'
COLOR_CYAN='\033[0;36m'
COLOR_MAGENTA='\033[0;35m'

# Helper functions
log_info() {
    echo -e "${COLOR_CYAN}[INFO]${COLOR_RESET} $1"
}

log_success() {
    echo -e "${COLOR_GREEN}[SUCCESS]${COLOR_RESET} $1"
}

log_error() {
    echo -e "${COLOR_RED}[ERROR]${COLOR_RESET} $1"
}

log_warning() {
    echo -e "${COLOR_YELLOW}[WARNING]${COLOR_RESET} $1"
}

log_step() {
    echo -e "${COLOR_MAGENTA}[STEP]${COLOR_RESET} $1"
}

# Helper function to get full hostname
get_full_hostname() {
    local node=$1
    echo "$node"
}

# Export variables for other scripts
export USERNAME
export CLOUDLAB_NODES
export SSH_KEY
export SSH_OPTS
export REMOTE_DIR
export LOCAL_DIR
export ORCHESTRATOR_PORT
export VLLM_PREFILL_BASE_PORT
export VLLM_DECODE_BASE_PORT
export NODE_TCP_BASE_PORT
export MODEL_NAME
export MODEL_MAX_LEN
export GPU_MEMORY_UTILIZATION
export TENSOR_PARALLEL_SIZE
export VLLM_PATH
export USE_GPU
