#pragma once
// =============================================================================
//  gpudirect_rdma.hpp
//  Types and function declarations for the GPUDirect RDMA layer.
//  Implementations are in gpudirect_rdma.cpp.
// =============================================================================

#include <infiniband/verbs.h>
#include <cuda_runtime.h>
#include <cstdint>
#include <string>

// Forward declarations (full definitions in config.hpp)
struct RdmaConfig;
struct MemoryConfig;

// ── tunables (device-independent defaults, overridden via RdmaConfig) ─────────
static constexpr int IB_PORT = 1;
static constexpr int MAX_SGE = 4;

// ── GPU memory region ──────────────────────────────────────────────────────────
struct GpuMemRegion
{
    void *d_ptr = nullptr; // GPU device pointer (HBM2)
    void *h_ptr = nullptr; // pinned host mirror (CPU accessible)
    size_t bytes = 0;
    ibv_mr *mr = nullptr; // RDMA Memory Region registered over d_ptr
    uint32_t lkey = 0;    // local key  — used in SGEs we post
    uint32_t rkey = 0;    // remote key — sent to peer via OOB
    uint64_t addr = 0;    // VA of d_ptr as uint64 — sent to peer
};

// ── QP address exchanged out-of-band ──────────────────────────────────────────
struct QpAddr
{
    uint32_t qpn;    // Queue Pair Number
    uint16_t lid;    // Local Identifier (IB fabric; 0 for RoCEv2)
    uint8_t gid[16]; // GID (always present; required for RoCEv2)
    uint32_t psn;    // Initial Packet Sequence Number
};

// ── Per-GPU RDMA context ───────────────────────────────────────────────────────
struct RdmaContext
{
    ibv_context *ctx = nullptr; // verbs device context
    ibv_pd *pd = nullptr;       // Protection Domain
    ibv_cq *send_cq = nullptr;  // send Completion Queue
    ibv_cq *recv_cq = nullptr;  // recv Completion Queue
    ibv_qp *qp = nullptr;       // Queue Pair (RC)
    ibv_port_attr port_attr{};
    ibv_device_attr dev_attr{};
    QpAddr local_addr{};  // our address (sent to peer)
    QpAddr remote_addr{}; // peer's address (received from peer)
    GpuMemRegion mem{};
    int gpu_id = 0;
};

// ── Function declarations ──────────────────────────────────────────────────────

// Bind calling thread to gpu_id; force context creation; enable P2P
void bind_cuda_device(int gpu_id);

// Open IB device by name (empty = first found), allocate PD
void open_ib_device(RdmaContext &ctx, const std::string &dev_name);

// Allocate GPU memory + pinned host mirror; register MR via GPUDirect RDMA
void alloc_and_register_gpu_mem(RdmaContext &ctx,
                                size_t bytes,
                                int gpu_id,
                                const MemoryConfig &mcfg);

// Create send/recv CQs and RC QP; drive QP RESET → INIT
void create_cqs_and_qp(RdmaContext &ctx, const RdmaConfig &rcfg);

// Drive QP INIT → RTR → RTS (requires ctx.remote_addr to be filled)
void connect_qp(RdmaContext &ctx, const RdmaConfig &rcfg);

// Post one RDMA WRITE_WITH_IMM to the send queue
void post_rdma_write(RdmaContext &ctx,
                     size_t src_offset,
                     size_t remote_offset,
                     size_t len,
                     uint64_t remote_addr,
                     uint32_t remote_rkey,
                     uint32_t imm_data,
                     bool signal);

// Busy-poll send CQ until `count` completions are reaped
void poll_send_cq(RdmaContext &ctx, int count);

// Post one zero-byte recv WR (consumes WRITE_WITH_IMM notification)
void post_recv(RdmaContext &ctx);

// Tear down all verbs and CUDA objects in correct dependency order
void destroy_rdma_context(RdmaContext &ctx);