#!/bin/bash

# ============================================
# Local Deployment Test — 1 prefill + 1 decode + orchestrator
# use_gpu=true (GPUDirect RDMA path)
# ============================================
# Requirements:
#   - NVIDIA GPU (Volta/A100-class, sm_70+)
#   - Active InfiniBand / RoCE adapter (ibv_devinfo shows PORT_ACTIVE)
#   - Linux, CMake 3.18+, GCC 9+
#
# Usage:
#   ./scripts/test_local_deploy.sh [--skip-build] [--model <name>]
#
# Optional flags:
#   --skip-build        skip the cmake/make step (use existing build/)
#   --model <name>      vLLM model name (default: Qwen/Qwen3-8B)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ── Defaults ──────────────────────────────────────────────────────────────────
SKIP_BUILD=false
MODEL_NAME="Qwen/Qwen3-8B"
TIMEOUT_SECS=120   # how long to wait for RUNNING state

# ── Colours ───────────────────────────────────────────────────────────────────
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
RESET='\033[0m'

log_info()    { echo -e "${CYAN}[INFO]${RESET}    $*"; }
log_success() { echo -e "${GREEN}[PASS]${RESET}    $*"; }
log_error()   { echo -e "${RED}[FAIL]${RESET}    $*"; }
log_warning() { echo -e "${YELLOW}[WARN]${RESET}    $*"; }
log_step()    { echo -e "\n${CYAN}════════════════════════════════════════${RESET}"; echo -e "${CYAN}  $*${RESET}"; echo -e "${CYAN}════════════════════════════════════════${RESET}"; }

# ── Argument parsing ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=true; shift ;;
        --model)      MODEL_NAME="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Port assignments (all on localhost) ───────────────────────────────────────
ORCH_CLIENT_PORT=7001   # orchestrator port for external clients
ORCH_SERVER_PORT=7002   # orchestrator port for prefill/decode node connections
PREFILL_CLIENT_PORT=7010
PREFILL_SERVER_PORT=7011
DECODE_CLIENT_PORT=7020
DECODE_SERVER_PORT=7021
VLLM_PREFILL_PORT=8020
VLLM_DECODE_PORT=8030

# ── Working directories ───────────────────────────────────────────────────────
TEST_DIR="/tmp/rdma-kv-test"
LOG_DIR="${TEST_DIR}/logs"
CFG_DIR="${TEST_DIR}/configs"
BUILD_DIR="${PROJECT_ROOT}/build"

# ── PIDs ─────────────────────────────────────────────────────────────────────
ORCH_PID=""
PREFILL_PID=""
DECODE_PID=""

# ── Cleanup on exit ───────────────────────────────────────────────────────────
cleanup() {
    echo ""
    log_info "Cleaning up processes..."
    for pid in "$ORCH_PID" "$PREFILL_PID" "$DECODE_PID"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done
    # Wait briefly then force-kill stragglers
    sleep 2
    for pid in "$ORCH_PID" "$PREFILL_PID" "$DECODE_PID"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill -KILL "$pid" 2>/dev/null || true
        fi
    done
    log_info "Cleanup done. Logs are in ${LOG_DIR}"
}
trap cleanup EXIT

# ─────────────────────────────────────────────────────────────────────────────
# STEP 0 — Hardware preflight
# ─────────────────────────────────────────────────────────────────────────────
log_step "Hardware preflight (GPU + InfiniBand)"

GPU_OK=false
IB_OK=false

if command -v nvidia-smi &>/dev/null && nvidia-smi -L &>/dev/null; then
    GPU_COUNT=$(nvidia-smi -L | wc -l)
    log_success "Found ${GPU_COUNT} NVIDIA GPU(s):"
    nvidia-smi -L | sed 's/^/          /'
    GPU_OK=true
else
    log_error "No NVIDIA GPU detected (nvidia-smi not found or driver not loaded)."
    log_error "use_gpu=true requires an NVIDIA GPU. Aborting."
    exit 1
fi

if command -v ibv_devinfo &>/dev/null; then
    ACTIVE_PORTS=$(ibv_devinfo 2>/dev/null | grep -c "PORT_ACTIVE" || true)
    if [[ "$ACTIVE_PORTS" -gt 0 ]]; then
        log_success "Found ${ACTIVE_PORTS} active InfiniBand port(s):"
        ibv_devinfo 2>/dev/null | grep -E "hca_id|port:|state:" | sed 's/^/          /' || true
        IB_OK=true
    else
        log_error "No active InfiniBand ports found (ibv_devinfo shows no PORT_ACTIVE)."
        log_error "Check: ibstat / ifconfig for RoCE, or load the IB kernel modules."
        exit 1
    fi
else
    log_error "ibv_devinfo not found. Install libibverbs-utils."
    exit 1
fi

# ─────────────────────────────────────────────────────────────────────────────
# STEP 1 — Install build dependencies
# ─────────────────────────────────────────────────────────────────────────────
log_step "Installing build dependencies"

MISSING_PKGS=()

for pkg in \
    build-essential cmake git \
    libibverbs-dev librdmacm-dev ibverbs-utils \
    libyaml-cpp-dev \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    pybind11-dev; do

    if ! dpkg -s "$pkg" &>/dev/null; then
        MISSING_PKGS+=("$pkg")
    fi
done

if [[ ${#MISSING_PKGS[@]} -gt 0 ]]; then
    log_info "Installing missing packages: ${MISSING_PKGS[*]}"
    sudo apt-get update -qq
    sudo apt-get install -y "${MISSING_PKGS[@]}"
else
    log_info "All required system packages already installed"
fi

# Check for CUDA toolkit (nvcc)
if ! command -v nvcc &>/dev/null; then
    log_warning "nvcc not found — trying nvidia-cuda-toolkit"
    sudo apt-get install -y nvidia-cuda-toolkit 2>/dev/null || true
fi

if command -v nvcc &>/dev/null; then
    log_success "CUDA toolkit: $(nvcc --version | grep 'release' | awk '{print $5}' | tr -d ',')"
else
    log_error "CUDA toolkit (nvcc) not available. ENABLE_GPU_DIRECT requires nvcc."
    exit 1
fi

# ─────────────────────────────────────────────────────────────────────────────
# STEP 2 — Build
# ─────────────────────────────────────────────────────────────────────────────
log_step "Building project (ENABLE_GPU_DIRECT=ON)"

mkdir -p "${LOG_DIR}" "${CFG_DIR}"

if [[ "$SKIP_BUILD" == "true" ]]; then
    log_info "Skipping build (--skip-build)"
    if [[ ! -x "${BUILD_DIR}/cpp/orchestrator" ]]; then
        log_error "Build directory missing or binaries not found. Run without --skip-build first."
        exit 1
    fi
else
    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"

    log_info "Running cmake..."
    export PATH="/usr/local/cuda/bin:/usr/bin:${PATH}"
    cmake "${PROJECT_ROOT}" \
        -DENABLE_GPU_DIRECT=ON \
        -DBUILD_TESTS=OFF \
        -DBUILD_BENCHMARKS=OFF \
        2>&1 | tee "${LOG_DIR}/cmake.log"

    log_info "Building (orchestrator, prefill_node, decode_node)..."
    make -j"$(nproc)" orchestrator prefill_node decode_node \
        2>&1 | tee "${LOG_DIR}/build.log"

    cd "${PROJECT_ROOT}"
    log_success "Build complete"
fi

ORCHESTRATOR_BIN="${BUILD_DIR}/cpp/orchestrator"
PREFILL_BIN="${BUILD_DIR}/cpp/prefill_node"
DECODE_BIN="${BUILD_DIR}/cpp/decode_node"

for bin in "$ORCHESTRATOR_BIN" "$PREFILL_BIN" "$DECODE_BIN"; do
    if [[ ! -x "$bin" ]]; then
        log_error "Binary not found or not executable: ${bin}"
        exit 1
    fi
done

# ─────────────────────────────────────────────────────────────────────────────
# STEP 3 — Generate localhost configs
# ─────────────────────────────────────────────────────────────────────────────
log_step "Generating localhost YAML configs (use_gpu=true)"

# Memory sizing: small enough for testing (1 GB buffer, 4 layers × 8 MB × 2 requests = 64 MB required)
KV_BUFFER_MB=1024
NUM_LAYERS=4
LAYER_SIZE_MB=8
MAX_CONCURRENT_REQUESTS=2

cat > "${CFG_DIR}/orchestrator.yaml" <<YAML
# Orchestrator — test_local_deploy.sh
host: "localhost"
client_socket_port: ${ORCH_CLIENT_PORT}
server_socket_port: ${ORCH_SERVER_PORT}

orchestrator_host: "localhost"
orchestrator_port: ${ORCH_SERVER_PORT}

replication_factor: 1
number_of_keys_hashtable: 1000
size_local_buffer: 256
event_queue_size: 100
worker_pool_size: 2
vnodes_number: 1
num_layers: ${NUM_LAYERS}

use_gpu: true

orchestrator:
  expected_prefill_nodes: 1
  expected_decode_nodes: 1

node:
  role: "orchestrator"
  ib_port: 1
  gid_index: 0
  mtu: 0
  sl: 0
  qp_max_send_wr: 16
  qp_max_recv_wr: 16
  qp_max_inline_data: 64
  max_rd_atomic: 4
  min_rnr_timer: 12
  timeout: 14
  retry_cnt: 7
  rnr_retry: 7
  cq_depth: 32
  enable_peer_access: true
  require_same_numa: false

prefill_nodes:
  - host: "localhost"
    client_socket_port: ${PREFILL_CLIENT_PORT}
    server_socket_port: ${PREFILL_SERVER_PORT}

decode_nodes:
  - host: "localhost"
    client_socket_port: ${DECODE_CLIENT_PORT}
    server_socket_port: ${DECODE_SERVER_PORT}

peers:
  - host: "localhost"
    client_socket_port: ${PREFILL_CLIENT_PORT}
    server_socket_port: ${PREFILL_SERVER_PORT}
  - host: "localhost"
    client_socket_port: ${DECODE_CLIENT_PORT}
    server_socket_port: ${DECODE_SERVER_PORT}
YAML

cat > "${CFG_DIR}/prefill.yaml" <<YAML
# Prefill node — test_local_deploy.sh
host: "localhost"
client_socket_port: ${PREFILL_CLIENT_PORT}
server_socket_port: ${PREFILL_SERVER_PORT}

orchestrator_host: "localhost"
orchestrator_port: ${ORCH_SERVER_PORT}

vllm_port: ${VLLM_PREFILL_PORT}
model_name: "${MODEL_NAME}"

replication_factor: 1
number_of_keys_hashtable: 1000
size_local_buffer: 256
event_queue_size: 100
worker_pool_size: 2
vnodes_number: 1
num_layers: ${NUM_LAYERS}

use_gpu: true

node:
  role: "prefill"
  ib_port: 1
  gid_index: 0
  mtu: 0
  sl: 0
  qp_max_send_wr: 16
  qp_max_recv_wr: 16
  qp_max_inline_data: 64
  max_rd_atomic: 4
  min_rnr_timer: 12
  timeout: 14
  retry_cnt: 7
  rnr_retry: 7
  cq_depth: 32
  enable_peer_access: true
  require_same_numa: false

memory:
  kv_buffer_mb: ${KV_BUFFER_MB}
  layer_size_mb: ${LAYER_SIZE_MB}
  chunk_size_mb: 64
  num_kv_chunks: 4
  mr_relaxed_ordering: true
  max_concurrent_requests: ${MAX_CONCURRENT_REQUESTS}

prefill_nodes: []

decode_nodes:
  - host: "localhost"
    client_socket_port: ${DECODE_CLIENT_PORT}
    server_socket_port: ${DECODE_SERVER_PORT}

peers:
  - host: "localhost"
    client_socket_port: ${DECODE_CLIENT_PORT}
    server_socket_port: ${DECODE_SERVER_PORT}
YAML

cat > "${CFG_DIR}/decode.yaml" <<YAML
# Decode node — test_local_deploy.sh
host: "localhost"
client_socket_port: ${DECODE_CLIENT_PORT}
server_socket_port: ${DECODE_SERVER_PORT}

orchestrator_host: "localhost"
orchestrator_port: ${ORCH_SERVER_PORT}

vllm_port: ${VLLM_DECODE_PORT}
model_name: "${MODEL_NAME}"

replication_factor: 1
number_of_keys_hashtable: 1000
size_local_buffer: 256
event_queue_size: 100
worker_pool_size: 2
vnodes_number: 1
num_layers: ${NUM_LAYERS}

use_gpu: true

node:
  role: "decode"
  ib_port: 1
  gid_index: 0
  mtu: 0
  sl: 0
  qp_max_send_wr: 16
  qp_max_recv_wr: 16
  qp_max_inline_data: 64
  max_rd_atomic: 4
  min_rnr_timer: 12
  timeout: 14
  retry_cnt: 7
  rnr_retry: 7
  cq_depth: 32
  enable_peer_access: true
  require_same_numa: false

memory:
  kv_buffer_mb: ${KV_BUFFER_MB}
  layer_size_mb: ${LAYER_SIZE_MB}
  chunk_size_mb: 64
  num_kv_chunks: 4
  mr_relaxed_ordering: true
  max_concurrent_requests: ${MAX_CONCURRENT_REQUESTS}

prefill_nodes:
  - host: "localhost"
    client_socket_port: ${PREFILL_CLIENT_PORT}
    server_socket_port: ${PREFILL_SERVER_PORT}

decode_nodes: []

peers:
  - host: "localhost"
    client_socket_port: ${PREFILL_CLIENT_PORT}
    server_socket_port: ${PREFILL_SERVER_PORT}
YAML

log_success "Generated configs in ${CFG_DIR}/"

# ─────────────────────────────────────────────────────────────────────────────
# Helper: wait for a pattern to appear in a log file
# ─────────────────────────────────────────────────────────────────────────────
wait_for_log() {
    local logfile="$1"
    local pattern="$2"
    local label="$3"
    local timeout="${4:-${TIMEOUT_SECS}}"
    local elapsed=0

    while [[ $elapsed -lt $timeout ]]; do
        if grep -q "$pattern" "$logfile" 2>/dev/null; then
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done

    log_error "Timed out waiting for '${pattern}' in ${label} log (${timeout}s)"
    log_error "Last 20 lines of ${label} log:"
    tail -20 "$logfile" 2>/dev/null | sed 's/^/  /' || true
    return 1
}

# ─────────────────────────────────────────────────────────────────────────────
# STEP 4 — Start orchestrator
# ─────────────────────────────────────────────────────────────────────────────
log_step "Starting orchestrator"

"${ORCHESTRATOR_BIN}" --config "${CFG_DIR}/orchestrator.yaml" \
    > "${LOG_DIR}/orchestrator.log" 2>&1 &
ORCH_PID=$!

log_info "Orchestrator PID=${ORCH_PID}"
log_info "Waiting for orchestrator to bind TCP ports..."

if ! wait_for_log "${LOG_DIR}/orchestrator.log" "Orchestrator ONLINE" "orchestrator" 30; then
    log_error "Orchestrator did not start within 30 seconds"
    exit 1
fi
log_success "Orchestrator online (client=${ORCH_CLIENT_PORT}, server=${ORCH_SERVER_PORT})"

# ─────────────────────────────────────────────────────────────────────────────
# STEP 5 — Start prefill node
# ─────────────────────────────────────────────────────────────────────────────
log_step "Starting prefill node"

"${PREFILL_BIN}" --config "${CFG_DIR}/prefill.yaml" \
    > "${LOG_DIR}/prefill.log" 2>&1 &
PREFILL_PID=$!

log_info "Prefill node PID=${PREFILL_PID}"
log_info "Waiting for prefill node to initialize RDMA and connect to orchestrator..."

if ! wait_for_log "${LOG_DIR}/prefill.log" "Node ONLINE" "prefill" 60; then
    log_error "Prefill node did not reach ONLINE state"
    exit 1
fi
log_success "Prefill node online"

# ─────────────────────────────────────────────────────────────────────────────
# STEP 6 — Start decode node
# ─────────────────────────────────────────────────────────────────────────────
log_step "Starting decode node"

"${DECODE_BIN}" --config "${CFG_DIR}/decode.yaml" \
    > "${LOG_DIR}/decode.log" 2>&1 &
DECODE_PID=$!

log_info "Decode node PID=${DECODE_PID}"
log_info "Waiting for decode node to initialize RDMA and connect to orchestrator..."

if ! wait_for_log "${LOG_DIR}/decode.log" "Node ONLINE" "decode" 60; then
    log_error "Decode node did not reach ONLINE state"
    exit 1
fi
log_success "Decode node online"

# ─────────────────────────────────────────────────────────────────────────────
# STEP 7 — Wait for cluster to reach RUNNING state
# ─────────────────────────────────────────────────────────────────────────────
log_step "Waiting for cluster to reach RUNNING state"

log_info "Orchestrator broadcasts QP maps → nodes connect QPs → nodes send RDMA_READY"

if ! wait_for_log "${LOG_DIR}/orchestrator.log" \
        "All nodes are ready for RDMA communication" \
        "orchestrator" "${TIMEOUT_SECS}"; then
    log_error "Cluster did not reach RUNNING state within ${TIMEOUT_SECS}s"
    exit 1
fi

log_success "Orchestrator transitioned to RUNNING state"

# Verify both nodes are in RUNNING state too
for role in prefill decode; do
    if grep -q "State:.*RUNNING\|set_state.*RUNNING\|node_ptr_->set_state" \
            "${LOG_DIR}/${role}.log" 2>/dev/null; then
        log_success "${role} node confirmed RUNNING"
    else
        log_warning "${role} node RUNNING state not yet confirmed in log (may still be OK)"
    fi
done

# ─────────────────────────────────────────────────────────────────────────────
# STEP 8 — Print summary and pass
# ─────────────────────────────────────────────────────────────────────────────
log_step "Deployment test PASSED"

echo ""
echo -e "${GREEN}╔══════════════════════════════════════════════════╗${RESET}"
echo -e "${GREEN}║  All 3 components started and cluster is RUNNING ║${RESET}"
echo -e "${GREEN}╚══════════════════════════════════════════════════╝${RESET}"
echo ""
echo "  Orchestrator  PID=${ORCH_PID}    logs: ${LOG_DIR}/orchestrator.log"
echo "  Prefill node  PID=${PREFILL_PID}    logs: ${LOG_DIR}/prefill.log"
echo "  Decode node   PID=${DECODE_PID}    logs: ${LOG_DIR}/decode.log"
echo ""
echo "Ports:"
echo "  Orchestrator client: localhost:${ORCH_CLIENT_PORT}"
echo "  Orchestrator server: localhost:${ORCH_SERVER_PORT}"
echo "  Prefill  server:     localhost:${PREFILL_SERVER_PORT}"
echo "  Decode   server:     localhost:${DECODE_SERVER_PORT}"
echo ""

# Keep processes running for 5 seconds so caller can inspect, then exit (cleanup kills them)
log_info "Processes will be stopped in 5 seconds..."
sleep 5
