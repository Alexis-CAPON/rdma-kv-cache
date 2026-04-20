# Benchmark Client Guide

This guide explains how to use the benchmark client to test and measure the performance of your disaggregated LLM inference system.

---

## Overview

The benchmark client establishes a **persistent TCP connection** to the orchestrator and sends inference requests. It supports:

1. **Single request mode**: Test latency for individual requests
2. **Throughput mode**: Measure system throughput with multiple requests

**Key Feature**: The client reuses the same TCP connection for all requests, which is realistic for production workloads.

---

## Architecture

```
Benchmark Client (TCP) → Orchestrator → Prefill Node → RDMA → Decode Node → Response
```

The client:
- Establishes TCP connection to orchestrator's client port (default: 9000)
- Sends serialized `CLIENT_REQUEST` messages
- Waits for `CLIENT_RESPONSE` messages
- Measures end-to-end latency (request → response)
- Reuses connection for multiple requests (throughput mode)

---

## Building

The benchmark client is built automatically with the project:

```bash
cd ~/rdma-kv-cache
mkdir -p build && cd build
cmake ..
make benchmark_client
```

Binary location: `build/bin/benchmark_client`

---

## Usage

### Command Line Options

```bash
./build/bin/benchmark_client [options]

Options:
  -h, --host <host>       Orchestrator hostname (default: localhost)
  -p, --port <port>       Orchestrator port (default: 9000)
  -m, --mode <mode>       Benchmark mode: single, throughput (default: single)
  -n, --num <num>         Number of requests for throughput mode (default: 100)
  -t, --tokens <tokens>   Max tokens to generate (default: 50)
  --prompt <prompt>       Prompt text (default: 'Once upon a time')
  --help                  Show help message
```

### Examples

#### 1. Single Request (Latency Test)

```bash
./build/bin/benchmark_client \
    --host clgpu014.clemson.cloudlab.us \
    --prompt "Once upon a time" \
    --tokens 50
```

**Output**:
```
========================================
Request successful!
========================================
Prompt:     Once upon a time
Max tokens: 50
Latency:    1234.56 ms

Generated text:
Once upon a time, in a land far away, there lived a young princess...
========================================
```

#### 2. Throughput Benchmark (100 requests)

```bash
./build/bin/benchmark_client \
    --host clgpu014.clemson.cloudlab.us \
    --mode throughput \
    --num 100 \
    --tokens 50
```

**Output**:
```
Progress: 100/100 requests

========================================
Benchmark Results
========================================
Total requests:      100
Successful:          100
Failed:              0
Average latency:     1234.56 ms
Min latency:         987 ms
Max latency:         2103 ms
Success rate:        100.0 %
Total time:          123.45 seconds
Throughput:          0.81 req/sec
========================================
```

---

## Benchmark Scripts

We provide helper scripts for easier benchmarking:

### 1. Single Benchmark (`scripts/benchmark.sh`)

Convenient wrapper around the benchmark client:

```bash
# Single request
./scripts/benchmark.sh --prompt "Tell me a story"

# Throughput test
./scripts/benchmark.sh \
    --mode throughput \
    --num 500 \
    --tokens 100

# Run from a CloudLab node
./scripts/benchmark.sh \
    --client-node clgpu021.clemson.cloudlab.us \
    --mode throughput \
    --num 100
```

### 2. Benchmark Suite (`scripts/benchmark_suite.sh`)

Runs a comprehensive set of tests:

```bash
./scripts/benchmark_suite.sh
```

**Tests included**:
1. Cold start latency
2. Warm latency
3. Throughput with 10, 50, 100 requests
4. Variable token generation (10, 50, 100, 200 tokens)
5. Different prompt lengths (short, medium, long)

**Output**: Results saved to `benchmark_results/<timestamp>/` with summary report.

---

## Metrics Explained

### Latency Metrics

- **Average latency**: Mean end-to-end time (request sent → response received)
- **Min latency**: Fastest request
- **Max latency**: Slowest request

**Components included in latency**:
1. Network time (client → orchestrator)
2. Orchestrator routing time
3. Prefill time (vLLM processing + RDMA write)
4. RDMA transfer time (GPU → GPU)
5. Decode time (vLLM token generation)
6. Network time (orchestrator → client)

### Throughput Metrics

- **Success rate**: Percentage of requests that completed successfully
- **Total time**: Wall clock time for all requests
- **Throughput**: Requests per second (successful_requests / total_time)

**Note**: Current implementation is **serial** (one request at a time). For concurrent throughput, you can run multiple client instances.

---

## Benchmarking Best Practices

### 1. Warm-Up Phase

Always run a few warm-up requests before measuring:

```bash
# Warm up
./build/bin/benchmark_client \
    --host <orchestrator> \
    --num 5

# Then measure
./build/bin/benchmark_client \
    --host <orchestrator> \
    --mode throughput \
    --num 100
```

### 2. Vary Request Parameters

Test different scenarios:

```bash
# Short prompts, few tokens
./scripts/benchmark.sh --prompt "Hi" --tokens 10 --num 50

# Long prompts, many tokens
./scripts/benchmark.sh \
    --prompt "Write a detailed story about..." \
    --tokens 200 \
    --num 50
```

### 3. Run from Different Locations

Compare latency from different locations:

```bash
# From local machine
./scripts/benchmark.sh --mode throughput --num 100

# From a CloudLab node (lower network latency)
./scripts/benchmark.sh \
    --client-node clgpu021.clemson.cloudlab.us \
    --mode throughput \
    --num 100
```

### 4. Monitor System During Benchmark

While benchmark runs, monitor:

```bash
# On orchestrator
ssh Alexis@clgpu014.clemson.cloudlab.us 'tail -f logs/orchestrator.log'

# On prefill node
ssh Alexis@clgpu012.clemson.cloudlab.us 'nvidia-smi dmon'

# On decode node
ssh Alexis@clgpu013.clemson.cloudlab.us 'nvidia-smi dmon'

# RDMA traffic
ssh Alexis@clgpu012.clemson.cloudlab.us 'ibstat'
```

---

## Interpreting Results

### Expected Performance

**Latency** (single request):
- Prefill time: 50-200ms (depends on prompt length)
- RDMA transfer: <10ms (GPUDirect should be ~1-5ms)
- Decode time: 500-2000ms (depends on tokens generated)
- **Total**: 600-2200ms per request

**Throughput** (serial):
- ~0.5-2 req/sec (depends on token count)
- Limited by serial processing

### Troubleshooting Performance Issues

#### High Latency (>5000ms)

**Check**:
1. RDMA working? (`lsmod | grep nvidia_peermem`)
2. Network latency: `ping <orchestrator>`
3. GPU utilization: `nvidia-smi` (should be >80%)
4. vLLM logs for errors

#### Low Throughput (<0.1 req/sec)

**Check**:
1. Orchestrator logs for routing issues
2. Node failures (prefill/decode crashed?)
3. vLLM startup (may take 1-2 min on first request)

#### Connection Failures

**Check**:
1. Orchestrator running: `ssh <orch> 'pgrep orchestrator'`
2. Port accessible: `telnet <orch> 9000`
3. Firewall: CloudLab should allow all ports

---

## Advanced: Concurrent Clients

To test concurrent load, run multiple clients:

```bash
# Terminal 1
./build/bin/benchmark_client --host <orch> --mode throughput --num 50 &

# Terminal 2
./build/bin/benchmark_client --host <orch> --mode throughput --num 50 &

# Terminal 3
./build/bin/benchmark_client --host <orch> --mode throughput --num 50 &

# Wait for all
wait
```

Or use a script:

```bash
#!/bin/bash
# concurrent_benchmark.sh

NUM_CLIENTS=5
REQUESTS_PER_CLIENT=20

for i in $(seq 1 $NUM_CLIENTS); do
    ./build/bin/benchmark_client \
        --host clgpu014.clemson.cloudlab.us \
        --mode throughput \
        --num $REQUESTS_PER_CLIENT \
        > "client_${i}.log" 2>&1 &
done

wait
echo "All clients finished"
```

---

## Example: Full Benchmark Workflow

```bash
# 1. Start system
./scripts/smart_run.sh 2 2

# Wait for vLLM to initialize (2-3 minutes)
sleep 180

# 2. Warm up
./scripts/benchmark.sh --num 5

# 3. Run comprehensive suite
./scripts/benchmark_suite.sh

# 4. Analyze results
cat benchmark_results/*/SUMMARY.txt

# 5. Custom tests
./scripts/benchmark.sh \
    --mode throughput \
    --num 500 \
    --tokens 100 \
    --prompt "Your custom prompt here"

# 6. Stop system
./scripts/smart_stop.sh
```

---

## Exporting Results

### CSV Format

Extract metrics to CSV:

```bash
# Create CSV header
echo "test,requests,success,failed,avg_latency_ms,min_latency_ms,max_latency_ms,throughput_rps" > results.csv

# Parse log files and append
for log in benchmark_results/*/0*.log; do
    TEST=$(basename $log .log)
    # Extract metrics using grep/awk and append to CSV
    # ...
done
```

### Plotting

Use Python/gnuplot to visualize:

```python
import pandas as pd
import matplotlib.pyplot as plt

df = pd.read_csv('results.csv')
df.plot(x='test', y='avg_latency_ms', kind='bar')
plt.title('Average Latency by Test')
plt.show()
```

---

## Quick Reference

```bash
# Single request
./build/bin/benchmark_client --host <orch> --prompt "Hello"

# Throughput test
./build/bin/benchmark_client --host <orch> --mode throughput --num 100

# With script
./scripts/benchmark.sh --mode throughput --num 100

# Full suite
./scripts/benchmark_suite.sh

# Concurrent (5 clients, 20 req each)
for i in {1..5}; do
    ./build/bin/benchmark_client --host <orch> --mode throughput --num 20 &
done; wait
```

---

## Next Steps

1. **Baseline**: Run benchmark suite to establish baseline performance
2. **Tune**: Adjust vLLM/RDMA settings based on results
3. **Scale**: Add more prefill/decode nodes and re-benchmark
4. **Compare**: Benchmark with/without GPUDirect to measure benefit
5. **Profile**: Use `nvidia-smi` and `ibstat` during benchmarks to identify bottlenecks
