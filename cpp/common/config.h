#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct PeerConfig
{
    std::string node_id;
    std::string hostname;
    uint32_t client_socket_port;
    uint32_t server_socket_port;
};

// ── GPUDirect RDMA Configuration ─────────────────────────────────────────────
struct RdmaConfig
{
    // IB device selection
    std::vector<std::string> ib_devices; // Empty = auto-select first active
    int ib_port = 1;                     // Physical port (usually 1)
    int gid_index = 0;                   // 0=IB, 3=RoCEv2

    // QP parameters
    int mtu = 0;                 // Path MTU: 0=auto-discover from port, or override with 256/512/1024/2048/4096
    int sl = 0;                  // Service Level
    int qp_max_send_wr = 64;     // Max outstanding send WRs
    int qp_max_recv_wr = 64;     // Max outstanding recv WRs
    int qp_max_inline_data = 64; // Inline data threshold
    int max_rd_atomic = 16;      // Max RDMA read/atomic ops
    int min_rnr_timer = 12;      // RNR retry timer (~0.64ms)
    int timeout = 14;            // ACK timeout (~67ms)
    int retry_cnt = 7;           // Retry count before QP error
    int rnr_retry = 7;           // RNR retry (7 = infinite)

    // CQ depth
    int cq_depth = 128;
};

struct MemoryConfig
{
    size_t kv_buffer_mb = 16384; // GPU memory buffer size in MB (total staging buffer)
    size_t required_mb = 0;      // Minimum required buffer (computed in Config::fromFile)
    size_t layer_size_mb = 128;      // Per-layer KV cache size in MB (128MB for typical LLMs)
    int num_kv_chunks = 16;          // Number of chunks for pipelined transfer
    size_t chunk_size_mb = 64;       // Size of each chunk in MB (64-128 recommended)
    bool mr_relaxed_ordering = true; // Enable PCIe relaxed ordering
    int max_concurrent_requests = 8; // Max simultaneous KV transfers
};

struct GpuConfig
{
    std::vector<int> gpu_ids;       // Empty = use all available GPUs
    bool enable_peer_access = true; // Enable P2P between GPUs
    bool require_same_numa = false; // Require GPU and HCA on same NUMA node
};

// ──────────────────────────────────────────────────────────────────────────────

class Config
{
public:
    static std::string generate_node_id(const std::string &host, uint16_t port);
    std::string node_id;
    uint32_t client_socket_port;
    uint32_t server_socket_port;
    std::string hostname;
    uint32_t replication_factor;
    uint32_t vnodes_number;
    uint32_t number_of_keys_hashtable;
    size_t size_local_buffer;
    size_t client_event_queue_size;
    uint32_t worker_pool_size;
    std::string orchestrator_host;
    uint32_t orchestrator_port;
    std::string orchestrator_id;
    uint32_t vllm_port;
    std::string model_name;

    uint32_t num_layers;

    std::string role;

    uint32_t orchestrator_event_queue_size;

    int expected_decode_nodes;
    int expected_prefill_nodes;

    std::vector<PeerConfig> prefill_nodes;
    std::vector<PeerConfig> decode_nodes;

    std::vector<PeerConfig> peers;

    // ── Transfer backend selection ────────────────────────────────────────────
    // true  → GPUDirect RDMA (RDMAConnector, requires nvidia-peermem + A100-class GPU)
    // false → Standard RDMA via MooncakeConnector (no GPU required, works on CPU-only servers)
    bool use_gpu = true;

    // ── GPUDirect RDMA Configuration ─────────────────────────────────────────
    RdmaConfig rdma;
    MemoryConfig memory;
    GpuConfig gpu;

    static Config fromFile(const std::string &path);
};