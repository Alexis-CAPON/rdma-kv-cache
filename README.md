# Disaggregated LLM Inference with GPUDirect RDMA

High-performance LLM inference system that separates prefill and decode phases across multiple GPUs, using RDMA for ultra-low-latency KV cache transfer.

## Quick Start

```bash
# 1. Install dependencies on all nodes (one-time)
./scripts/install_all_nodes.sh
./scripts/reboot_all_nodes.sh
./scripts/verify_gpudirect.sh

# 2. Deploy and build
./scripts/smart_deploy.sh 5

# 3. Start system (2 prefill, 2 decode)
./scripts/smart_run.sh 2 2

# 4. Test with benchmark client
./build/bin/benchmark_client \
    --host clgpu014.clemson.cloudlab.us \
    --prompt "Once upon a time"

# 5. Stop
./scripts/smart_stop.sh
```

## Documentation

| Document                                             | Description                                 |
| ---------------------------------------------------- | ------------------------------------------- |
| **[SETUP_GUIDE.md](SETUP_GUIDE.md)**                 | Complete setup for Clemson CloudLab cluster |
| **[DEPLOYMENT_GUIDE.md](DEPLOYMENT_GUIDE.md)**       | Detailed deployment and API usage           |
| **[BENCHMARK_GUIDE.md](BENCHMARK_GUIDE.md)**         | Benchmarking and performance testing        |
| **[VERIFICATION_REPORT.md](VERIFICATION_REPORT.md)** | End-to-end implementation verification      |

## Architecture

```
Benchmark Client (TCP) → Orchestrator → Prefill Node (vLLM)
                                             ↓ RDMA (GPU→GPU)
                                        Decode Node (vLLM) → Response
```

**Key Components**:

- **Orchestrator**: Routes requests, manages node registry
- **Prefill Nodes**: Process prompts, generate KV cache (vLLM with RDMA send)
- **Decode Nodes**: Generate tokens from transferred KV cache (vLLM with RDMA recv)
- **GPUDirect RDMA**: Direct GPU-to-GPU memory transfer (bypasses CPU)
- **Benchmark Client**: C++ client that establishes persistent TCP connection to orchestrator

## Features

- ✅ Zero-copy RDMA on prefill side (register vLLM memory)
- ✅ GPUDirect RDMA (nvidia-peermem kernel module)
- ✅ vLLM integration with custom RDMAConnector
- ✅ HTTP client for C++ ↔ vLLM communication
- ✅ Persistent TCP connection benchmark client
- ✅ Comprehensive deployment scripts
- ✅ Automated dependency installation

## Project Structure

```
rdma-kv-cache/
├── cpp/                           # C++ implementation
│   ├── apps/
│   │   ├── orchestrator/          # Orchestrator implementation
│   │   ├── benchmark_client.cpp   # Benchmark client (NEW)
│   │   ├── prefill_node_main.cpp
│   │   └── decode_node_main.cpp
│   ├── rdma/                      # RDMA engine (GPUDirect)
│   ├── nodes/                     # Worker pool, event loop
│   ├── network/                   # TCP, HTTP client
│   ├── common/                    # Messages, config, types
│   └── bindings/                  # Python bindings (pybind11)
├── python/
│   └── rdma_connector.py          # vLLM KV connector
├── scripts/
│   ├── install_all_nodes.sh       # Install dependencies on all nodes
│   ├── install_dependencies.sh    # Dependency script (GPUDirect RDMA)
│   ├── verify_gpudirect.sh        # Verify GPUDirect setup
│   ├── smart_deploy.sh            # Deploy code
│   ├── smart_run.sh               # Start system
│   ├── smart_stop.sh              # Stop system
│   ├── benchmark.sh               # Single benchmark (NEW)
│   └── benchmark_suite.sh         # Full benchmark suite (NEW)
├── configs/                       # YAML configuration files
└── docs/                          # Additional documentation
```

## Prerequisites

- **Hardware**: NVIDIA GPUs (A100), Mellanox ConnectX InfiniBand
- **OS**: Ubuntu 24.04
- **Software**: CUDA 12.6, Python 3.10+, CMake 3.18+

## Usage Examples

### Benchmark Client (Single Request)

```bash
./build/bin/benchmark_client \
    --host clgpu014.clemson.cloudlab.us \
    --prompt "Tell me a story" \
    --tokens 50
```

### Throughput Benchmark

```bash
./build/bin/benchmark_client \
    --host clgpu014.clemson.cloudlab.us \
    --mode throughput \
    --num 100 \
    --tokens 50
```

### Using Benchmark Scripts

```bash
# Single benchmark
./scripts/benchmark.sh --mode throughput --num 100

# Full benchmark suite (7 tests)
./scripts/benchmark_suite.sh
```

### Monitor Logs

```bash
# Orchestrator
ssh Alexis@clgpu014.clemson.cloudlab.us 'tail -f ~/rdma-kv-cache/logs/orchestrator.log'

# Prefill node
ssh Alexis@clgpu012.clemson.cloudlab.us 'tail -f ~/rdma-kv-cache/logs/prefill-00-vllm.log'

# Decode node
ssh Alexis@clgpu013.clemson.cloudlab.us 'tail -f ~/rdma-kv-cache/logs/decode-00-vllm.log'
```

## Performance

**Expected Metrics** (preliminary):

- **Prefill**: 50-200ms (depends on prompt length)
- **RDMA Transfer**: <10ms (GPUDirect should be 1-5ms)
- **Decode**: 500-2000ms (depends on tokens generated)
- **End-to-End Latency**: 600-2200ms per request

## Configuration

Edit `scripts/deploy_config.sh`:

```bash
MODEL_NAME="meta-llama/Llama-2-7b-hf"
GPU_MEMORY_UTILIZATION=0.9
MAX_MODEL_LEN=4096
```

Edit `configs/decode_node.yaml`:

```yaml
memory:
  kv_buffer_mb: 16384 # GPU buffer size
  chunk_size_mb: 128 # RDMA chunk size

node:
  qp_max_send_wr: 128 # RDMA queue depth
```

## Testing

```bash
# Verify GPUDirect RDMA
./scripts/verify_gpudirect.sh

# Single request test
./build/bin/benchmark_client --host <orch> --prompt "Hello"

# Throughput benchmark
./scripts/benchmark.sh --mode throughput --num 100

# Full benchmark suite
./scripts/benchmark_suite.sh
```

## Research

This project implements disaggregated inference as described in:

- [vLLM Disaggregated Inference](https://docs.vllm.ai/en/latest/features/disagg_prefill.html)
- [GPUDirect RDMA](https://docs.nvidia.com/cuda/gpudirect-rdma/)

## Acknowledgments

- Built on [vLLM](https://github.com/vllm-project/vllm)
- Uses [Mellanox nvidia-peermem](https://github.com/Mellanox/nv_peer_memory) for GPUDirect
- Deployed on [CloudLab](https://www.cloudlab.us/) Clemson cluster

---

**Status**: 🟡 In Development

**Last Updated**: April 20, 2026
