# python/rdma_connector.py
"""
  RDMAConnector - GPUDirect RDMA-based KV cache transfer for vLLM

  This connector leverages vLLM's disaggregated inference API to transfer
  KV cache between prefill and decode nodes using RDMA instead of disk.

  Key optimizations:
  - Prefill: Zero-copy RDMA write directly from vLLM's KV cache (register memory)
  - Decode: One-copy injection from RDMA staging buffer to vLLM's KV cache
"""

import os
from typing import TYPE_CHECKING, Any
import node_accessor

import torch

from vllm.config import VllmConfig
from vllm.distributed.kv_transfer.kv_connector.v1.example_connector import (
      ExampleConnector,
      ExampleConnectorMetadata,
  )
from vllm.logger import init_logger
from vllm.utils.hashing import safe_hash
from vllm.v1.attention.backend import AttentionMetadata

  # Import C++ RDMA bindings
try:
      import rdma_bindings
      RDMA_AVAILABLE = True
except ImportError:
      RDMA_AVAILABLE = False
      import warnings
      warnings.warn("rdma_bindings not available, running in simulation mode")

if TYPE_CHECKING:
      from vllm.forward_context import ForwardContext
      from vllm.v1.kv_cache_interface import KVCacheConfig
      from vllm.distributed.kv_transfer.kv_connector.v1.base import KVConnectorRole

logger = init_logger(__name__)


class RDMAConnector(ExampleConnector):
      """
      RDMA-based KV connector for vLLM disaggregated inference
      
      Configuration via kv_connector_extra_config:
          - role: "send" (prefill) or "recv" (decode)
          - peer_id: ID of remote peer node (for RDMA connection)
          - rdma_device: InfiniBand device name (e.g., "mlx5_0")
          - rdma_port: IB port number (default: 1)
          - rdma_gid_index: GID index (default: 0)
          - rdma_qp_num: Queue pair number for remote peer
          - rdma_remote_addr: Remote GPU base address
          - rdma_remote_rkey: Remote memory region key
      """

      def __init__(
          self,
          vllm_config: "VllmConfig",
          role: "KVConnectorRole",
          kv_cache_config: "KVCacheConfig | None" = None,
      ):
          super().__init__(vllm_config, role, kv_cache_config)

          # RDMA configuration
          self._role_str = self._kv_transfer_config.get_from_extra_config("role", "send")
          self._rdma_device = self._kv_transfer_config.get_from_extra_config(
              "rdma_device", "mlx5_0"
          )
          self._rdma_port = self._kv_transfer_config.get_from_extra_config("rdma_port", 1)
          self._rdma_gid_index = self._kv_transfer_config.get_from_extra_config(
              "rdma_gid_index", 0
          )

          # Remote peer information
          self._rdma_qp_num = self._kv_transfer_config.get_from_extra_config("rdma_qp_num", 0)
          self._peer_id = self._kv_transfer_config.get_from_extra_config("peer_id", 0)
          self._rdma_remote_addr = self._kv_transfer_config.get_from_extra_config(
              "rdma_remote_addr", 0
          )
          self._rdma_remote_rkey = self._kv_transfer_config.get_from_extra_config(
              "rdma_remote_rkey", 0
          )

          # Layer size for offset calculation
          self._layer_size = self._kv_transfer_config.get_from_extra_config(
              "layer_size", 128 * 1024 * 1024  # 128MB default
          )

          # Initialize RDMA
          if RDMA_AVAILABLE:
              engine_ptr = node_accessor.get_node_rdma_engine_ptr()

              if not engine_ptr:
                    raise RuntimeError("Failed to get RDMA engine pointer from node accessor")
              self._rdma = rdma_bindings.RDMABindings(engine_ptr)  

          else:
              self._rdma = None

          # State tracking
          self._kv_caches_registered = False
          self._staging_buffer_tensor = None

          # For decode node: wrap staging buffer
          if self._role_str == "recv" and RDMA_AVAILABLE:
              self._init_decode_staging_buffer()

          logger.info(
              f"RDMAConnector initialized: role={self._role_str}, "
              f"device={self._rdma_device}, "
              f"rdma_available={RDMA_AVAILABLE}"
          )

      # ========================================================================
      # Memory Registration (Prefill Node)
      # ========================================================================

      def register_kv_caches(self, kv_caches: dict[str, torch.Tensor]):
          """
          Called by vLLM after KV cache allocation.
          Register all KV cache memory with RDMA for zero-copy writes.
          
          This is the KEY method for prefill node optimization!
          """
          if self._role_str != "send":
              return

          if self._kv_caches_registered:
              logger.warning("KV caches already registered with RDMA")
              return

          if not RDMA_AVAILABLE:
              logger.warning("RDMA not available, skipping registration")
              return

          logger.info("Registering vLLM KV caches with RDMA...")

          for layer_name, kv_cache in kv_caches.items():
              gpu_ptr = kv_cache.data_ptr()
              size = kv_cache.numel() * kv_cache.element_size()

              success = self._rdma.register_gpu_memory(gpu_ptr, size, layer_name)
              if not success:
                  raise RuntimeError(f"Failed to register layer {layer_name} with RDMA")

              logger.info(
                  f"Registered {layer_name}: "
                  f"ptr=0x{gpu_ptr:x}, "
                  f"size={size/(1024**2):.1f}MB"
              )

          self._kv_caches_registered = True
          logger.info("All KV caches registered with RDMA (zero-copy enabled)")

      # ========================================================================
      # Decode Node: Staging Buffer Setup
      # ========================================================================

      def _init_decode_staging_buffer(self):
          """
          For decode node: wrap C++-allocated RDMA staging buffer as PyTorch tensor.
          Skipped when CUDA is not available (use_gpu=false / MooncakeConnector path).
          """
          if not RDMA_AVAILABLE:
              return

          if not torch.cuda.is_available():
              logger.info(
                  "CUDA not available — skipping RDMA staging buffer setup "
                  "(MooncakeConnector manages its own buffers)"
              )
              return

          # Get pre-allocated buffer from C++
          buffer_ptr = self._rdma.get_staging_buffer_ptr()
          buffer_size = self._rdma.get_staging_buffer_size()

          if buffer_ptr == 0 or buffer_size == 0:
              raise RuntimeError("No staging buffer available from RDMA engine")

          logger.info(
              f"Wrapping RDMA staging buffer: "
              f"ptr=0x{buffer_ptr:x}, "
              f"size={buffer_size/(1024**3):.1f}GB"
          )

          # Wrap as PyTorch tensor (zero-copy)
          self._staging_buffer_tensor = self._wrap_gpu_pointer(buffer_ptr, buffer_size)
          logger.info("Staging buffer wrapped as PyTorch tensor")

      def _wrap_gpu_pointer(self, gpu_ptr: int, size_bytes: int) -> torch.Tensor:
          """
          Wrap C++-allocated GPU memory as PyTorch tensor (zero-copy)

          FIX BUG #15: Revert to private API - it's the only option

          IMPORTANT: This uses PyTorch's private API (HalfStorage._new_with_data_ptr)
          because there is NO public API to wrap external GPU pointers.

          The memory is allocated by C++ (cudaMalloc) and registered for RDMA.
          PyTorch does not provide a public API for this use case:
          - torch.frombuffer() only works with CPU memory
          - DLPack requires memory from another DL framework (CuPy/JAX/TF)
          - CUDA Array Interface is not supported by PyTorch

          The private API is the only working solution until PyTorch adds
          public support for external GPU memory wrapping.
          """
          if not torch.cuda.is_available():
              raise RuntimeError(
                  "_wrap_gpu_pointer called but CUDA is not available. "
                  "This code path is only reached when use_gpu=true (GPUDirect RDMAConnector). "
                  "If you are running without a GPU, set use_gpu=false in your node config "
                  "and use MooncakeConnector instead."
              )

          dtype = torch.float16  # Match model dtype
          element_size = torch.finfo(dtype).bits // 8
          num_elements = size_bytes // element_size

          try:
              # Use private API - only option for external GPU memory
              # This has been stable across PyTorch versions 1.x-2.x
              storage = torch.cuda.HalfStorage._new_with_data_ptr(
                  data_ptr=gpu_ptr,
                  size=num_elements
              )
              tensor = torch.tensor([], dtype=dtype, device='cuda').set_(
                  storage=storage,
                  storage_offset=0,
                  size=(num_elements,)
              )

              # Verify zero-copy worked
              if tensor.data_ptr() != gpu_ptr:
                  raise RuntimeError(
                      f"Tensor wrapping failed: expected ptr=0x{gpu_ptr:x}, "
                      f"got ptr=0x{tensor.data_ptr():x}. "
                      f"This indicates a copy occurred instead of zero-copy wrapping."
                  )

              logger.debug(
                  f"Wrapped GPU buffer: shape={tensor.shape}, "
                  f"ptr=0x{tensor.data_ptr():x}, "
                  f"size={size_bytes/(1024**2):.1f}MB"
              )

              return tensor

          except AttributeError as e:
              # Private API removed in future PyTorch version
              raise RuntimeError(
                  f"PyTorch private API unavailable (version {torch.__version__}). "
                  f"This RDMA connector requires torch.cuda.HalfStorage._new_with_data_ptr "
                  f"to wrap C++-allocated GPU memory. "
                  f"Please file an issue at https://github.com/pytorch/pytorch requesting "
                  f"public API for external GPU memory wrapping (cudaMalloc-based tensors)."
              ) from e
          except Exception as e:
              raise RuntimeError(
                  f"Failed to wrap GPU pointer 0x{gpu_ptr:x} as PyTorch tensor: {e}"
              ) from e

      # ========================================================================
      # Prefill Node: Save KV (Zero-Copy RDMA Write)
      # ========================================================================

      def save_kv_layer(
          self,
          layer_name: str,
          kv_layer: torch.Tensor,
          attn_metadata: AttentionMetadata,
          **kwargs: Any,
      ) -> None:
          """
          Save KV layer via RDMA (zero-copy on prefill node)
          
          vLLM gives us kv_layer (GPU tensor in paged format).
          We extract KV and RDMA write directly from vLLM's memory.
          """
          if self._role_str != "send":
              return

          connector_metadata = self._get_connector_metadata()
          if not isinstance(connector_metadata, ExampleConnectorMetadata):
              return

          for request in connector_metadata.requests:
              if not request.is_store:
                  continue

              # Extract KV from vLLM's paged buffer (use parent's logic)
              kv_cache = self._extract_kv_from_layer_parent(kv_layer, request.slot_mapping)
              # kv_cache.shape: (2, num_tokens, hidden_dim)

              # Calculate RDMA offset
              rdma_offset = self._calculate_rdma_offset(
                  layer_name, request.token_ids, request.mm_hashes
              )

              # Extract layer ID for imm_data encoding
              layer_id = self._extract_layer_id(layer_name)

              # Get request sequence number from slot_id (orchestrator-assigned)
              # This matches the seq_num used by decode's RequestTrackerLayer
              slot_id = getattr(request, 'slot_id', None)
              if slot_id is not None:
                  request_seq = slot_id  # Use orchestrator-assigned slot as seq_num
              else:
                  # Fallback to hash-based for backward compatibility
                  request_seq = self._get_request_seq(request.token_ids, request.mm_hashes)
                  logger.warning(
                      f"Request missing slot_id, using legacy hash-based seq_num={request_seq}. "
                      f"This may cause decode-side lookup failures!"
                  )

              # RDMA write K and V caches
              if kv_cache.shape[0] == 2:  # Standard format: (2, num_tokens, hidden_dim)
                  k_cache = kv_cache[0]
                  v_cache = kv_cache[1]
              else:  # MLA format
                  k_cache = kv_cache
                  v_cache = kv_cache

              # Zero-copy RDMA write K cache
              if RDMA_AVAILABLE:
                  self._rdma_write(
                      local_ptr=k_cache.data_ptr(),  # vLLM's GPU memory (registered!)
                      remote_offset=rdma_offset,  # Calculated offset for this layer/request
                      size=k_cache.numel() * k_cache.element_size(),
                      request_seq=request_seq,  # FIX BUG #11: Pass request sequence
                      layer_id=layer_id  # FIX BUG #11: Pass layer ID for identification
                  )

                  # RDMA write V cache (right after K)
                  v_offset = k_cache.numel() * k_cache.element_size()
                  self._rdma_write(
                      local_ptr=v_cache.data_ptr(),
                      remote_offset=rdma_offset + v_offset,
                      size=v_cache.numel() * v_cache.element_size(),
                      request_seq=request_seq,
                      layer_id=layer_id
                  )

              logger.debug(
                  f"Zero-copy RDMA write: layer={layer_name}, "
                  f"K_size={k_cache.nbytes/(1024**2):.1f}MB, "
                  f"V_size={v_cache.nbytes/(1024**2):.1f}MB"
              )

      def _extract_kv_from_layer_parent(
          self, kv_layer: torch.Tensor, slot_mapping: torch.Tensor
      ) -> torch.Tensor:
          """
          Extract KV from vLLM's paged attention buffer
          Reuses ExampleConnector's logic
          """
          from vllm.model_executor.layers.attention.mla_attention import MLACommonMetadata
          from vllm.v1.attention.backends.triton_attn import TritonAttentionMetadata

          # Get attention metadata
          connector_metadata = self._get_connector_metadata()
          if not isinstance(connector_metadata, ExampleConnectorMetadata):
              raise RuntimeError("Invalid connector metadata")

          # Shape: (2, num_blocks, block_size, hidden_dim) → extract at slots
          if kv_layer.ndim == 4:  # Standard paged attention
              num_blocks = kv_layer.shape[1]
              block_size = kv_layer.shape[2]
              kv_layer_flat = kv_layer.reshape(2, num_blocks * block_size, -1)
              return kv_layer_flat[:, slot_mapping, ...]
          else:
              # MLA or other format
              return kv_layer[slot_mapping, ...]

      # ========================================================================
      # Decode Node: Load KV (From Staging Buffer)
      # ========================================================================

      def start_load_kv(self, forward_context: "ForwardContext", **kwargs: Any) -> None:
          """
          Load KV cache from RDMA staging buffer into vLLM's KV cache
          
          RDMA data has already arrived in staging buffer.
          We create tensor views and inject into vLLM.
          """
          if self._role_str != "recv":
              return

          metadata = self._get_connector_metadata()
          if not isinstance(metadata, ExampleConnectorMetadata):
              return

          attn_metadata = forward_context.attn_metadata
          if attn_metadata is None:
              logger.warning("start_load_kv called but attn_metadata is None")
              return

          for request in metadata.requests:
              if request.is_store:
                  continue

              logger.info(
                  f"Loading KV cache from RDMA staging buffer: "
                  f"{len(request.slot_mapping)} tokens"
              )

              # Inject KV into each layer
              for layer_name in forward_context.no_compile_layers:
                  layer = forward_context.no_compile_layers[layer_name]
                  kv_cache_layer = getattr(layer, "kv_cache", None)

                  if kv_cache_layer is None:
                      continue

                  # Calculate offset in staging buffer
                  rdma_offset = self._calculate_rdma_offset(
                      layer_name, request.token_ids, request.mm_hashes
                  )

                  # Create view into staging buffer (zero-copy)
                  kv_cache = self._load_kv_from_staging_buffer(
                      offset=rdma_offset,
                      num_tokens=len(request.slot_mapping),
                      hidden_dim=kv_cache_layer.shape[-1],
                  )

                  # Inject into vLLM (copies data)
                  if isinstance(attn_metadata, dict):
                      self._inject_kv_into_layer_parent(
                          kv_cache_layer,
                          kv_cache,
                          request.slot_mapping,
                          attn_metadata[layer_name],
                      )
                  else:
                      self._inject_kv_into_layer_parent(
                          kv_cache_layer,
                          kv_cache,
                          request.slot_mapping,
                          attn_metadata,
                      )

                  logger.debug(f"Loaded layer {layer_name} from staging buffer")

      def _load_kv_from_staging_buffer(
          self, offset: int, num_tokens: int, hidden_dim: int
      ) -> torch.Tensor:
          """
          Create view into RDMA staging buffer (zero-copy)
          """
          if self._staging_buffer_tensor is None:
              raise RuntimeError("Staging buffer not initialized")

          # Calculate size
          # Shape: (2, num_tokens, hidden_dim) for K and V
          num_elements = 2 * num_tokens * hidden_dim

          # Create view into staging buffer
          kv_view = self._staging_buffer_tensor[offset:offset + num_elements]
          kv_tensor = kv_view.view(2, num_tokens, hidden_dim)

          logger.debug(
              f"Created staging buffer view: "
              f"offset={offset}, "
              f"shape={kv_tensor.shape}, "
              f"size={kv_tensor.nbytes/(1024**2):.1f}MB"
          )

          return kv_tensor

      def _inject_kv_into_layer_parent(
          self,
          dst_kv_cache_layer: torch.Tensor,
          src_kv_cache: torch.Tensor,
          slot_mapping: torch.Tensor,
          attn_metadata: AttentionMetadata,
      ) -> None:
          """
          Inject KV into vLLM's paged cache
          Reuses ExampleConnector's logic
          """
          from vllm.model_executor.layers.attention.mla_attention import MLACommonMetadata
          from vllm.v1.attention.backends.triton_attn import TritonAttentionMetadata

          dst_shape = dst_kv_cache_layer.shape

          if isinstance(attn_metadata, MLACommonMetadata):
              # MLA format
              num_pages = dst_shape[0]
              page_size = dst_shape[1]
              dst_kv_cache_layer = dst_kv_cache_layer.reshape(num_pages * page_size, -1)
              dst_kv_cache_layer[slot_mapping, ...] = src_kv_cache

          elif isinstance(attn_metadata, TritonAttentionMetadata):
              # Triton backend
              block_idxs = slot_mapping // self._block_size
              offsets = slot_mapping % self._block_size
              dst_kv_cache_layer[block_idxs, :, offsets] = src_kv_cache

          else:
              # Standard paged attention: (2, num_pages, page_size, hidden_dim)
              num_pages = dst_shape[1]
              page_size = dst_shape[2]
              dst_kv_cache_layer = dst_kv_cache_layer.reshape(2, num_pages * page_size, -1)
              dst_kv_cache_layer[:, slot_mapping, ...] = src_kv_cache

          logger.debug(f"Injected KV with {len(slot_mapping)} slots")

      # ========================================================================
      # Helper Methods
      # ========================================================================

      def _rdma_write(self, local_ptr: int, remote_offset: int, size: int,
                      request_seq: int = 0, layer_id: int = 0) -> None:
          """
          Perform RDMA write operation

          Args:
              local_ptr: Local GPU memory address
              remote_offset: Offset in remote buffer
              size: Number of bytes to transfer
              request_seq: Request sequence number (16-bit)
              layer_id: Layer ID (16-bit)
          """
          if RDMA_AVAILABLE:
              try:
                  # FIX BUG #11: Encode layer identification in imm_data
                  # Format: [16-bit request_seq | 16-bit layer_id]
                  # This allows decode node to identify which layer arrived
                  imm_data = self._encode_imm_data(request_seq, layer_id)

                  success = self._rdma.rdma_write(
                      peer_id=self._peer_id,
                      local_addr=local_ptr,
                      remote_offset=remote_offset,
                      size=size,
                      imm_data=imm_data  # ✓ Now passes layer identification
                  )
                  if not success:
                      raise RuntimeError("RDMA write failed")

                  logger.debug(f"RDMA write: {size} bytes, offset=0x{remote_offset:x}, "
                               f"req_seq={request_seq}, layer={layer_id}, imm_data=0x{imm_data:08x}")
              except Exception as e:
                  logger.error(f"RDMA write failed: {e}")
                  raise
          else:
              logger.warning(f"Simulated RDMA write: {size} bytes to offset 0x{remote_offset:x}, "
                             f"layer={layer_id}")

      def _encode_imm_data(self, request_seq: int, layer_id: int) -> int:
          """
          Encode request sequence and layer ID into 32-bit immediate data

          Format: [16-bit request_seq | 16-bit layer_id]

          Args:
              request_seq: Request sequence number (0-65535)
              layer_id: Layer ID (0-65535)

          Returns:
              32-bit encoded immediate data
          """
          # Validate bounds
          if request_seq < 0 or request_seq > 0xFFFF:
              logger.warning(f"request_seq {request_seq} out of range, truncating to 16-bit")
              request_seq = request_seq & 0xFFFF

          if layer_id < 0 or layer_id > 0xFFFF:
              logger.warning(f"layer_id {layer_id} out of range, truncating to 16-bit")
              layer_id = layer_id & 0xFFFF

          # Encode: upper 16 bits = request_seq, lower 16 bits = layer_id
          imm_data = (request_seq << 16) | layer_id
          return imm_data

      def _calculate_rdma_offset(
          self, layer_name: str, token_ids: torch.Tensor, mm_hashes: list[str]
      ) -> int:
          """
          Calculate offset in RDMA buffer for a layer using LAYER-BASED addressing.

          NEW: Layer-based offset calculation (not hash-based chunking)

          Structure:
              [Base for Slot] + [Layer Offset]

          Example for 8 concurrent requests, 32 layers, 128MB/layer:
              Request 0 Layer 0: offset = (0 * 4GB) + (0 * 128MB) = 0
              Request 0 Layer 1: offset = (0 * 4GB) + (1 * 128MB) = 128MB
              Request 1 Layer 0: offset = (1 * 4GB) + (0 * 128MB) = 4GB
              Request 7 Layer 31: offset = (7 * 4GB) + (31 * 128MB) = 28GB + 3.875GB

          This eliminates hash collisions and provides deterministic addressing.
          """
          # Get slot_id from request metadata
          connector_metadata = self._get_connector_metadata()
          if not isinstance(connector_metadata, ExampleConnectorMetadata):
              raise RuntimeError("Invalid connector metadata for offset calculation")

          # Find request matching these token_ids
          slot_id = None
          slot_base_offset = None
          for request in connector_metadata.requests:
              # Match by token_ids
              if torch.equal(request.token_ids, token_ids):
                  # NEW: Get slot information from request metadata
                  slot_id = getattr(request, 'slot_id', None)
                  slot_base_offset = getattr(request, 'slot_base_offset', None)
                  break

          if slot_id is None or slot_base_offset is None:
              # Fallback to old hash-based method for backward compatibility
              logger.warning(
                  f"Request missing slot_id/slot_base_offset, using legacy hash-based offset. "
                  f"This should not happen in production!"
              )
              token_bytes = token_ids.numpy().tobytes()
              if mm_hashes:
                  mm_str = "-".join(mm_hashes)
                  token_bytes += mm_str.encode("utf-8")

              request_hash = safe_hash(token_bytes, usedforsecurity=False).hexdigest()
              offset_base = int(request_hash[:16], 16) % (1 << 32)  # Within 4GB
              layer_id = self._extract_layer_id(layer_name)
              layer_offset = layer_id * self._layer_size
              return offset_base + layer_offset

          # NEW CALCULATION: Layer-based offset
          # Base offset for this request's slot
          # + Layer offset within slot
          layer_id = self._extract_layer_id(layer_name)
          layer_offset_within_slot = layer_id * self._layer_size

          final_offset = slot_base_offset + layer_offset_within_slot

          # CRITICAL: Bounds checking
          # Each request has num_layers * layer_size bytes allocated
          num_layers = self._kv_transfer_config.get_from_extra_config("num_layers", 32)
          max_offset_for_slot = slot_base_offset + (num_layers * self._layer_size)

          if final_offset >= max_offset_for_slot:
              raise ValueError(
                  f"RDMA offset out of bounds! "
                  f"slot_id={slot_id}, "
                  f"layer_id={layer_id}, "
                  f"final_offset={final_offset} >= max_offset={max_offset_for_slot}. "
                  f"This indicates a configuration error (num_layers or layer_size)."
              )

          # Additional sanity check: ensure we don't exceed buffer size
          buffer_size = self._kv_transfer_config.get_from_extra_config(
              "buffer_size_mb", 16384
          ) * 1024 * 1024
          if final_offset >= buffer_size:
              raise ValueError(
                  f"RDMA offset exceeds total buffer size! "
                  f"offset={final_offset} >= buffer_size={buffer_size}"
              )

          logger.debug(
              f"Layer-based offset: slot={slot_id}, layer={layer_id}, "
              f"base=0x{slot_base_offset:x}, layer_offset=0x{layer_offset_within_slot:x}, "
              f"final=0x{final_offset:x}"
          )

          return final_offset

      def _extract_layer_id(self, layer_name: str) -> int:
          """Extract numeric layer ID from layer name (e.g., 'layers.5' → 5)"""
          parts = layer_name.split(".")
          for part in parts:
              if part.isdigit():
                  return int(part)
          return 0

      def _get_request_seq(self, token_ids: torch.Tensor, mm_hashes: list[str]) -> int:
          """
          Get request sequence number for imm_data encoding

          Uses hash of token_ids to generate a 16-bit sequence number.
          In a full implementation, this could come from the orchestrator.

          Args:
              token_ids: Request token IDs
              mm_hashes: Multimodal hashes

          Returns:
              16-bit request sequence number (0-65535)
          """
          token_bytes = token_ids.numpy().tobytes()
          if mm_hashes:
              mm_str = "-".join(mm_hashes)
              token_bytes += mm_str.encode("utf-8")

          request_hash = safe_hash(token_bytes, usedforsecurity=False).hexdigest()
          # Use first 4 hex digits for 16-bit sequence
          request_seq = int(request_hash[:4], 16)
          return request_seq

      def wait_for_save(self):
          """Wait for all RDMA transfers to complete"""
          if RDMA_AVAILABLE:
              try:
                  self._rdma.rdma_wait_completion()
                  logger.info("All RDMA transfers completed")
              except Exception as e:
                  logger.error(f"RDMA wait failed: {e}")
          else:
              logger.info("Simulated RDMA wait")


  # ============================================================================
  # Factory function for vLLM
  # ============================================================================

def create_rdma_connector(
      vllm_config: VllmConfig,
      role: "KVConnectorRole",
      kv_cache_config: "KVCacheConfig | None" = None,
  ) -> RDMAConnector:
      """Factory function called by vLLM to instantiate connector"""
      return RDMAConnector(vllm_config, role, kv_cache_config)