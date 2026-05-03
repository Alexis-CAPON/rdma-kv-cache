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
IB_PORT="${IB_PORT:-1}"
ORCHESTRATOR_PORT="${ORCHESTRATOR_PORT:-6010}"
ORCHESTRATOR_NODE_PORT="${ORCHESTRATOR_NODE_PORT:-6011}"  # Orchestrator port for node (prefill/decode) connections
VLLM_PREFILL_BASE_PORT="${VLLM_PREFILL_BASE_PORT:-6020}"  # Prefill nodes: 6020, 6021, 6022...
VLLM_DECODE_BASE_PORT="${VLLM_DECODE_BASE_PORT:-6030}"   # Decode nodes: 6030, 6031, 6032...
NODE_TCP_BASE_PORT="${NODE_TCP_BASE_PORT:-6040}"     # C++ node TCP: 6040, 6041, 6042...

# Model configuration
MODEL_NAME="${MODEL_NAME:-meta-llama/Llama-2-7b-hf}"
MODEL_MAX_LEN="${MODEL_MAX_LEN:-4096}"
GPU_MEMORY_UTILIZATION="${GPU_MEMORY_UTILIZATION:-0.9}"
TENSOR_PARALLEL_SIZE="${TENSOR_PARALLEL_SIZE:-1}"

# Transfer backend selection
# USE_GPU=true  → GPUDirect RDMA (RDMAConnector, requires nvidia-peermem + A100-class GPU)
# USE_GPU=false → Standard RDMA via MooncakeConnector (no GPU required, works on CPU-only servers)
USE_GPU="${USE_GPU:-true}"

# vLLM installation path
VLLM_PATH="/users/${USERNAME}/vllm"

# Deployment settings
RSYNC_EXCLUDE="--exclude='build/' --exclude='logs/' --exclude='.git/' --exclude='vllm/' --exclude='*.o' --exclude='*.a'"

# Build command on remote server
# The CMake config will auto-detect CUDA availability and fallback to CPU-only if needed
if [ "$USE_GPU" = "true" ]; then
    # Try GPU mode first - CMake will auto-fallback to CPU if CUDA not available
    # Ensure CUDA is in PATH for nvcc detection
    REMOTE_BUILD_CMD="cd ${REMOTE_DIR} && rm -rf build && mkdir -p build && cd build && export PATH=/usr/local/cuda/bin:/usr/bin:\$PATH && cmake .. -DENABLE_GPU_DIRECT=ON && make -j\$(nproc)"
else
    # Force CPU-only mode
    REMOTE_BUILD_CMD="cd ${REMOTE_DIR} && rm -rf build && mkdir -p build && cd build && cmake .. -DENABLE_GPU_DIRECT=OFF && make -j\$(nproc)"
fi

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

# ============================================
# YAML Configuration Defaults
# ============================================
# These values can be overridden in .env file

# Cluster settings
REPLICATION_FACTOR="${REPLICATION_FACTOR:-3}"
NUMBER_OF_KEYS_HASHTABLE="${NUMBER_OF_KEYS_HASHTABLE:-10000}"
SIZE_LOCAL_BUFFER="${SIZE_LOCAL_BUFFER:-1024}"
EVENT_QUEUE_SIZE="${EVENT_QUEUE_SIZE:-1000}"
WORKER_POOL_SIZE="${WORKER_POOL_SIZE:-4}"
VNODES_NUMBER="${VNODES_NUMBER:-1}"
NUM_LAYERS="${NUM_LAYERS:-32}"

# RDMA/InfiniBand Configuration
NODE_GID_INDEX="${NODE_GID_INDEX:-0}"              # 0=IB, 3=RoCEv2
NODE_MTU="${NODE_MTU:-0}"                          # Path MTU: 0=auto-discover (recommended), or override with 256/512/1024/2048/4096
NODE_SL="${NODE_SL:-0}"                            # Service Level
NODE_QP_MAX_SEND_WR="${NODE_QP_MAX_SEND_WR:-64}"
NODE_QP_MAX_RECV_WR="${NODE_QP_MAX_RECV_WR:-64}"
NODE_QP_MAX_INLINE_DATA="${NODE_QP_MAX_INLINE_DATA:-64}"
NODE_MAX_RD_ATOMIC="${NODE_MAX_RD_ATOMIC:-16}"
NODE_MIN_RNR_TIMER="${NODE_MIN_RNR_TIMER:-12}"     # ~0.64 ms
NODE_TIMEOUT="${NODE_TIMEOUT:-14}"                 # local ACK timeout (~67 ms)
NODE_RETRY_CNT="${NODE_RETRY_CNT:-7}"
NODE_RNR_RETRY="${NODE_RNR_RETRY:-7}"              # 7 = infinite
NODE_CQ_DEPTH="${NODE_CQ_DEPTH:-128}"

# GPU Configuration
NODE_ENABLE_PEER_ACCESS="${NODE_ENABLE_PEER_ACCESS:-true}"
NODE_REQUIRE_SAME_NUMA="${NODE_REQUIRE_SAME_NUMA:-false}"

# Memory Configuration
MEMORY_KV_BUFFER_MB="${MEMORY_KV_BUFFER_MB:-16384}"           # Total GPU memory buffer (16 GB)
MEMORY_LAYER_SIZE_MB="${MEMORY_LAYER_SIZE_MB:-128}"           # Per-layer KV cache size (128 MB default)
MEMORY_CHUNK_SIZE_MB="${MEMORY_CHUNK_SIZE_MB:-64}"             # Size of each chunk (64-128 MB recommended)
MEMORY_NUM_KV_CHUNKS="${MEMORY_NUM_KV_CHUNKS:-32}"             # Number of chunks per request
MEMORY_MR_RELAXED_ORDERING="${MEMORY_MR_RELAXED_ORDERING:-true}"     # Enable PCIe relaxed ordering for better BW
MEMORY_MAX_CONCURRENT_REQUESTS="${MEMORY_MAX_CONCURRENT_REQUESTS:-8}"    # Max simultaneous KV transfers

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
export ORCHESTRATOR_NODE_PORT
export VLLM_PREFILL_BASE_PORT
export VLLM_DECODE_BASE_PORT
export NODE_TCP_BASE_PORT
export MODEL_NAME
export MODEL_MAX_LEN
export GPU_MEMORY_UTILIZATION
export TENSOR_PARALLEL_SIZE
export VLLM_PATH
export USE_GPU

# Export YAML configuration variables
export REPLICATION_FACTOR
export NUMBER_OF_KEYS_HASHTABLE
export SIZE_LOCAL_BUFFER
export EVENT_QUEUE_SIZE
export WORKER_POOL_SIZE
export VNODES_NUMBER
export NUM_LAYERS
export NODE_GID_INDEX
export NODE_MTU
export NODE_SL
export NODE_QP_MAX_SEND_WR
export NODE_QP_MAX_RECV_WR
export NODE_QP_MAX_INLINE_DATA
export NODE_MAX_RD_ATOMIC
export NODE_MIN_RNR_TIMER
export NODE_TIMEOUT
export NODE_RETRY_CNT
export NODE_RNR_RETRY
export NODE_CQ_DEPTH
export NODE_ENABLE_PEER_ACCESS
export NODE_REQUIRE_SAME_NUMA
export MEMORY_KV_BUFFER_MB
export MEMORY_LAYER_SIZE_MB
export MEMORY_CHUNK_SIZE_MB
export MEMORY_NUM_KV_CHUNKS
export MEMORY_MR_RELAXED_ORDERING
export MEMORY_MAX_CONCURRENT_REQUESTS
