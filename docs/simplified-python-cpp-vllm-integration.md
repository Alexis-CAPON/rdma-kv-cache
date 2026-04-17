# Simplified Python-C++ Binding and vLLM Integration (GPUDirect RDMA)

This document describes the **simplified** approach using direct GPU-to-GPU RDMA transfers for KV cache, eliminating complex per-tensor registration.

## Key Simplification

**OLD approach:** Register each vLLM KV cache tensor individually, track scattered memory
**NEW approach:** Register GPU memory once at startup, transfer blocks directly as needed

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                           vLLM Attention Layer                                      │
│  (Python - PyTorch model execution)                                                 │
│                                                                                     │
│  unified_attention_with_output(q, k, v, layer_name="layers.0.self_attn", ...)     │
│         ↓                                                                           │
│  @maybe_transfer_kv_layer decorator                                                │
└─────────────────────────────────────────────────────────────────────────────────────┘
         ↓
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                         RDMAConnector (Python)                                      │
│  python/rdma_connector/connector.py                                                 │
│                                                                                     │
│  PREFILL: Extract used blocks → Send via RDMA                                      │
│  DECODE:  Receive blocks via RDMA → Write to vLLM's kv_cache                       │
│         ↓                                                                           │
│  self.cpp_engine.transfer_blocks_async(request_id, layer_name, blocks, gpu_ptr)   │
│  self.cpp_engine.receive_blocks(request_id, layer_name, target_gpu_ptr)           │
└─────────────────────────────────────────────────────────────────────────────────────┘
         ↓
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                     PyBind11 Bindings Layer                                         │
│  cpp/bindings/rdma_kv_bindings.cpp                                                  │
│                                                                                     │
│  - Simple block transfer API                                                       │
│  - GPU memory pointer + size                                                       │
│  - GIL release for blocking operations                                             │
└─────────────────────────────────────────────────────────────────────────────────────┘
         ↓
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                    C++ RDMA Transfer Engine                                         │
│  cpp/rdma/gpu_transfer.h/.cpp                                                       │
│                                                                                     │
│  - GPU memory registered ONCE at startup (ibv_reg_mr)                              │
│  - Direct RDMA READ/WRITE from GPU pointer + offset                                │
│  - Block-level granularity transfers                                               │
└─────────────────────────────────────────────────────────────────────────────────────┘
         ↓
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                    GPUDirect RDMA (InfiniBand/RoCE)                                 │
│  GPU0 Memory ←──────────────→ GPU1 Memory                                          │
│  (Prefill)                        (Decode)                                          │
└─────────────────────────────────────────────────────────────────────────────────────┘
```

## 1. Startup: GPU Memory Registration

### 1.1 One-Time RDMA Setup

**File:** `cpp/rdma/gpu_rdma_manager.cpp`

```cpp
class GPURdmaManager {
public:
    void initialize(int gpu_device_id) {
        // 1. Get CUDA device properties
        cudaSetDevice(gpu_device_id);

        // 2. Open RDMA device (InfiniBand/RoCE)
        device_list_ = ibv_get_device_list(&num_devices);
        context_ = ibv_open_device(device_list_[0]);

        // 3. Allocate protection domain
        pd_ = ibv_alloc_pd(context_);

        // 4. Register GPU memory region for RDMA
        // This is THE critical step - enables GPUDirect
        size_t gpu_mem_size = get_available_gpu_memory();
        void* gpu_base_ptr = 0;  // Will register entire GPU address space

        mr_ = ibv_reg_mr(
            pd_,
            gpu_base_ptr,
            gpu_mem_size,
            IBV_ACCESS_LOCAL_WRITE |
            IBV_ACCESS_REMOTE_READ |
            IBV_ACCESS_REMOTE_WRITE
        );

        // Now ANY GPU memory can be RDMA'd without further registration!
        LOG(INFO) << "Registered " << gpu_mem_size << " bytes of GPU memory for RDMA";
    }

    // Simple transfer API
    void transfer_gpu_memory(
        void* local_gpu_ptr,     // Source GPU pointer
        void* remote_gpu_ptr,    // Destination GPU pointer (on remote GPU)
        size_t size_bytes,       // Transfer size
        uint32_t remote_rkey     // Remote memory key
    ) {
        // Post RDMA WRITE directly from GPU memory
        ibv_sge sge = {
            .addr = (uintptr_t)local_gpu_ptr,
            .length = size_bytes,
            .lkey = mr_->lkey
        };

        ibv_send_wr wr = {
            .opcode = IBV_WR_RDMA_WRITE,
            .send_flags = IBV_SEND_SIGNALED,
            .wr.rdma = {
                .remote_addr = (uintptr_t)remote_gpu_ptr,
                .rkey = remote_rkey
            }
        };

        ibv_post_send(qp_, &wr, &bad_wr);
    }
};
```

**Key Point:** After `ibv_reg_mr()` at startup, you can RDMA transfer ANY GPU memory address!

### 1.2 Python Initialization

```python
class RDMAConnector:
    def __init__(self, vllm_config: VllmConfig, role: KVConnectorRole):
        # Initialize C++ RDMA engine with GPU device
        self.cpp_engine = rdma_kv_bindings.GPURdmaManager()
        self.cpp_engine.initialize(gpu_device_id=0)

        # No per-tensor registration needed!
        self.kv_caches = {}  # Will store references to vLLM's tensors

    def register_kv_caches(self, kv_caches: dict[str, torch.Tensor]):
        """
        Simple reference storage - NO C++ registration!
        """
        self.kv_caches = kv_caches
        LOG(INFO) << f"Stored references to {len(kv_caches)} KV cache layers"
```

## 2. Prefill Flow: Extract and Transfer Blocks

### 2.1 After Attention Computation

```python
def save_kv_layer(
    self,
    layer_name: str,
    kv_cache: torch.Tensor,  # Full tensor: (2, num_blocks, block_size, num_heads, head_dim)
    attn_metadata: AttentionMetadata,
    **kwargs
) -> None:
    """
    Extract used blocks and send via RDMA.
    """
    if not self.is_producer:
        return  # Decode doesn't save

    # Get metadata
    metadata = self._get_connector_metadata()
    if not metadata or not metadata.requests:
        return

    for req in metadata.requests:
        request_id = req['request_id']
        block_ids = req['block_ids']  # e.g., [0, 1] for 2 blocks

        # ═══════════════════════════════════════════════════════════
        # STEP 1: Extract used blocks (contiguous in memory)
        # ═══════════════════════════════════════════════════════════

        # Get block range from kv_cache tensor
        # Shape: kv_cache[2, num_blocks, block_size, num_heads, head_dim]
        #        kv_cache[0] = key cache, kv_cache[1] = value cache

        k_blocks = kv_cache[0, block_ids]  # Shape: [num_blocks, block_size, num_heads, head_dim]
        v_blocks = kv_cache[1, block_ids]

        # Stack into single tensor for transfer
        blocks_to_transfer = torch.stack([k_blocks, v_blocks])
        # Shape: [2, num_blocks, block_size, num_heads, head_dim]

        # ═══════════════════════════════════════════════════════════
        # STEP 2: Get GPU pointer and size
        # ═══════════════════════════════════════════════════════════

        gpu_ptr = blocks_to_transfer.data_ptr()  # CUDA pointer
        size_bytes = blocks_to_transfer.numel() * blocks_to_transfer.element_size()

        # ═══════════════════════════════════════════════════════════
        # STEP 3: Initiate direct GPU-to-GPU RDMA transfer
        # ═══════════════════════════════════════════════════════════

        self.cpp_engine.transfer_blocks_async(
            request_id=request_id,
            layer_name=layer_name,
            source_gpu_ptr=gpu_ptr,
            size_bytes=size_bytes,
            num_blocks=len(block_ids)
        )

        # Transfer happens asynchronously in background
        # No CPU copy involved!
```

### 2.2 Wait for Completion

```python
def wait_for_save(self) -> None:
    """
    Ensure all RDMA transfers complete before GPU memory can be reused.
    """
    if not self.is_producer:
        return

    # Block until all RDMA WRITEs complete
    self.cpp_engine.wait_for_all_transfers()
```

## 3. Decode Flow: Receive and Write Blocks

### 3.1 Before Attention Computation

```python
def wait_for_layer_load(self, layer_name: str) -> None:
    """
    Receive blocks via RDMA and write into vLLM's kv_cache tensor.
    """
    if self.is_producer:
        return  # Prefill doesn't load

    # Get metadata
    metadata = self._get_connector_metadata()
    if not metadata or not metadata.requests:
        return

    for req in metadata.requests:
        request_id = req['request_id']
        target_block_ids = req['block_ids']  # Where to write in local kv_cache

        # Get vLLM's kv_cache tensor for this layer
        kv_cache = self.kv_caches[layer_name]
        # Shape: [2, num_blocks, block_size, num_heads, head_dim]

        # ═══════════════════════════════════════════════════════════
        # STEP 1: Prepare destination memory
        # ═══════════════════════════════════════════════════════════

        # Extract the slice where we'll write received blocks
        target_slice = kv_cache[:, target_block_ids]
        # Shape: [2, num_target_blocks, block_size, num_heads, head_dim]

        dest_gpu_ptr = target_slice.data_ptr()
        expected_size = target_slice.numel() * target_slice.element_size()

        # ═══════════════════════════════════════════════════════════
        # STEP 2: Receive via RDMA directly into vLLM's kv_cache
        # ═══════════════════════════════════════════════════════════

        self.cpp_engine.receive_blocks(
            request_id=request_id,
            layer_name=layer_name,
            dest_gpu_ptr=dest_gpu_ptr,
            size_bytes=expected_size
        )

        # When this returns, kv_cache[:, target_block_ids] contains
        # the K/V values from prefill GPU!
        # No CPU copy, no intermediate buffers!
```

## 4. C++ RDMA Transfer Implementation

### 4.1 PyBind11 Bindings

**File:** `cpp/bindings/rdma_kv_bindings.cpp`

```cpp
PYBIND11_MODULE(rdma_kv_bindings, m) {
    py::class_<GPURdmaManager>(m, "GPURdmaManager")
        .def(py::init<>())

        // One-time initialization
        .def("initialize",
             &GPURdmaManager::initialize,
             py::arg("gpu_device_id"),
             "Initialize GPU RDMA (registers GPU memory)")

        // Prefill: Send blocks
        .def("transfer_blocks_async",
             &GPURdmaManager::transfer_blocks_async,
             py::arg("request_id"),
             py::arg("layer_name"),
             py::arg("source_gpu_ptr"),
             py::arg("size_bytes"),
             py::arg("num_blocks"),
             py::call_guard<py::gil_scoped_release>(),
             "Async GPU-to-GPU RDMA transfer")

        // Prefill: Wait for completion
        .def("wait_for_all_transfers",
             &GPURdmaManager::wait_for_all_transfers,
             py::call_guard<py::gil_scoped_release>(),
             "Wait for all pending RDMA transfers")

        // Decode: Receive blocks
        .def("receive_blocks",
             &GPURdmaManager::receive_blocks,
             py::arg("request_id"),
             py::arg("layer_name"),
             py::arg("dest_gpu_ptr"),
             py::arg("size_bytes"),
             py::call_guard<py::gil_scoped_release>(),
             "Receive blocks via RDMA into GPU memory")

        // Statistics
        .def("get_stats", &GPURdmaManager::get_stats);
}
```

### 4.2 Transfer Implementation

```cpp
void GPURdmaManager::transfer_blocks_async(
    const std::string& request_id,
    const std::string& layer_name,
    uintptr_t source_gpu_ptr,
    size_t size_bytes,
    int num_blocks
) {
    // Get remote destination address
    // (orchestrator coordinates where each request's KV cache goes)
    RemoteMemoryInfo remote = get_remote_memory_for_request(request_id, layer_name);

    // Post RDMA WRITE work request
    ibv_sge sge = {
        .addr = source_gpu_ptr,        // Source: GPU memory (already registered)
        .length = size_bytes,
        .lkey = mr_->lkey              // Local memory key from registration
    };

    ibv_send_wr wr = {
        .wr_id = generate_wr_id(request_id, layer_name),
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma = {
            .remote_addr = remote.gpu_ptr,
            .rkey = remote.rkey
        }
    };

    ibv_post_send(qp_, &wr, &bad_wr);

    // Track pending transfer
    pending_transfers_[wr.wr_id] = TransferInfo{
        .request_id = request_id,
        .layer_name = layer_name,
        .size_bytes = size_bytes,
        .start_time = now()
    };
}

void GPURdmaManager::receive_blocks(
    const std::string& request_id,
    const std::string& layer_name,
    uintptr_t dest_gpu_ptr,
    size_t size_bytes
) {
    // Post RDMA READ work request
    // (or wait for incoming RDMA WRITE from prefill)

    auto key = make_pair(request_id, layer_name);

    // Check if already received
    if (completed_receives_.count(key)) {
        return;
    }

    // Wait for completion
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] {
        return completed_receives_.count(key) > 0;
    });
}
```

## 5. Sequence Diagram: Prefill Instance

```
╔═══════════════════════════════════════════════════════════════════════════════════╗
║           PREFILL INSTANCE - Simplified GPU-to-GPU RDMA Transfer                  ║
╚═══════════════════════════════════════════════════════════════════════════════════╝

┌─────────────────────────────────────────────────────────────────────────────────┐
│ PHASE 1: INITIALIZATION (once at startup)                                       │
└─────────────────────────────────────────────────────────────────────────────────┘

1. Initialize RDMA:
   cpp_engine.initialize(gpu_device_id=0)
   └─ ibv_open_device()
   └─ ibv_alloc_pd()
   └─ ibv_reg_mr(entire_gpu_memory)  ← ONE-TIME REGISTRATION
      └─ Now ANY GPU pointer can be RDMA'd!

2. vLLM allocates KV cache tensors:
   kv_caches = {
       "layers.0.self_attn": torch.zeros([2, 1024, 16, 32, 128], device='cuda:0'),
       ...
   }

3. Store references (NO C++ registration):
   connector.register_kv_caches(kv_caches)
   └─ self.kv_caches = kv_caches  # Just store Python references

┌─────────────────────────────────────────────────────────────────────────────────┐
│ PHASE 2: FORWARD PASS - Per Layer                                               │
└─────────────────────────────────────────────────────────────────────────────────┘

FOR EACH LAYER (e.g., "layers.0.self_attn"):

4. Attention computation:
   unified_attention_with_output(...)
   └─ Writes K/V to kv_cache[blocks]

5. Extract used blocks:
   block_ids = [0, 1]  # For this request
   blocks = kv_cache[:, block_ids]
   └─ Shape: [2, 2, 16, 32, 128]  # 2 blocks for K and V

6. Get GPU pointer:
   gpu_ptr = blocks.data_ptr()      # e.g., 0x7f8b40000000
   size = blocks.numel() * 2        # e.g., 524,288 bytes

7. Initiate RDMA transfer:
   cpp_engine.transfer_blocks_async(
       request_id="req_12345",
       layer_name="layers.0.self_attn",
       source_gpu_ptr=gpu_ptr,
       size_bytes=524288,
       num_blocks=2
   )

   C++ Implementation:
   └─ ibv_sge sge = {
        .addr = 0x7f8b40000000,      ← Direct GPU address!
        .length = 524288,
        .lkey = mr_->lkey             ← From startup registration
      }
   └─ ibv_send_wr wr = {
        .opcode = IBV_WR_RDMA_WRITE,
        .wr.rdma = {
            .remote_addr = decode_gpu_addr,  ← Decode GPU address
            .rkey = remote_rkey
        }
      }
   └─ ibv_post_send(qp, &wr)
      └─ GPU Memory ────RDMA───→ Remote GPU Memory
         (NO CPU INVOLVEMENT!)

8. Continue to next layer (transfer happens in background)

┌─────────────────────────────────────────────────────────────────────────────────┐
│ PHASE 3: SYNCHRONIZATION (after all layers)                                     │
└─────────────────────────────────────────────────────────────────────────────────┘

9. Wait for all RDMA transfers:
   cpp_engine.wait_for_all_transfers()
   └─ Poll completion queue until all work requests complete
   └─ All 32 layers * 2 blocks = 64 RDMA operations complete

═══════════════════════════════════════════════════════════════════════════════════

MEMORY VIEW:

Prefill GPU 0:                        Decode GPU 1:
┌──────────────────────┐             ┌──────────────────────┐
│ kv_cache tensor      │             │ kv_cache tensor      │
│ [2, 1024, 16, 32, 128]             │ [2, 1024, 16, 32, 128]│
│                      │             │                      │
│ Block 0: [....]      │──RDMA────→│ Block 10: [....]     │
│ Block 1: [....]      │──RDMA────→│ Block 11: [....]     │
│ Block 2: (unused)    │             │ Block 12: (unused)   │
│ ...                  │             │ ...                  │
└──────────────────────┘             └──────────────────────┘
      ↑                                      ↑
      └──── Direct GPU pointer ─────────────┘
            (no CPU copy!)
```

## 6. Sequence Diagram: Decode Instance

```
╔═══════════════════════════════════════════════════════════════════════════════════╗
║            DECODE INSTANCE - Simplified GPU-to-GPU RDMA Receive                   ║
╚═══════════════════════════════════════════════════════════════════════════════════╝

┌─────────────────────────────────────────────────────────────────────────────────┐
│ PHASE 1: INITIALIZATION (once at startup)                                       │
└─────────────────────────────────────────────────────────────────────────────────┘

1. Initialize RDMA:
   cpp_engine.initialize(gpu_device_id=0)
   └─ ibv_reg_mr(entire_gpu_memory)  ← ONE-TIME REGISTRATION

2. vLLM allocates KV cache tensors (same as prefill)

3. Store references:
   connector.register_kv_caches(kv_caches)

┌─────────────────────────────────────────────────────────────────────────────────┐
│ PHASE 2: FORWARD PASS - Per Layer                                               │
└─────────────────────────────────────────────────────────────────────────────────┘

FOR EACH LAYER (e.g., "layers.0.self_attn"):

4. Wait for layer load:
   connector.wait_for_layer_load("layers.0.self_attn")

   4.1. Get target blocks:
        target_block_ids = [10, 11]  # Allocated by vLLM scheduler

   4.2. Get destination GPU pointer:
        kv_cache = self.kv_caches["layers.0.self_attn"]
        target_slice = kv_cache[:, target_block_ids]
        dest_gpu_ptr = target_slice.data_ptr()  # e.g., 0x7f9c50005000
        size = target_slice.numel() * 2

   4.3. Receive blocks:
        cpp_engine.receive_blocks(
            request_id="req_12345",
            layer_name="layers.0.self_attn",
            dest_gpu_ptr=dest_gpu_ptr,
            size_bytes=524288
        )

        C++ Implementation:
        └─ Wait for RDMA WRITE completion from prefill
        └─ Or post RDMA READ if needed
        └─ When complete, data is IN kv_cache[:, [10, 11]]
           ↑
           └─── Direct GPU write! No CPU copy!

5. Attention computation:
   unified_attention_with_output(...)
   └─ kv_cache[:, [10, 11]] contains prefill K/V values
   └─ Computes attention with loaded cache + new token

6. Continue to next layer

┌─────────────────────────────────────────────────────────────────────────────────┐
│ PHASE 3: GENERATE NEXT TOKEN                                                    │
└─────────────────────────────────────────────────────────────────────────────────┘

7. All layers processed with loaded KV cache
8. Generate next token
9. Return to scheduler

═══════════════════════════════════════════════════════════════════════════════════

MEMORY VIEW:

Prefill GPU 0:                        Decode GPU 1:
┌──────────────────────┐             ┌──────────────────────┐
│ Block 0: [K/V data]  │──RDMA────→│ Block 10: [K/V data] │
│ Block 1: [K/V data]  │──RDMA────→│ Block 11: [K/V data] │
└──────────────────────┘             └──────────────────────┘
                                              ↓
                                     vLLM attention reads from blocks 10-11
                                     (thinks it's local data - it is!)
```

## 7. Key Simplifications

### What Changed from Complex Version

| Aspect | OLD (Complex) | NEW (Simplified) |
|--------|---------------|------------------|
| **GPU Registration** | Per-tensor, per-layer | Once at startup for entire GPU |
| **Python API** | `register_kv_layer(layer, ptr, size, shape)` | `initialize(gpu_id)` |
| **Memory Tracking** | C++ tracks all tensor pointers | Python gets pointer on-demand |
| **Transfer Granularity** | Pre-registered memory regions | Any GPU pointer + size |
| **Block Extraction** | C++ manages scattered blocks | Python extracts with simple slice |

### Benefits

1. **Simpler Code**: No per-tensor registration logic
2. **More Flexible**: Can transfer any GPU memory, not just pre-registered
3. **Easier to Debug**: Direct pointer passing, no hidden state
4. **Less Coordination**: C++ doesn't need to know about vLLM's memory layout
5. **Same Performance**: GPUDirect RDMA works the same either way

## 8. What's Still "Tricky"?

You asked if registration is the tricky part. Here's what's actually tricky:

### Easy Parts ✅
- **GPU Memory Registration**: `ibv_reg_mr()` - standard RDMA, done once
- **Queue Pair Setup**: Standard RDMA connection establishment
- **Pointer Passing**: `tensor.data_ptr()` - PyTorch gives you the GPU pointer
- **Transfer Initiation**: `ibv_post_send()` - standard RDMA operation

### Potentially Tricky Parts ⚠️

1. **Orchestrator Coordination**:
   - Decode needs to know WHERE to read from on prefill GPU
   - Need to exchange: remote_gpu_addr + rkey for each request
   - Solution: Orchestrator tracks memory mappings

2. **Block Allocation Coordination**:
   - Prefill blocks [0, 1] → need to map to decode blocks [10, 11]
   - vLLM's scheduler allocates blocks independently
   - Solution: Metadata in `RDMAConnectorMetadata` tracks mapping

3. **Memory Synchronization**:
   - Ensure prefill finishes writing before decode reads
   - Solution: RDMA completion notifications

4. **Error Handling**:
   - What if RDMA transfer fails?
   - What if prefill crashes mid-transfer?
   - Solution: Timeout mechanisms and retries

### NOT Tricky ✅
- The RDMA GPU registration itself (standard process)
- Extracting blocks from tensors (simple PyTorch slice)
- Getting GPU pointers (PyTorch provides it)

## 9. Complete Example: Transfer 2 Blocks

```python
# PREFILL SIDE
kv_cache = torch.zeros([2, 1024, 16, 32, 128], device='cuda:0', dtype=torch.float16)
# After attention writes to blocks [0, 1]

blocks = kv_cache[:, [0, 1]]  # Extract: [2, 2, 16, 32, 128]
gpu_ptr = blocks.data_ptr()    # Get CUDA pointer
size = 2 * 2 * 16 * 32 * 128 * 2 = 524,288 bytes

cpp_engine.transfer_blocks_async(
    request_id="req_001",
    layer_name="layers.0.self_attn",
    source_gpu_ptr=gpu_ptr,
    size_bytes=524288,
    num_blocks=2
)

# Transfer happens:
# GPU0[0x7f8b40000000:0x7f8b40080000] ──RDMA──→ GPU1[remote_addr]

# DECODE SIDE
kv_cache = torch.zeros([2, 1024, 16, 32, 128], device='cuda:0', dtype=torch.float16)
# Scheduler allocated blocks [10, 11] for this request

target = kv_cache[:, [10, 11]]  # Slice where data will be written
dest_ptr = target.data_ptr()     # Get CUDA pointer

cpp_engine.receive_blocks(
    request_id="req_001",
    layer_name="layers.0.self_attn",
    dest_gpu_ptr=dest_ptr,
    size_bytes=524288
)

# After this returns:
# kv_cache[:, [10, 11]] contains the K/V data from prefill!
# vLLM's attention can now use it normally
```

## 10. Summary

**Your intuition is correct!** The registration part is straightforward:
- Register GPU memory ONCE at startup with `ibv_reg_mr()`
- After that, you can RDMA transfer ANY GPU pointer

The "complex" parts are:
- Coordinating which blocks go where (orchestrator's job)
- Handling failures and timeouts
- Integrating with vLLM's scheduler

But the actual RDMA transfer is simple: **pointer + size → post send → done!**

This simplified approach eliminates the complex per-tensor registration while maintaining the same performance benefits of direct GPU-to-GPU transfer.
