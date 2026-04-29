# Architecture Diagrams

## Deployment / Network Topology

The cluster is composed of three logical roles connected over two distinct networks: a regular TCP/IP Ethernet fabric (control plane) and a high-speed InfiniBand fabric (data plane).

```mermaid
graph TB
    subgraph Client["Client Machine"]
        BC["benchmark_client\n(C++ TCP client)"]
    end

    subgraph Orchestrator["Orchestrator Node\n(e.g. clgpu014)"]
        OE["OrchestratorEngine\n── EpollWorker\n── WorkerPool\n── NodeRegistry\n── RequestRouter\n── RdmaExchangeTracker"]
        OTC["TCP Server\n:client_port\n(benchmark clients)"]
        OTS["TCP Server\n:server_port\n(prefill/decode nodes)"]
        OE --- OTC
        OE --- OTS
    end

    subgraph Prefill1["Prefill Node 0\n(e.g. clgpu012)"]
        PN0["Node\n── EpollWorker\n── WorkerPool\n── RDMAEngine"]
        PV0["vLLM\n(send role)\nRDMAConnector"]
        PN0 <-->|"HTTP :vllm_port\nASSIGN_REQUEST /\nPREFILL_COMPLETE"| PV0
    end

    subgraph Prefill2["Prefill Node 1\n(e.g. clgpu015)"]
        PN1["Node\n── EpollWorker\n── WorkerPool\n── RDMAEngine"]
        PV1["vLLM\n(send role)\nRDMAConnector"]
        PN1 <-->|"HTTP :vllm_port"| PV1
    end

    subgraph Decode1["Decode Node 0\n(e.g. clgpu013)"]
        DN0["Node\n── EpollWorker\n── WorkerPool\n── RDMAEngine\n── RdmaPollThread\n── RequestTrackerLayer"]
        DV0["vLLM\n(recv role)\nRDMAConnector"]
        DN0 <-->|"HTTP :vllm_port\nKV_TRANSFER_COMPLETE /\nDECODE_COMPLETE"| DV0
    end

    subgraph Decode2["Decode Node 1\n(e.g. clgpu016)"]
        DN1["Node\n── EpollWorker\n── WorkerPool\n── RDMAEngine\n── RdmaPollThread\n── RequestTrackerLayer"]
        DV1["vLLM\n(recv role)\nRDMAConnector"]
        DN1 <-->|"HTTP :vllm_port"| DV1
    end

    BC -->|"TCP (Ethernet)\nCLIENT_REQUEST"| OTC
    OTC -->|"CLIENT_RESPONSE"| BC

    OTS <-->|"TCP (Ethernet)\ncontrol messages"| PN0
    OTS <-->|"TCP (Ethernet)\ncontrol messages"| PN1
    OTS <-->|"TCP (Ethernet)\ncontrol messages"| DN0
    OTS <-->|"TCP (Ethernet)\ncontrol messages"| DN1

    PV0 -->|"GPUDirect RDMA\n(InfiniBand)\nRDMA WRITE_WITH_IMM\nKV cache tensors"| DV0
    PV0 -.->|"RDMA (IB)"| DV1
    PV1 -.->|"RDMA (IB)"| DV0
    PV1 -.->|"RDMA (IB)"| DV1

    style BC fill:#f0f4ff,stroke:#6677cc
    style OE fill:#fff8e1,stroke:#f9a825
    style PN0 fill:#e8f5e9,stroke:#388e3c
    style PN1 fill:#e8f5e9,stroke:#388e3c
    style DN0 fill:#fce4ec,stroke:#c62828
    style DN1 fill:#fce4ec,stroke:#c62828
    style PV0 fill:#c8e6c9,stroke:#388e3c
    style PV1 fill:#c8e6c9,stroke:#388e3c
    style DV0 fill:#ffcdd2,stroke:#c62828
    style DV1 fill:#ffcdd2,stroke:#c62828
```

**Network layers:**
| Layer | Protocol | Purpose |
|-------|----------|---------|
| Control plane | TCP / Ethernet | Node registration, request routing, status messages |
| Data plane | InfiniBand RDMA (`WRITE_WITH_IMM`) | KV cache tensor transfer (GPU-to-GPU, zero-copy) |

---

## Request Lifecycle (Sequence Diagram)

This diagram traces a single LLM inference request from the moment the benchmark client sends it until the generated text is returned.

```mermaid
sequenceDiagram
    autonumber
    participant C  as Benchmark Client
    participant O  as Orchestrator
    participant P  as Prefill Node (C++)
    participant Pv as Prefill vLLM
    participant D  as Decode Node (C++)
    participant Dv as Decode vLLM

    Note over O,D: Startup / RDMA Bootstrap (one-time, before requests)
    P  ->> O: RDMA_PROCESS_REGISTRATION (NodeInfo + QP info)
    D  ->> O: RDMA_PROCESS_REGISTRATION (NodeInfo + QP info)
    O  ->> P: BROADCAST_MEMBER_INFO (full cluster QP map)
    O  ->> D: BROADCAST_MEMBER_INFO (full cluster QP map)
    P  ->> P: connect_all_qps() + start vLLM
    D  ->> D: connect_all_qps() + start vLLM
    P  ->> O: RDMA_READY
    D  ->> O: RDMA_READY
    Note over O: State → RUNNING

    Note over C,Dv: Inference Request
    C  ->> O: CLIENT_REQUEST (prompt, max_tokens)
    O  ->> O: select_nodes_and_allocate_slot()\n→ prefill=P, decode=D, slot_id, slot_base_offset
    O  ->> P: ASSIGN_REQUEST (RequestInfo incl. slot_id, decode_node_id)

    Note over P,Pv: Prefill Phase
    P  ->> Pv: HTTP POST /v1/completions\n{role:"send", rdma_qp_num, rdma_remote_addr, slot_id, ...}
    Pv ->> Pv: Run transformer forward pass\n(prompt tokens → KV cache in GPU HBM)
    Pv -->> Dv: GPUDirect RDMA WRITE_WITH_IMM\n(KV cache layers, layer by layer)
    Pv -->> P: HTTP 200 (prompt_tokens count)
    P  ->> O: PREFILL_COMPLETE_ORCHESTRATOR (RequestInfo + KV metadata)

    Note over O,D: Handoff
    O  ->> D: PREFILL_COMPLETE (RequestInfo + KV metadata)
    D  ->> D: RequestTrackerLayer.add_request()\nState → WAITING_FOR_KV

    Note over Dv,D: RDMA Receive & Decode Phase
    Dv -->> D: (RDMA poll thread) KV_LAYER_ARRIVED × N layers
    D  ->> D: All layers received?\nState → DECODING
    D  ->> D: KV_TRANSFER_COMPLETE event
    D  ->> Dv: HTTP POST /v1/completions\n{role:"recv", slot_id, slot_base_offset, num_layers, ...}
    Dv ->> Dv: Inject KV cache from staging buffer\nRun decode loop (token by token)
    Dv -->> D: HTTP 200 (generated_text)
    D  ->> O: DECODE_COMPLETE (generated_text)

    Note over O,C: Response
    O  ->> O: free_slot(request_id)\ndecrement_node_load()
    O  ->> C: CLIENT_RESPONSE (generated_text)
```

**Key latency contributors** (from README performance numbers):
| Phase | Typical Duration |
|-------|-----------------|
| Orchestrator routing + slot allocation | < 1 ms |
| Prefill (vLLM forward pass) | 50–200 ms |
| RDMA KV cache transfer (GPUDirect) | 1–10 ms |
| Decode (vLLM token generation) | 500–2000 ms |
| **End-to-end** | **600–2200 ms** |
