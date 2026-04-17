from typing import Any, Optional
import torch
from vllm.distributed.kv_transfer.kv_connector.v1.base import (
      KVConnectorBase_V1,
      KVConnectorRole,
      KVConnectorMetadata,
      KVConnectorWorkerMetadata
  )
from vllm.config import VllmConfig
from vllm.v1.attention.backend import AttentionMetadata
from vllm.v1.core.sched.output import SchedulerOutput
from vllm.v1.kv_cache_interface import KVCacheConfig
from vllm.forward_context import ForwardContext
from vllm.v1.request import Request
from vllm.v1.core.kv_cache_manager import KVCacheBlocks

try:
      import rdma_kv_bindings
except ImportError:
      raise ImportError(
          "rdma_kv_bindings module not found. "
          "Ensure C++ daemon is running and bindings are compiled."
      )


  class RDMAConnectorMetadata(KVConnectorMetadata):
      """Metadata passed from scheduler to worker."""
      def __init__(self):
          self.requests = []

      def add_request(self, request_id: str, block_ids: list[int]):
          self.requests.append({
              'request_id': request_id,
              'block_ids': block_ids
          })


  class RDMAConnector(KVConnectorBase_V1):
      """
      Custom RDMA KV connector for vLLM.
      
      This connector interfaces with the C++ RDMA KV Transfer Engine
      for high-performance GPU-to-GPU KV cache transfers.
      """

      def __init__(
          self,
          vllm_config: VllmConfig,
          role: KVConnectorRole,
          kv_cache_config: Optional[KVCacheConfig] = None,
      ):
          super().__init__(vllm_config, role, kv_cache_config)

          # Get C++ engine instance (set by C++ daemon)
          self.cpp_engine = rdma_kv_bindings.get_global_kv_engine()

          self.is_producer = self._kv_transfer_config.is_kv_producer
          self._block_size = vllm_config.cache_config.block_size

          # Track requests
          self._requests_need_load: dict[str, Request] = {}

          print(f"[RDMAConnector] Initialized as {'PRODUCER' if self.is_producer else 'CONSUMER'}")

      # ==========================================
      # Worker-side methods
      # ==========================================

      def register_kv_caches(self, kv_caches: dict[str, torch.Tensor]):
          """
          Called once by vLLM after KV cache allocation.
          Register all KV cache GPU memory with C++ RDMA engine.
          """
          print(f"[RDMAConnector] Registering {len(kv_caches)} KV cache layers")

          for layer_name, kv_cache in kv_caches.items():
              gpu_ptr = kv_cache.data_ptr()
              size_bytes = kv_cache.numel() * kv_cache.element_size()
              shape = list(kv_cache.shape)

              # Register with C++ RDMA engine
              self.cpp_engine.register_kv_layer(
                  layer_name,
                  gpu_ptr,
                  size_bytes,
                  shape
              )

              print(f"[RDMAConnector] Registered {layer_name}: "
                    f"ptr={hex(gpu_ptr)}, size={size_bytes/(1024**2):.2f}MB, "
                    f"shape={shape}")

      def start_load_kv(self, forward_context: ForwardContext, **kwargs: Any) -> None:
          """
          Called before forward pass to start loading KV caches.
          For RDMA, actual loading happens in wait_for_layer_load().
          """
          # Nothing to do here for RDMA - we wait per-layer
          pass

      def wait_for_layer_load(self, layer_name: str) -> None:
          """
          Called from attention layer before computation.
          Blocks until RDMA transfer for this layer is complete.
          """
          if self.is_producer:
              return  # Prefill doesn't load

          metadata = self._get_connector_metadata()
          if not metadata or not metadata.requests:
              return

          request_id = metadata.requests[0]['request_id']

          # C++ blocks until RDMA transfer complete
          self.cpp_engine.wait_for_layer_load(request_id, layer_name)

      def save_kv_layer(
          self,
          layer_name: str,
          kv_layer: torch.Tensor,
          attn_metadata: AttentionMetadata,
          **kwargs: Any,
      ) -> None:
          """
          Called from attention layer after computation.
          Initiates async RDMA transfer of KV cache.
          """
          if not self.is_producer:
              return  # Decode doesn't save

          metadata = self._get_connector_metadata()
          if not metadata or not metadata.requests:
              return

          for req in metadata.requests:
              request_id = req['request_id']
              block_ids = req['block_ids']

              # Initiate async RDMA transfer
              self.cpp_engine.save_kv_layer_async(
                  request_id,
                  layer_name,
                  block_ids
              )

      def wait_for_save(self):
          """
          Called after forward pass to ensure all saves are complete.
          """
          if self.is_producer:
              self.cpp_engine.wait_for_all_saves()

      # ==========================================
      # Scheduler-side methods
      # ==========================================

      def get_num_new_matched_tokens(
          self,
          request: Request,
          num_computed_tokens: int,
      ) -> tuple[int | None, bool]:
          """
          For RDMA disaggregated prefill, decode loads all prompt tokens.
          """
          if self.is_producer:
              return 0, False

          prompt_token_ids = request.prompt_token_ids or []
          # All tokens except the last one (which decode will generate)
          num_external_tokens = len(prompt_token_ids) - 1 - num_computed_tokens

          if num_external_tokens < 0:
              num_external_tokens = 0

          return num_external_tokens, False

      def update_state_after_alloc(
          self,
          request: Request,
          blocks: KVCacheBlocks,
          num_external_tokens: int
      ):
          """Track requests that need KV loading."""
          if not self.is_producer and num_external_tokens > 0:
              self._requests_need_load[request.request_id] = request

      def build_connector_meta(
          self,
          scheduler_output: SchedulerOutput,
      ) -> KVConnectorMetadata:
          """Build metadata for worker-side connector."""
          meta = RDMAConnectorMetadata()

          for new_req in scheduler_output.scheduled_new_reqs:
              if self.is_producer:
                  # Prefill: save KV cache
                  meta.add_request(
                      request_id=new_req.req_id,
                      block_ids=new_req.block_ids[0]
                  )
              else:
                  # Decode: load KV cache
                  if new_req.req_id in self._requests_need_load:
                      meta.add_request(
                          request_id=new_req.req_id,
                          block_ids=new_req.block_ids[0]
                      )
                      self._requests_need_load.pop(new_req.req_id)

          return meta
