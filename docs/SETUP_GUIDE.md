# Complete Setup Guide - Clemson CloudLab Cluster

This guide walks through setting up the GPUDirect RDMA disaggregated LLM inference system on your 5 Clemson r7525 nodes.

## Your Cluster

- **Node 0**: clgpu014.clemson.cloudlab.us (Orchestrator)
- **Node 1**: clgpu012.clemson.cloudlab.us (Prefill 1)
- **Node 2**: clgpu015.clemson.cloudlab.us (Prefill 2)
- **Node 3**: clgpu013.clemson.cloudlab.us (Decode 1)
- **Node 4**: clgpu021.clemson.cloudlab.us (Decode 2)

**Hardware**: r7525 (AMD EPYC, NVIDIA A100 GPUs, Mellanox ConnectX-6 InfiniBand)

---

## Phase 1: Install Dependencies (One-Time Setup)

### Step 1: Install on All Nodes

This installs CUDA, RDMA/InfiniBand libraries, GPUDirect kernel module, Python, vLLM, and all dependencies.

```bash
cd ~/rdma-kv-cache

# Install on all 5 nodes (takes 15-30 minutes)
./scripts/install_all_nodes.sh
```

**What it does**:
- Installs NVIDIA drivers and CUDA 12.6
- Installs InfiniBand/RDMA libraries (libibverbs, rdmacm)
- Builds and loads nvidia-peermem kernel module (GPUDirect)
- Installs PyTorch with CUDA support
- Installs vLLM and dependencies
- Configures system (locked memory, huge pages, PCIe relaxed ordering)

**Expected output**:
```
[INFO] Installing Dependencies on All Nodes
...
Progress: 5 / 5 nodes completed
[SUCCESS] All installations completed successfully!
```

### Step 2: Reboot All Nodes

After CUDA installation, a reboot is required:

```bash
./scripts/reboot_all_nodes.sh
```

Wait 3-5 minutes for nodes to reboot.

### Step 3: Verify Installation

Check that GPUDirect RDMA is working on all nodes:

```bash
./scripts/verify_gpudirect.sh
```

**Expected output**:
```
Node: clgpu014.clemson.cloudlab.us
  NVIDIA driver: [OK]
  InfiniBand devices: 1 found
  nvidia-peermem module: [LOADED]
  PyTorch CUDA: [OK]
  GPU Info:
    0, NVIDIA A100-PCIE-40GB, 40960 MiB
```

**If verification fails**:
```bash
# SSH to failing node
ssh Alexis@clgpu014.clemson.cloudlab.us

# Check NVIDIA driver
nvidia-smi

# Check InfiniBand
ibv_devices
ibstat

# Load GPUDirect module if not loaded
sudo modprobe nvidia-peermem
lsmod | grep nvidia_peermem

# Test GPUDirect bandwidth
ib_write_bw -d mlx5_0 -a --use_cuda=0
```

---

## Phase 2: Deploy and Build

### Step 4: Deploy Code to All Nodes

```bash
./scripts/smart_deploy.sh 5
```

**What it does**:
1. Builds C++ code locally
2. Syncs code to all 5 nodes
3. Builds C++ on each node
4. Installs Python RDMA bindings
5. Copies RDMAConnector to vLLM

**Expected output**:
```
[STEP] Syncing code to remote nodes...
[SUCCESS] Code synced to all nodes
[STEP] Building on remote nodes...
[SUCCESS] Build completed on clgpu014.clemson.cloudlab.us
...
[SUCCESS] Deployment completed successfully!
```

---

## Phase 3: Run the System

### Step 5: Start All Components

```bash
# Start with 2 prefill nodes and 2 decode nodes
./scripts/smart_run.sh 2 2
```

**What it does**:
1. Starts orchestrator on clgpu014
2. Starts vLLM + C++ node on prefill nodes (clgpu012, clgpu015)
3. Starts vLLM + C++ node on decode nodes (clgpu013, clgpu021)

**Expected output**:
```
[INFO] Starting Disaggregated LLM Inference System
[INFO] Orchestrator: clgpu014.clemson.cloudlab.us
[INFO] Prefill nodes: clgpu012.clemson.cloudlab.us clgpu015.clemson.cloudlab.us
[INFO] Decode nodes: clgpu013.clemson.cloudlab.us clgpu021.clemson.cloudlab.us

[STEP] Starting orchestrator...
[SUCCESS] Orchestrator started on clgpu014.clemson.cloudlab.us

[STEP] Starting prefill nodes...
[SUCCESS] prefill-00 started (vLLM + C++ node)
[SUCCESS] prefill-01 started (vLLM + C++ node)

[STEP] Starting decode nodes...
[SUCCESS] decode-00 started (vLLM + C++ node)
[SUCCESS] decode-01 started (vLLM + C++ node)

[SUCCESS] All components started successfully!
```

### Step 6: Test the System

```bash
./scripts/test_request.sh "Once upon a time"
```

**Expected output**:
```
[INFO] Testing Disaggregated LLM Inference
[STEP] Sending request to orchestrator...

[SUCCESS] Received response!

==========================================
Generated Text:
==========================================

Once upon a time, in a land far away, there lived a young princess...

[SUCCESS] Test completed successfully!
```

---

## Monitoring and Debugging

### View Logs

```bash
# Orchestrator log
ssh Alexis@clgpu014.clemson.cloudlab.us 'tail -f /users/Alexis/rdma-kv-cache/logs/orchestrator.log'

# Prefill node 0 logs
ssh Alexis@clgpu012.clemson.cloudlab.us 'tail -f /users/Alexis/rdma-kv-cache/logs/prefill-00-vllm.log'
ssh Alexis@clgpu012.clemson.cloudlab.us 'tail -f /users/Alexis/rdma-kv-cache/logs/prefill-00-node.log'

# Decode node 0 logs
ssh Alexis@clgpu013.clemson.cloudlab.us 'tail -f /users/Alexis/rdma-kv-cache/logs/decode-00-vllm.log'
ssh Alexis@clgpu013.clemson.cloudlab.us 'tail -f /users/Alexis/rdma-kv-cache/logs/decode-00-node.log'
```

### Check Process Status

```bash
# On any node
ssh Alexis@clgpu014.clemson.cloudlab.us

# Check processes
pgrep -a orchestrator
pgrep -a prefill_node
pgrep -a decode_node
pgrep -a python  # vLLM

# Check GPU usage
nvidia-smi

# Check RDMA activity
ibstatus
```

### Stop the System

```bash
./scripts/smart_stop.sh
```

---

## Troubleshooting

### Issue: vLLM fails to start

**Symptoms**: "Module not found: rdma_bindings"

**Fix**:
```bash
# On each node
ssh Alexis@<node>
cd ~/rdma-kv-cache
export PYTHONPATH=~/rdma-kv-cache/install/python:$PYTHONPATH
python3 -c "import rdma_bindings; print('OK')"
```

### Issue: RDMA transfer fails

**Symptoms**: "ibv_reg_mr failed: Cannot allocate memory"

**Fix**:
```bash
# Check locked memory limit
ulimit -l  # Should show "unlimited"

# If not unlimited, logout and login again
# Or check /etc/security/limits.conf
```

### Issue: GPUDirect not working

**Symptoms**: Low RDMA bandwidth, CPU usage high

**Check**:
```bash
# Verify module loaded
lsmod | grep nvidia_peermem

# Test GPUDirect bandwidth
ib_write_bw -d mlx5_0 -a --use_cuda=0

# Should see >90 GB/s with GPUDirect
# Without GPUDirect: ~10-20 GB/s
```

### Issue: Orchestrator can't reach nodes

**Symptoms**: "Failed to connect to orchestrator"

**Check**:
```bash
# On orchestrator
netstat -tulpn | grep 9000

# Test connectivity
ping clgpu012.clemson.cloudlab.us

# Check firewall (should be open in CloudLab)
sudo iptables -L
```

---

## Performance Tuning

### RDMA Configuration

Edit `configs/decode_node.yaml`:

```yaml
memory:
  kv_buffer_mb: 16384      # Increase for larger KV caches
  chunk_size_mb: 128       # 64-128 MB optimal

node:
  qp_max_send_wr: 128      # Increase for higher throughput
  max_rd_atomic: 16        # Increase for parallelism
```

### vLLM Configuration

Edit `scripts/deploy_config.sh`:

```bash
MODEL_NAME="meta-llama/Llama-2-7b-hf"  # Change model
GPU_MEMORY_UTILIZATION=0.9             # Adjust memory
MAX_MODEL_LEN=4096                     # Token limit
```

### Network Optimization

```bash
# On each node
ssh Alexis@<node>

# Enable relaxed ordering
echo 1 | sudo tee /sys/module/mlx5_core/parameters/relaxed_ordering_write

# Check IB link speed
ibstat  # Should show "Rate: 100" for 100 Gbps
```

---

## Directory Structure

```
rdma-kv-cache/
├── scripts/
│   ├── install_all_nodes.sh          # Install dependencies
│   ├── install_dependencies.sh        # Dependency script (runs on each node)
│   ├── reboot_all_nodes.sh           # Reboot cluster
│   ├── verify_gpudirect.sh           # Verify GPUDirect
│   ├── smart_deploy.sh               # Deploy code
│   ├── smart_run.sh                  # Start system
│   ├── smart_stop.sh                 # Stop system
│   └── test_request.sh               # Test end-to-end
├── configs/
│   ├── orchestrator.yaml
│   ├── prefill_node.yaml
│   └── decode_node.yaml
├── cpp/                               # C++ source
├── python/                            # Python RDMAConnector
└── logs/                              # Generated at runtime
```

---

## Quick Reference

```bash
# Full setup (first time)
./scripts/install_all_nodes.sh
./scripts/reboot_all_nodes.sh
# Wait 5 minutes
./scripts/verify_gpudirect.sh
./scripts/smart_deploy.sh 5

# Start system
./scripts/smart_run.sh 2 2

# Test
./scripts/test_request.sh "Hello world"

# Stop
./scripts/smart_stop.sh

# Rebuild after code changes
./scripts/smart_deploy.sh 5
```

---

## Next Steps

1. **Run benchmarks**: Measure end-to-end latency and throughput
2. **Profile RDMA**: Use `perftest` tools to measure GPU-to-GPU bandwidth
3. **Scale up**: Try with different numbers of prefill/decode nodes
4. **Monitor**: Set up logging aggregation and metrics

---

## Support

- **Verification Report**: See `VERIFICATION_REPORT.md` for implementation status
- **Deployment Guide**: See `DEPLOYMENT_GUIDE.md` for detailed API usage
- **CloudLab**: https://www.cloudlab.us/
- **vLLM Docs**: https://docs.vllm.ai/
