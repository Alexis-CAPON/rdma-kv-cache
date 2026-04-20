#pragma once
#include "cpp/common/types.h"
#include "cpp/common/config.h"
#include "cpp/rdma/gpudirect_rdma.h"
#include <map>
#include <memory>
#include <vector>

/**
 * RDMAEngine - Manages GPUDirect RDMA for KV cache transfer
 *
 * Integrates the GPUDirect RDMA layer with the node infrastructure:
 * - Device discovery and GPU↔HCA binding
 * - GPU memory registration
 * - QP creation and connection management (one QP per peer)
 * - KV cache transfer via RDMA WRITE_WITH_IMM
 * - Integration with epoll event loop for completion polling
 */
class RDMAEngine
{
public:
    RDMAEngine(Config &config, NodeInfo &node_info);
    ~RDMAEngine();

    // Non-copyable
    RDMAEngine(const RDMAEngine &) = delete;
    RDMAEngine &operator=(const RDMAEngine &) = delete;

    // ========================================
    // Lifecycle
    // ========================================

    /**
     * Initialize RDMA engine:
     * 1. Probe and bind GPU to HCA
     * 2. Allocate and register GPU memory
     * 3. Create one QP for each expected peer
     * 4. Fill local QP info into NodeInfo.qmaps
     */
    bool initialize();

    /**
     * Connect QPs to all peers after QP exchange
     * Called by Node after orchestrator broadcasts peer info
     */
    bool connect_all_qps();

    /**
     * Shutdown and cleanup all RDMA resources
     */
    void shutdown();

    /**
     * Check if RDMA engine is initialized
     */
    bool is_initialized() const { return initialized_; }

    const NodeInfo &get_node_info() const { return node_info_; }

    // ========================================
    // Data Transfer
    // ========================================

    /**
     * Transfer KV cache to a specific peer via RDMA WRITE_WITH_IMM
     * @param peer_id Target peer node ID
     * @param src_offset Offset in local GPU buffer
     * @param dst_offset Offset in remote GPU buffer
     * @param length Transfer size in bytes
     * @param imm_data Immediate data (e.g., chunk ID)
     * @param signal Whether to signal completion
     */
    bool post_write(const std::string &peer_id,
                    size_t src_offset,
                    size_t dst_offset,
                    size_t length,
                    uint32_t imm_data,
                    bool signal);

    bool post_write_external(
        const std::string &peer_id,
        uint64_t local_addr, // Absolute address (vLLM memory)
        uint32_t local_lkey, // lkey from vLLM's MR
        size_t dst_offset,
        size_t length,
        uint32_t imm_data,
        bool signal);

    /**
     * Post receive WR for incoming WRITE_WITH_IMM
     * @param count Number of recv WRs to post
     */
    bool post_recv_wrs(int count);

    /**
     * Poll send CQ for completions
     * @param count Expected number of completions
     * @return Number of completions reaped
     */
    int poll_send_cq(int count);

    /**
     * Poll recv CQ for completions
     * @param wcs Output vector for work completions
     * @param max_count Maximum completions to reap
     * @return Number of completions reaped
     */
    int poll_recv_cq(std::vector<ibv_wc> &wcs, int max_count);

    // ========================================
    // Accessors
    // ========================================

    /**
     * Get GPU device pointer for KV cache
     */
    void *get_gpu_ptr() const { return rdma_ctx_.mem.d_ptr; }

    /**
     * Get pinned host pointer for KV cache
     */
    void *get_host_ptr() const { return rdma_ctx_.mem.h_ptr; }

    /**
     * Get GPU memory region size
     */
    size_t get_memory_size() const { return rdma_ctx_.mem.bytes; }

    /**
     * Get send CQ file descriptor for epoll integration
     */
    int get_send_cq_fd() const;

    /**
     * Get recv CQ file descriptor for epoll integration
     */
    int get_recv_cq_fd() const;

    /**
     * Get RDMA context (for Python bindings memory registration)
     */
    RdmaContext &get_context() { return rdma_ctx_; }
    const RdmaContext &get_context() const { return rdma_ctx_; }

private:
    // Configuration
    Config &config_;
    NodeInfo &node_info_;
    std::atomic<bool> initialized_;

    // ── GPUDirect RDMA Context ───────────────────────────────────────────────
    RdmaContext rdma_ctx_; // Main RDMA context for this node's GPU

    // ── Per-Peer QP Management ────────────────────────────────────────────────
    // Map from peer_node_id to QP
    // For single GPU per node: we create one QP per peer
    std::map<std::string, ibv_qp *> peer_qps_;

    // ========================================
    // Internal Helpers
    // ========================================

    /**
     * Probe GPU and HCA, perform NUMA-aware binding
     * Fills node_info_ with device info
     */
    bool probe_and_bind_devices();

    /**
     * Bind CUDA device and enable P2P
     */
    bool bind_cuda_device();

    /**
     * Open IB device and allocate Protection Domain
     */
    bool open_ib_device();

    /**
     * Allocate GPU memory and register MR
     */
    bool alloc_and_register_gpu_memory();

    /**
     * Create CQs (send and recv)
     */
    bool create_cqs();

    /**
     * Create one QP for a specific peer
     * @param peer_id Peer node ID
     * @return Created QP pointer (owned by peer_qps_ map)
     */
    ibv_qp *create_qp_for_peer(const std::string &peer_id);

    /**
     * Connect a QP to its remote peer (INIT → RTR → RTS)
     * @param peer_id Peer node ID
     * @param qp Queue pair to connect
     */
    bool connect_qp_to_peer(const std::string &peer_id, ibv_qp *qp);

    /**
     * Find peer info in node_info_.qmaps
     */
    PeerInfoandQPs *find_peer_info(const std::string &peer_id);
};
