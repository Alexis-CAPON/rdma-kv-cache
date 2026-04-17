# Python-C++ Binding and vLLM Integration

This document describes how the C++ RDMA engine integrates with Python and vLLM during request processing, covering the complete flow from vLLM attention layers through Python bindings to RDMA transfers.

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
│  wait_for_layer_load(layer_name)  →  Load KV cache (decode)                       │
│  save_kv_layer(layer_name, ...)   →  Save KV cache (prefill)                      │
│         ↓                                                                           │
│  self.cpp_engine.wait_for_layer_load(request_id, layer_name)                      │
│  self.cpp_engine.save_kv_layer_async(request_id, layer_name, block_ids)           │
└─────────────────────────────────────────────────────────────────────────────────────┘
         ↓
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                     PyBind11 Bindings Layer                                         │
│  cpp/bindings/rdma_kv_bindings.cpp                                                  │
│                                                                                     │
│  PYBIND11_MODULE(rdma_kv_bindings, m)                                              │
│  - Exposes C++ KVTransferEngine to Python                                          │
│  - Manages GIL release for blocking operations                                     │
│  - Maintains global singleton engine                                               │
└─────────────────────────────────────────────────────────────────────────────────────┘
         ↓
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                    C++ RDMA KVTransferEngine                                        │
│  cpp/kv_transfer/kv_transfer_engine.h/.cpp                                          │
│                                                                                     │
│  - GPU memory registration (CUDA pointers)                                         │
│  - Async RDMA READ/WRITE operations                                                │
│  - Transfer completion tracking                                                    │
│  - Statistics and monitoring                                                       │
└─────────────────────────────────────────────────────────────────────────────────────┘
         ↓
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                         RDMA Network                                                │
│  GPU-to-GPU direct memory transfers                                                │
│  Prefill GPU ←──────────────→ Decode GPU                                           │
└─────────────────────────────────────────────────────────────────────────────────────┘
```

## 1. Component Details

### 1.1 PyBind11 Bindings

**File:** `cpp/bindings/rdma_kv_bindings.cpp`

#### Exposed C++ Classes and Functions

```cpp
// Global singleton for C++ RDMA engine
static std::shared_ptr<kv_cache::KVTransferEngine> g_kv_engine;

PYBIND11_MODULE(rdma_kv_bindings, m) {
    m.doc() = "RDMA KV Cache Transfer Engine Python Bindings";

    // Global engine management
    m.def("set_global_kv_engine", &set_global_kv_engine,
          "Set the global KV transfer engine instance");

    m.def("get_global_kv_engine",
          []() { return g_kv_engine; },
          "Get the global KV transfer engine instance");

    // Main KVTransferEngine class
    py::class_<kv_cache::KVTransferEngine>(m, "KVTransferEngine")

        // GPU memory registration
        .def("register_kv_layer",
             &kv_cache::KVTransferEngine::register_kv_layer,
             py::arg("layer_name"),
             py::arg("gpu_ptr"),
             py::arg("size_bytes"),
             py::arg("shape"),
             "Register a KV cache layer's GPU memory")

        // Async save (prefill → remote)
        .def("save_kv_layer_async",
             &kv_cache::KVTransferEngine::save_kv_layer_async,
             py::arg("request_id"),
             py::arg("layer_name"),
             py::arg("block_ids"),
             py::call_guard<py::gil_scoped_release>(),  // Release GIL!
             "Asynchronously save KV cache layer via RDMA")

        // Wait for load (remote → decode)
        .def("wait_for_layer_load",
             &kv_cache::KVTransferEngine::wait_for_layer_load,
             py::arg("request_id"),
             py::arg("layer_name"),
             py::call_guard<py::gil_scoped_release>(),  // Release GIL!
             "Wait for KV cache layer to be loaded via RDMA")

        // Synchronization
        .def("wait_for_all_saves",
             &kv_cache::KVTransferEngine::wait_for_all_saves,
             py::call_guard<py::gil_scoped_release>(),  // Release GIL!
             "Wait for all pending save operations to complete")

        // Statistics
        .def("get_stats",
             &kv_cache::KVTransferEngine::get_stats,
             "Get transfer statistics");

    // Statistics structure
    py::class_<kv_cache::KVTransferStats>(m, "KVTransferStats")
        .def_readonly("total_layers_transferred",
                      &kv_cache::KVTransferStats::total_layers_transferred)
        .def_readonly("total_bytes_transferred",
                      &kv_cache::KVTransferStats::total_bytes_transferred)
        .def_readonly("avg_layer_transfer_time_ms",
                      &kv_cache::KVTransferStats::avg_layer_transfer_time_ms)
        .def_readonly("avg_bandwidth_gbps",
                      &kv_cache::KVTransferStats::avg_bandwidth_gbps);
}
```

#### Key Features

1. **GIL Management**: `py::call_guard<py::gil_scoped_release>()`
   - Releases Python's Global Interpreter Lock during blocking operations
   - Allows other Python threads to execute while waiting for RDMA
   - Critical for async performance

2. **Global Singleton Pattern**:
   - Single C++ engine shared across all Python threads
   - Thread-safe operations managed by C++ mutexes
   - Initialized once at startup via `set_global_kv_engine()`

### 1.2 Python Connector

**File:** `python/rdma_connector/connector.py`

```python
from vllm.distributed.kv_transfer.kv_connector.v1.base import KVConnectorBase_V1
import rdma_kv_bindings

class RDMAConnector(KVConnectorBase_V1):
    """
    RDMA-based KV cache connector for disaggregated prefill/decode.

    Integrates vLLM with C++ RDMA engine for efficient GPU-to-GPU transfers.
    """

    def __init__(self, vllm_config: VllmConfig, role: KVConnectorRole, ...):
        super().__init__(vllm_config, role, kv_cache_config)

        # Get global C++ engine
        self.cpp_engine = rdma_kv_bindings.get_global_kv_engine()

        # Determine if this instance produces or consumes KV cache
        self.is_producer = self._kv_transfer_config.is_kv_producer

        # vLLM cache configuration
        self._block_size = vllm_config.cache_config.block_size
        self._num_layers = vllm_config.model_config.get_num_layers()

        # Track requests needing KV load (decode side only)
        self._requests_need_load: set[str] = set()

    # ═════════════════════════════════════════════════════════════
    # WORKER-SIDE METHODS (called during forward pass)
    # ═════════════════════════════════════════════════════════════

    def register_kv_caches(self, kv_caches: dict[str, torch.Tensor]) -> None:
        """
        Called ONCE after vLLM allocates KV cache tensors.
        Registers GPU memory with C++ RDMA engine.

        Args:
            kv_caches: {layer_name: torch.Tensor}
                      e.g., {"layers.0.self_attn": tensor([...], device='cuda:0')}
        """
        for layer_name, kv_cache in kv_caches.items():
            # Get CUDA memory pointer
            gpu_ptr = kv_cache.data_ptr()

            # Calculate size in bytes
            size_bytes = kv_cache.numel() * kv_cache.element_size()

            # Get tensor shape for metadata
            shape = list(kv_cache.shape)

            # Register with C++ engine
            self.cpp_engine.register_kv_layer(
                layer_name=layer_name,
                gpu_ptr=gpu_ptr,
                size_bytes=size_bytes,
                shape=shape
            )

        logger.info(f"Registered {len(kv_caches)} KV cache layers with RDMA engine")

    def start_load_kv(self, forward_context: ForwardContext) -> None:
        """
        Called at the START of forward pass (before any layers).
        For RDMA: no-op, loading happens per-layer in wait_for_layer_load().
        """
        pass  # RDMA loads are per-layer

    def wait_for_layer_load(self, layer_name: str) -> None:
        """
        Called BEFORE computing attention for this layer.
        Blocks until RDMA transfer is complete (decode side only).

        For PREFILL: returns immediately (no loading needed)
        For DECODE: blocks until KV cache loaded from prefill instance

        Args:
            layer_name: e.g., "layers.0.self_attn"
        """
        if self.is_producer:
            return  # Prefill doesn't load

        # Get request metadata
        metadata = self._get_connector_metadata()
        if not metadata or not metadata.requests:
            return

        # Load KV cache for all requests in this batch
        for req in metadata.requests:
            request_id = req['request_id']

            # CRITICAL: This blocks until RDMA READ completes
            # GIL is released in C++, so other Python code can run
            self.cpp_engine.wait_for_layer_load(
                request_id=request_id,
                layer_name=layer_name
            )

    def save_kv_layer(
        self,
        layer_name: str,
        kv_layer: torch.Tensor,
        attn_metadata: AttentionMetadata,
        **kwargs
    ) -> None:
        """
        Called AFTER computing attention for this layer.
        Initiates async RDMA transfer (prefill side only).

        For PREFILL: initiates async RDMA WRITE to decode instance
        For DECODE: returns immediately (no saving needed)

        Args:
            layer_name: e.g., "layers.0.self_attn"
            kv_layer: torch.Tensor containing K/V values
            attn_metadata: Attention metadata with block mapping
        """
        if not self.is_producer:
            return  # Decode doesn't save

        # Get request metadata
        metadata = self._get_connector_metadata()
        if not metadata or not metadata.requests:
            return

        # Save KV cache for all requests in this batch
        for req in metadata.requests:
            request_id = req['request_id']
            block_ids = req['block_ids']

            # CRITICAL: This is ASYNC - returns immediately
            # RDMA transfer happens in background
            self.cpp_engine.save_kv_layer_async(
                request_id=request_id,
                layer_name=layer_name,
                block_ids=block_ids
            )

    def wait_for_save(self) -> None:
        """
        Called AFTER forward pass completes.
        Ensures all RDMA writes are done before GPU memory can be reused.

        For PREFILL: blocks until all async saves complete
        For DECODE: returns immediately (no saves to wait for)
        """
        if not self.is_producer:
            return  # Decode doesn't save

        # CRITICAL: This blocks until all RDMA WRITEs complete
        # Ensures GPU memory can be safely reused for next batch
        self.cpp_engine.wait_for_all_saves()

    # ═════════════════════════════════════════════════════════════
    # SCHEDULER-SIDE METHODS (called during request scheduling)
    # ═════════════════════════════════════════════════════════════

    def get_num_new_matched_tokens(
        self,
        request: Request,
        num_computed_tokens: int
    ) -> int:
        """
        Called by scheduler to determine how many tokens from prefill cache.

        For DECODE: returns num_computed_tokens (all prompt tokens cached)
        For PREFILL: returns 0 (nothing cached yet)
        """
        if self.is_producer:
            return 0  # Prefill has no cached tokens
        else:
            # Decode: all prompt tokens come from prefill cache
            return num_computed_tokens

    def update_state_after_alloc(
        self,
        request: Request,
        blocks: list[int],
        num_external_tokens: int
    ) -> None:
        """
        Called after scheduler allocates blocks for a request.
        Track which requests need KV loading (decode side).
        """
        if not self.is_producer and num_external_tokens > 0:
            self._requests_need_load.add(request.req_id)

    def build_connector_meta(
        self,
        scheduler_output: SchedulerOutput
    ) -> RDMAConnectorMetadata:
        """
        Build metadata to send to worker.
        Tells worker which requests need save/load and their block IDs.

        Returns:
            RDMAConnectorMetadata with list of requests to process
        """
        meta = RDMAConnectorMetadata()

        for new_req in scheduler_output.scheduled_new_reqs:
            if self.is_producer:
                # PREFILL: save all new requests
                meta.add_request(
                    request_id=new_req.req_id,
                    block_ids=new_req.block_ids[0]  # First block table
                )
            else:
                # DECODE: load only if request needs external tokens
                if new_req.req_id in self._requests_need_load:
                    meta.add_request(
                        request_id=new_req.req_id,
                        block_ids=new_req.block_ids[0]
                    )
                    # Remove after scheduling
                    self._requests_need_load.discard(new_req.req_id)

        return meta
```

### 1.3 vLLM Integration Points

#### 1.3.1 Attention Decorator

**File:** `vllm/vllm/model_executor/layers/attention/kv_transfer_utils.py`

```python
def maybe_transfer_kv_layer(func: Callable) -> Callable:
    """
    Decorator that wraps attention functions to handle KV cache transfer.

    Execution order:
    1. Load KV cache (decode: blocks until RDMA complete)
    2. Execute attention computation
    3. Save KV cache (prefill: initiates async RDMA)

    This decorator is applied to all attention layers in vLLM models.
    """

    # Find layer_name parameter index
    sig = inspect.signature(func)
    params = list(sig.parameters.keys())
    layer_name_index = params.index('layer_name')

    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        # Check if KV transfer is enabled
        if not has_kv_transfer_group() or not is_v1_kv_transfer_group():
            return func(*args, **kwargs)

        # Extract layer name from arguments
        layer_name: str = args[layer_name_index]

        # Get attention context and connector
        attn_metadata, attn_layer, kv_cache, _ = get_attention_context(layer_name)
        connector = get_kv_transfer_group()

        # Check if we have metadata for this forward pass
        if attn_metadata is None or not connector.has_connector_metadata():
            return func(*args, **kwargs)

        # ═══════════════════════════════════════════════════════
        # STEP 1: WAIT FOR KV LOAD (decode side blocks here)
        # ═══════════════════════════════════════════════════════
        connector.wait_for_layer_load(layer_name)

        # ═══════════════════════════════════════════════════════
        # STEP 2: EXECUTE ATTENTION COMPUTATION
        # ═══════════════════════════════════════════════════════
        result = func(*args, **kwargs)

        # ═══════════════════════════════════════════════════════
        # STEP 3: SAVE KV CACHE (prefill side initiates async)
        # ═══════════════════════════════════════════════════════
        connector.save_kv_layer(layer_name, kv_cache, attn_metadata)

        return result

    return wrapper


# Applied to attention functions
@maybe_transfer_kv_layer
def unified_attention_with_output(
    query: torch.Tensor,
    key: torch.Tensor,
    value: torch.Tensor,
    layer_name: str,  # REQUIRED parameter for decorator
    kv_cache: torch.Tensor,
    attn_metadata: AttentionMetadata,
    attn_type: AttentionType,
    output: torch.Tensor | None = None,
) -> torch.Tensor:
    """
    Unified attention implementation with KV transfer support.
    This function is wrapped by @maybe_transfer_kv_layer decorator.
    """
    # Attention computation happens here
    # K/V values written to kv_cache tensor
    ...
```

#### 1.3.2 Attention Context Extraction

**File:** `vllm/vllm/model_executor/layers/attention/attention.py`

```python
def get_attention_context(layer_name: str):
    """
    Extract attention context for a specific layer from forward context.

    Returns:
        attn_metadata: AttentionMetadata for this layer
        attn_layer: Attention layer instance
        kv_cache: torch.Tensor for KV cache
        layer_slot_mapping: Slot mapping for cache writes
    """
    forward_context: ForwardContext = get_forward_context()

    # Get layer-specific metadata
    attn_metadata = forward_context.attn_metadata.get(layer_name)

    # Get attention layer instance
    attn_layer = forward_context.no_compile_layers.get(layer_name)

    # Get KV cache tensor
    kv_cache = attn_layer.kv_cache if attn_layer else None

    # Get slot mapping (how to write KV values to cache)
    layer_slot_mapping = None  # Extracted from attn_metadata

    return attn_metadata, attn_layer, kv_cache, layer_slot_mapping
```

#### 1.3.3 Configuration

**File:** `vllm/vllm/config/kv_transfer.py`

```python
@dataclass
class KVTransferConfig:
    """
    Configuration for KV cache transfer between instances.
    """

    # Connector type (e.g., "rdma_connector")
    kv_connector: str | None = None

    # Role: "kv_producer", "kv_consumer", or "kv_both"
    kv_role: KVRole | None = None

    # Rank: 0 for prefill, 1+ for decode instances
    kv_rank: int = 0

    # Network configuration
    kv_ip: str | None = None
    kv_port: int | None = None

    # Buffer configuration
    kv_buffer_size: int | None = None

    @property
    def is_kv_transfer_instance(self) -> bool:
        """Check if KV transfer is enabled."""
        return self.kv_connector is not None

    @property
    def is_kv_producer(self) -> bool:
        """Check if this instance produces KV cache (prefill)."""
        return self.kv_role in [KVRole.KV_PRODUCER, KVRole.KV_BOTH]

    @property
    def is_kv_consumer(self) -> bool:
        """Check if this instance consumes KV cache (decode)."""
        return self.kv_role in [KVRole.KV_CONSUMER, KVRole.KV_BOTH]
```

## 2. Request Flow: Prefill Instance

```
╔═══════════════════════════════════════════════════════════════════════════════════╗
║                         PREFILL INSTANCE (kv_producer=True)                       ║
╚═══════════════════════════════════════════════════════════════════════════════════╝

┌─────────────────────────────────────────────────────────────────────────────────┐
│ PHASE 1: INITIALIZATION (once at startup)                                       │
└─────────────────────────────────────────────────────────────────────────────────┘

1. vLLM allocates KV cache tensors:
   kv_caches = {
       "layers.0.self_attn": torch.zeros([num_blocks, block_size, num_heads, head_dim],
                                         device='cuda:0', dtype=torch.float16),
       "layers.1.self_attn": torch.zeros([...]),
       ...
       "layers.31.self_attn": torch.zeros([...])
   }

2. RDMAConnector.register_kv_caches(kv_caches):
   for layer_name, kv_cache in kv_caches.items():
       ├─ gpu_ptr = kv_cache.data_ptr()              # CUDA pointer: 0x7f8b40000000
       ├─ size_bytes = kv_cache.numel() * element_size()  # e.g., 16 MB
       ├─ shape = [num_blocks, block_size, num_heads, head_dim]
       └─ cpp_engine.register_kv_layer(layer_name, gpu_ptr, size_bytes, shape)
          └─ C++ registers GPU memory for RDMA access
             ├─ Creates CUDA memory handle
             ├─ Registers with RDMA device
             └─ Stores mapping: layer_name → gpu_ptr

┌─────────────────────────────────────────────────────────────────────────────────┐
│ PHASE 2: REQUEST SCHEDULING (scheduler process)                                 │
└─────────────────────────────────────────────────────────────────────────────────┘

Request: "How are you?" → tokens: [How, are, you, ?]

3. Scheduler schedules request:
   scheduled_output = SchedulerOutput(
       scheduled_new_reqs=[
           ScheduledRequest(
               req_id="req_12345",
               block_ids=[0, 1],  # Allocated blocks for KV cache
               num_tokens=4
           )
       ]
   )

4. RDMAConnector.build_connector_meta(scheduled_output):
   └─ Creates metadata for worker:
      RDMAConnectorMetadata(
          requests=[{
              'request_id': 'req_12345',
              'block_ids': [0, 1]
          }]
      )

┌─────────────────────────────────────────────────────────────────────────────────┐
│ PHASE 3: FORWARD PASS (worker process)                                          │
└─────────────────────────────────────────────────────────────────────────────────┘

5. Worker receives metadata and binds to forward context:
   forward_context.bind_connector_metadata(metadata)

6. FOR EACH TOKEN in [How, are, you, ?]:

   FOR EACH LAYER in layers[0..31]:

       ┌──────────────────────────────────────────────────────────────────────┐
       │ Layer: "layers.0.self_attn"                                          │
       └──────────────────────────────────────────────────────────────────────┘

       6.1. DECORATOR ENTRY: @maybe_transfer_kv_layer
            ├─ layer_name = "layers.0.self_attn"
            ├─ attn_metadata, attn_layer, kv_cache = get_attention_context(layer_name)
            └─ connector = get_kv_transfer_group()

       6.2. WAIT FOR LOAD:
            └─ connector.wait_for_layer_load("layers.0.self_attn")
               └─ if self.is_producer: return  # NO-OP for prefill

       6.3. ATTENTION COMPUTATION:
            └─ unified_attention_with_output(q, k, v, ...)
               ├─ Compute attention scores
               ├─ Write K/V to kv_cache[blocks[0:2]]
               └─ Return attention output

       6.4. SAVE KV CACHE:
            └─ connector.save_kv_layer("layers.0.self_attn", kv_cache, attn_metadata)
               └─ if self.is_producer:
                  └─ cpp_engine.save_kv_layer_async(
                        request_id="req_12345",
                        layer_name="layers.0.self_attn",
                        block_ids=[0, 1]
                     )
                     ├─ [GIL RELEASED]
                     ├─ C++ initiates RDMA WRITE
                     ├─ Source: GPU memory at kv_cache[blocks[0:2]]
                     ├─ Destination: Remote decode GPU
                     ├─ Transfer happens ASYNC in background
                     └─ Returns immediately

       (Repeat for layers.1.self_attn, layers.2.self_attn, ..., layers.31.self_attn)

┌─────────────────────────────────────────────────────────────────────────────────┐
│ PHASE 4: SYNCHRONIZATION (after forward pass)                                   │
└─────────────────────────────────────────────────────────────────────────────────┘

7. Wait for all RDMA transfers to complete:
   connector.wait_for_save()
   └─ if self.is_producer:
      └─ cpp_engine.wait_for_all_saves()
         ├─ [GIL RELEASED]
         ├─ C++ blocks until all RDMA WRITEs complete
         ├─ Polls completion queue
         └─ Returns when all layers transferred

8. KV cache memory can now be safely reused for next batch

═══════════════════════════════════════════════════════════════════════════════════

TIMELINE VIEW (showing async overlap):

Time →  Layer 0          Layer 1          Layer 2          ...  Layer 31
        ──────────────────────────────────────────────────────────────────────
Token 0:
Compute │████│            │    │          │    │                │    │
Save    │    │░░░░░░░░░░│    │░░░░░░░░│    │░░░░░░░░      │    │░░░░░░░░
        │    │            │    │          │    │                │    │
Token 1:│    │            │    │          │    │                │    │
Compute │    │  ████│     │    │          │    │                │    │
Save    │    │      │░░░░░░░░│    │░░░░░░░░│    │          │    │░░░░░░░░
        │    │            │    │          │    │                │    │
...     │    │            │    │          │    │                │    │
        │    │            │    │          │    │                │    │
Wait    │    │            │    │          │    │                │    │███████
        └────┴────────────┴────┴──────────┴────┴────────────────┴────┘

Legend:
  ████  = Compute (attention)
  ░░░░  = RDMA transfer (async, overlaps with next layer's compute)
  ████  = wait_for_all_saves() (blocks until all transfers done)

Key insight: RDMA transfers for layer N overlap with compute for layer N+1
```

## 3. Request Flow: Decode Instance

```
╔═══════════════════════════════════════════════════════════════════════════════════╗
║                         DECODE INSTANCE (kv_consumer=True)                        ║
╚═══════════════════════════════════════════════════════════════════════════════════╝

┌─────────────────────────────────────────────────────────────────────────────────┐
│ PHASE 1: INITIALIZATION (once at startup)                                       │
└─────────────────────────────────────────────────────────────────────────────────┘

1. vLLM allocates KV cache tensors (same as prefill)

2. RDMAConnector.register_kv_caches(kv_caches)
   (Same as prefill - registers GPU memory for RDMA access)

┌─────────────────────────────────────────────────────────────────────────────────┐
│ PHASE 2: REQUEST SCHEDULING (scheduler process)                                 │
└─────────────────────────────────────────────────────────────────────────────────┘

Request: "req_12345" (same request_id as prefill)

3. Scheduler determines tokens need loading:
   └─ connector.get_num_new_matched_tokens(request, num_computed=4)
      └─ Returns 4 (all prompt tokens from prefill cache)

4. Scheduler allocates blocks:
   └─ connector.update_state_after_alloc(request, blocks=[10, 11], num_external=4)
      └─ self._requests_need_load.add("req_12345")

5. Build metadata for worker:
   └─ connector.build_connector_meta(scheduled_output)
      └─ if "req_12345" in self._requests_need_load:
         └─ RDMAConnectorMetadata(
               requests=[{
                   'request_id': 'req_12345',
                   'block_ids': [10, 11]  # Local blocks for loading
               }]
            )

┌─────────────────────────────────────────────────────────────────────────────────┐
│ PHASE 3: FORWARD PASS (worker process) - Generate token 5                       │
└─────────────────────────────────────────────────────────────────────────────────┘

6. Worker receives metadata and binds to forward context:
   forward_context.bind_connector_metadata(metadata)

7. FOR NEW TOKEN (token 5):

   FOR EACH LAYER in layers[0..31]:

       ┌──────────────────────────────────────────────────────────────────────┐
       │ Layer: "layers.0.self_attn"                                          │
       └──────────────────────────────────────────────────────────────────────┘

       7.1. DECORATOR ENTRY: @maybe_transfer_kv_layer
            ├─ layer_name = "layers.0.self_attn"
            ├─ attn_metadata, attn_layer, kv_cache = get_attention_context(layer_name)
            └─ connector = get_kv_transfer_group()

       7.2. WAIT FOR LOAD (CRITICAL - BLOCKS HERE):
            └─ connector.wait_for_layer_load("layers.0.self_attn")
               └─ if not self.is_producer:
                  └─ cpp_engine.wait_for_layer_load(
                        request_id="req_12345",
                        layer_name="layers.0.self_attn"
                     )
                     ├─ [GIL RELEASED]
                     ├─ C++ initiates RDMA READ (if not already started)
                     ├─ Source: Remote prefill GPU
                     ├─ Destination: Local GPU kv_cache[blocks[10:12]]
                     ├─ BLOCKS until transfer complete
                     ├─ Polls completion queue
                     └─ Returns when K/V for tokens [How, are, you, ?] loaded

       7.3. ATTENTION COMPUTATION:
            └─ unified_attention_with_output(q_new, k_new, v_new, ...)
               ├─ kv_cache now contains:
               │  ├─ tokens[0:4]: [How, are, you, ?] (from RDMA)
               │  └─ token[4]: <new_token> (computed this step)
               │
               ├─ Compute attention:
               │  ├─ scores = Q_new @ K[0:5].T
               │  └─ output = softmax(scores) @ V[0:5]
               │
               └─ Write new token's K/V to kv_cache[blocks[11]][4]
                  (only the NEW token, not the loaded ones)

       7.4. SAVE KV CACHE:
            └─ connector.save_kv_layer("layers.0.self_attn", kv_cache, attn_metadata)
               └─ if not self.is_producer: return  # NO-OP for decode

       (Repeat for layers.1.self_attn, layers.2.self_attn, ..., layers.31.self_attn)

┌─────────────────────────────────────────────────────────────────────────────────┐
│ PHASE 4: SYNCHRONIZATION (after forward pass)                                   │
└─────────────────────────────────────────────────────────────────────────────────┘

8. Wait for saves (NO-OP for decode):
   connector.wait_for_save()
   └─ if not self.is_producer: return  # NO-OP

9. Generate next token and return to scheduler

═══════════════════════════════════════════════════════════════════════════════════

TIMELINE VIEW:

Time →  Layer 0          Layer 1          Layer 2          ...  Layer 31
        ──────────────────────────────────────────────────────────────────────
Load    │████████│       │████████│      │████████│            │████████│
Compute │        │████│  │        │████│ │        │████│       │        │████│
        └────────┴────┴──┴────────┴────┴─┴────────┴────┴───────┴────────┴────┘

Legend:
  ████  = RDMA load (blocks before compute can start)
  ████  = Compute (attention with loaded + new K/V)

Key insight: Each layer blocks on RDMA load before computing attention
```

## 4. Memory Layout and RDMA Transfer Details

### 4.1 KV Cache Tensor Structure

```python
# vLLM KV cache tensor shape (PagedAttention)
kv_cache = torch.Tensor(
    shape=[
        num_blocks,      # e.g., 1024 blocks
        2,               # K and V
        block_size,      # e.g., 16 tokens per block
        num_heads,       # e.g., 32 attention heads
        head_dim         # e.g., 128 dimensions per head
    ],
    dtype=torch.float16,
    device='cuda:0'
)

# Example dimensions:
# [1024, 2, 16, 32, 128] = 1024 * 2 * 16 * 32 * 128 * 2 bytes
#                         = ~134 MB per layer
```

### 4.2 Block-based Transfer

```
Request: 4 tokens = [How, are, you, ?]
Block size: 16 tokens
Allocated blocks: [0, 1]  (need 2 blocks for 4 tokens)

PREFILL saves:
├─ Block 0: tokens[0:16]   (only first 4 used: [How, are, you, ?])
└─ Block 1: tokens[16:32]  (unused, but allocated)

RDMA transfer:
├─ Source GPU:  kv_cache[block_0:2, :, :, :, :]
├─ Dest GPU:    kv_cache[block_10:12, :, :, :, :]
└─ Size: 2 blocks * 2 * 16 * 32 * 128 * 2 bytes = ~524 KB per layer
         32 layers * 524 KB = ~16.4 MB total transfer

DECODE loads:
├─ Block 10: tokens[0:16]  (uses first 4: [How, are, you, ?])
└─ Block 11: tokens[16:32] (writes new token at position 4)
```

### 4.3 C++ RDMA Transfer Implementation

```cpp
// Conceptual C++ code (simplified)

void KVTransferEngine::save_kv_layer_async(
    const std::string& request_id,
    const std::string& layer_name,
    const std::vector<int>& block_ids
) {
    // Get registered layer info
    auto& layer_info = registered_layers_[layer_name];
    void* gpu_base_ptr = layer_info.gpu_ptr;

    // Calculate source address for blocks
    size_t block_size_bytes = 2 * 16 * 32 * 128 * sizeof(float16);
    void* src_ptr = gpu_base_ptr + (block_ids[0] * block_size_bytes);
    size_t transfer_size = block_ids.size() * block_size_bytes;

    // Get destination address on remote GPU
    void* dest_ptr = get_remote_address(request_id, layer_name, block_ids);

    // Post RDMA WRITE work request
    ibv_sge sge = {
        .addr = (uintptr_t)src_ptr,
        .length = transfer_size,
        .lkey = layer_info.lkey  // Local memory key
    };

    ibv_send_wr wr = {
        .wr_id = generate_wr_id(request_id, layer_name),
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma = {
            .remote_addr = (uintptr_t)dest_ptr,
            .rkey = remote_rkey_  // Remote memory key
        }
    };

    // Post to send queue (non-blocking)
    ibv_post_send(qp_, &wr, &bad_wr);

    // Track pending transfer
    pending_transfers_[wr.wr_id] = TransferInfo{
        .request_id = request_id,
        .layer_name = layer_name,
        .start_time = now()
    };
}

void KVTransferEngine::wait_for_layer_load(
    const std::string& request_id,
    const std::string& layer_name
) {
    // Check if load already completed
    auto key = make_pair(request_id, layer_name);
    if (completed_loads_.count(key)) {
        return;  // Already loaded
    }

    // Wait for RDMA READ completion
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] {
        return completed_loads_.count(key) > 0;
    });
}

void KVTransferEngine::wait_for_all_saves() {
    // Poll completion queue until all pending transfers done
    while (!pending_transfers_.empty()) {
        ibv_wc wc;
        int n = ibv_poll_cq(cq_, 1, &wc);

        if (n > 0 && wc.status == IBV_WC_SUCCESS) {
            // Transfer completed
            auto it = pending_transfers_.find(wc.wr_id);
            if (it != pending_transfers_.end()) {
                // Update stats
                update_stats(it->second);

                // Remove from pending
                pending_transfers_.erase(it);
            }
        }
    }
}
```

## 5. Configuration Example

### Prefill Instance

```bash
python -m vllm.entrypoints.openai.api_server \
    --model meta-llama/Llama-2-7b-hf \
    --kv-connector rdma_connector \
    --kv-role kv_producer \
    --kv-rank 0 \
    --kv-ip 192.168.1.100 \
    --kv-port 50051 \
    --gpu-memory-utilization 0.8 \
    --max-num-seqs 256
```

### Decode Instance

```bash
python -m vllm.entrypoints.openai.api_server \
    --model meta-llama/Llama-2-7b-hf \
    --kv-connector rdma_connector \
    --kv-role kv_consumer \
    --kv-rank 1 \
    --kv-ip 192.168.1.101 \
    --kv-port 50052 \
    --kv-producer-ip 192.168.1.100 \
    --kv-producer-port 50051 \
    --gpu-memory-utilization 0.9 \
    --max-num-seqs 512
```

## 6. Performance Characteristics

### Transfer Latencies

| Operation | Latency | Notes |
|-----------|---------|-------|
| `register_kv_layer()` | ~1-5 ms | One-time cost at startup |
| `save_kv_layer_async()` | <0.1 ms | Non-blocking, returns immediately |
| `wait_for_layer_load()` | ~0.5-2 ms | Blocks, depends on transfer size |
| `wait_for_all_saves()` | ~10-50 ms | Waits for all layers (32 layers) |

### RDMA Bandwidth

- **Theoretical**: 100 Gbps (InfiniBand EDR) = 12.5 GB/s
- **Practical**: 80-90 Gbps = 10-11 GB/s
- **Per-layer transfer**: ~16 MB / 11 GB/s = ~1.5 ms
- **All 32 layers**: ~512 MB / 11 GB/s = ~46 ms

### Computation vs Transfer Overlap

```
Without RDMA (monolithic):
├─ Prefill: 100 ms (GPU 0)
├─ Transfer: 50 ms (CPU copy via PCIe + network)
└─ Decode: 10 ms per token (GPU 0)
Total latency: 100 + 50 + 10 = 160 ms for first token

With RDMA (disaggregated):
├─ Prefill: 100 ms (GPU 0)
│  └─ RDMA transfer overlaps: 46 ms (background)
├─ Transfer wait: 4 ms (remaining transfer time)
└─ Decode: 10 ms per token (GPU 1)
Total latency: 100 + 4 + 10 = 114 ms for first token

Speedup: ~40% reduction in TTFT
```

## 7. Key Takeaways

1. **Single Integration Point**: The `@maybe_transfer_kv_layer` decorator is the ONLY place where RDMA integration happens in vLLM's attention code.

2. **Transparent to Models**: Model code doesn't need to know about RDMA - it just calls attention functions normally.

3. **Async Prefill Saves**: Prefill instance initiates async RDMA writes and continues computing next layers, maximizing overlap.

4. **Blocking Decode Loads**: Decode instance must wait for each layer's KV cache before computing attention - unavoidable dependency.

5. **GIL Release**: Critical for performance - blocking C++ calls release Python's GIL so other operations can proceed.

6. **Block-based Granularity**: Transfers happen at block granularity (16 tokens/block) for efficient memory management.

7. **Metadata-driven**: Scheduler creates metadata specifying which requests need save/load, keeping worker logic simple.

8. **Statistics Tracking**: C++ engine tracks bandwidth, latency, and throughput for monitoring and debugging.

This architecture enables efficient GPU-to-GPU KV cache transfer with minimal changes to vLLM's core attention implementation!
