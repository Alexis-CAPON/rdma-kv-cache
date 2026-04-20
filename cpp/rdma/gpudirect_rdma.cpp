// =============================================================================
//  gpudirect_rdma.cpp
//  Full implementation of every function declared in gpudirect_rdma.hpp.
//
//  Covers:
//    - CUDA device binding
//    - IB device open + Protection Domain
//    - GPU memory allocation + MR registration (GPUDirect RDMA path)
//    - CQ + RC Queue Pair creation
//    - QP state machine:  RESET → INIT → RTR → RTS
//    - RDMA WRITE_WITH_IMM posting
//    - Send / Recv CQ polling
//    - Graceful teardown
// =============================================================================

#include "cpp/rdma/gpudirect_rdma.h"
#include "cpp/common/config.h"
#include <infiniband/verbs.h>
#include <cuda_runtime.h>
#include "cpp/common/logger.h"

#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>
#include <iomanip>
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

    // Format errno into a readable string
    static std::string errno_str(int e)
    {
        char buf[128] = {};
        // GNU strerror_r returns char*
        return std::string(strerror_r(e, buf, sizeof(buf)));
    }

    // Check CUDA return code and throw on error
    static void cuda_must(cudaError_t e, const char *where)
    {
        if (e != cudaSuccess)
            throw std::runtime_error(
                std::string(where) + ": " + cudaGetErrorString(e));
    }

    // Convert config MTU int to ibv_mtu enum
    static ibv_mtu mtu_to_enum(int mtu)
    {
        switch (mtu)
        {
        case 512:
            return IBV_MTU_512;
        case 1024:
            return IBV_MTU_1024;
        case 2048:
            return IBV_MTU_2048;
        case 4096:
            return IBV_MTU_4096;
        default:
            throw std::runtime_error(
                "Invalid MTU value: " + std::to_string(mtu));
        }
    }

    // Hexdump a GID for logging
    static std::string fmt_gid(const uint8_t gid[16])
    {
        std::ostringstream oss;
        for (int i = 0; i < 16; ++i)
        {
            if (i && (i % 2 == 0))
                oss << ":";
            oss << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(gid[i]);
        }
        return oss.str();
    }

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
//  bind_cuda_device
//
//  Sets the calling thread's active CUDA device and forces context creation.
//  Also enables intra-node peer access between this GPU and every other GPU
//  that is accessible (P2P over NVLink or PCIe).
//
//  Why force context creation here?
//  CUDA uses lazy initialisation: the actual GPU context is not created until
//  the first CUDA API call that requires it.  If we let that happen later
//  (e.g. inside cudaMalloc), the context setup adds ~100 ms of latency right
//  in the middle of the RDMA initialisation sequence.  Forcing it here keeps
//  the MR-registration path clean and predictable.
// ─────────────────────────────────────────────────────────────────────────────

void bind_cuda_device(int gpu_id)
{
    cuda_must(cudaSetDevice(gpu_id), "cudaSetDevice");

    // Force context creation by issuing a lightweight API call that requires
    // the context to exist.
    cudaFree(nullptr); // no-op allocation, but context must be live

    // Query total GPU count to know the P2P candidate set
    int total = 0;
    cuda_must(cudaGetDeviceCount(&total), "cudaGetDeviceCount");

    for (int peer = 0; peer < total; ++peer)
    {
        if (peer == gpu_id)
            continue;

        int can_access = 0;
        cudaDeviceCanAccessPeer(&can_access, gpu_id, peer);
        if (!can_access)
        {
            std::cout << "[CUDA] GPU" << gpu_id << " cannot P2P access GPU"
                      << peer << " — skipped\n";
            continue;
        }

        cudaError_t e = cudaDeviceEnablePeerAccess(peer, 0);
        if (e == cudaErrorPeerAccessAlreadyEnabled)
        {
            // Another thread already enabled it — not an error
        }
        else if (e != cudaSuccess)
        {
            std::cerr << "[CUDA] WARNING: GPU" << gpu_id
                      << " EnablePeerAccess(GPU" << peer
                      << ") failed: " << cudaGetErrorString(e) << "\n";
        }
        else
        {
            std::cout << "[CUDA] P2P enabled: GPU" << gpu_id
                      << " → GPU" << peer << "\n";
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  open_ib_device
//
//  Opens an IB verbs context by device name, queries device and port
//  attributes, verifies the port is ACTIVE, and allocates a Protection Domain.
//
//  Protection Domain (PD):
//  Every verbs object that participates in data transfer (MR, QP, AH) must
//  belong to exactly one PD.  The HCA enforces that a QP can only reference
//  MRs in the same PD, preventing one process from using another's rkeys.
//  We create one PD per GPU worker — one PD per RdmaContext.
// ─────────────────────────────────────────────────────────────────────────────

void open_ib_device(RdmaContext &ctx, const std::string &dev_name)
{
    int num_devs = 0;
    ibv_device **dev_list = ibv_get_device_list(&num_devs);
    if (!dev_list || num_devs == 0)
        throw std::runtime_error(
            "[IB] ibv_get_device_list returned no devices. "
            "Is the rdma-core stack installed? (ibv_devinfo)");

    ibv_device *chosen = nullptr;
    for (int i = 0; i < num_devs; ++i)
    {
        if (dev_name.empty() ||
            dev_name == ibv_get_device_name(dev_list[i]))
        {
            chosen = dev_list[i];
            break;
        }
    }

    ibv_free_device_list(dev_list); // free before throwing — no leak

    if (!chosen)
        throw std::runtime_error(
            "[IB] Device not found: '" + dev_name + "'. "
                                                    "Run ibv_devinfo to list available devices.");

    ctx.ctx = ibv_open_device(chosen);
    if (!ctx.ctx)
        throw std::runtime_error(
            "[IB] ibv_open_device failed for " + dev_name +
            " (errno=" + errno_str(errno) + ")");

    // Device-level capabilities — used later to cap CQ/QP sizes
    if (ibv_query_device(ctx.ctx, &ctx.dev_attr))
    {
        ibv_close_device(ctx.ctx);
        ctx.ctx = nullptr;
        throw std::runtime_error("[IB] ibv_query_device failed");
    }

    // Port-level state — must be ACTIVE before we create any QP
    if (ibv_query_port(ctx.ctx, IB_PORT, &ctx.port_attr))
    {
        ibv_close_device(ctx.ctx);
        ctx.ctx = nullptr;
        throw std::runtime_error("[IB] ibv_query_port failed");
    }

    if (ctx.port_attr.state != IBV_PORT_ACTIVE)
    {
        ibv_close_device(ctx.ctx);
        ctx.ctx = nullptr;
        throw std::runtime_error(
            "[IB] Port " + std::to_string(IB_PORT) +
            " on " + dev_name + " is not ACTIVE "
                                "(state=" +
            std::to_string(ctx.port_attr.state) + "). "
                                                  "Check: ibstat");
    }

    // Protection Domain
    ctx.pd = ibv_alloc_pd(ctx.ctx);
    if (!ctx.pd)
    {
        ibv_close_device(ctx.ctx);
        ctx.ctx = nullptr;
        throw std::runtime_error("[IB] ibv_alloc_pd failed");
    }

    std::cout << "[IB] Opened " << dev_name
              << "  port=" << IB_PORT
              << "  LID=0x" << std::hex << ctx.port_attr.lid << std::dec
              << "  max_mr=" << ctx.dev_attr.max_mr
              << "  max_qp=" << ctx.dev_attr.max_qp
              << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  alloc_and_register_gpu_mem
//
//  Allocates a buffer in GPU HBM2 with cudaMalloc and registers it as an RDMA
//  Memory Region via ibv_reg_mr.
//
//  The GPUDirect RDMA path through ibv_reg_mr:
//
//    1. libibverbs calls into the mlx5 provider (libibverbs/providers/mlx5).
//    2. The provider calls the kernel verb (via /dev/infiniband/uverbs0).
//    3. ib_core checks whether the VA range is "peer memory" by calling every
//       registered peer_memory_client — nvidia_peermem registers one.
//    4. nvidia_peermem calls into the NVIDIA kernel driver, which pins the
//       physical HBM2 pages backing ctx.mem.d_ptr and returns their physical
//       addresses to ib_core.
//    5. ib_core programs the HCA's MPT (Memory Protection Table) with those
//       physical addresses.  The HCA can now DMA to/from GPU memory.
//    6. ibv_reg_mr returns an ibv_mr* with valid lkey and rkey.
//
//  From this point the verbs layer treats the GPU MR identically to a host MR.
//  No special opcode, no special flag at post_send time.
//
//  IBV_ACCESS_RELAXED_ORDERING:
//  On PCIe the NIC issues memory-write transactions that can arrive out of
//  order relative to each other.  For host memory, the CPU's cache coherence
//  protocol resolves this transparently.  GPU memory has NO cache coherence
//  with the CPU — the GPU's memory controller sees raw PCIe writes.  Without
//  RELAXED_ORDERING the HCA inserts PCIe fences between every write
//  transaction, serialising them and costing ~30% bandwidth.  With it, the
//  HCA can pipeline writes and lets the GPU's memory controller sort out
//  ordering.  This flag requires OFED >= 5.0 and Linux kernel >= 5.2.
// ─────────────────────────────────────────────────────────────────────────────

void alloc_and_register_gpu_mem(RdmaContext &ctx,
                                size_t bytes,
                                int gpu_id,
                                const MemoryConfig &mcfg)
{
    ctx.gpu_id = gpu_id;
    ctx.mem.bytes = bytes;

    cuda_must(cudaSetDevice(gpu_id), "cudaSetDevice (alloc)");

    // ── GPU device memory ────────────────────────────────────────────────────
    //
    //  cudaMalloc returns a device VA that is valid only within the GPU's
    //  address space.  nvidia_peermem maps it to physical HBM2 pages for the
    //  HCA.  The alignment is guaranteed to be at least 256 bytes, which
    //  satisfies the HCA's MR alignment requirement.
    cuda_must(cudaMalloc(&ctx.mem.d_ptr, bytes), "cudaMalloc");

    // ── Optional pinned host mirror ──────────────────────────────────────────
    //
    //  Used for CPU-side buffer preparation (e.g. packing prompt tokens before
    //  the first prefill kernel).  cudaHostAllocMapped gives a host VA that
    //  maps to the same physical pages as the host allocation — useful for
    //  zero-copy access from the CPU side.  This is a HOST allocation, not
    //  GPUDirect; it does not replace the GPU MR.
    cuda_must(cudaHostAlloc(&ctx.mem.h_ptr, bytes,
                            cudaHostAllocPortable | cudaHostAllocMapped),
              "cudaHostAlloc");

    // ── ibv_reg_mr — the GPUDirect RDMA registration call ───────────────────
    int mr_flags = IBV_ACCESS_LOCAL_WRITE |
                   IBV_ACCESS_REMOTE_WRITE |
                   IBV_ACCESS_REMOTE_READ;

    if (mcfg.mr_relaxed_ordering)
    {
        // Cast needed: IBV_ACCESS_RELAXED_ORDERING may not be defined in older
        // OFED headers.  Check with: grep RELAXED /usr/include/infiniband/verbs.h
#ifdef IBV_ACCESS_RELAXED_ORDERING
        mr_flags |= IBV_ACCESS_RELAXED_ORDERING;
#else
        std::cerr << "[MR] WARNING: IBV_ACCESS_RELAXED_ORDERING not available "
                     "in this OFED version — skipping\n";
#endif
    }

    ctx.mem.mr = ibv_reg_mr(ctx.pd, ctx.mem.d_ptr, bytes, mr_flags);

    if (!ctx.mem.mr)
    {
        // Diagnose the most common failure modes
        std::string reason;
        switch (errno)
        {
        case ENOTSUP:
            reason = "ENOTSUP — nvidia_peermem/nv_peer_mem not loaded, "
                     "or OFED does not support peer memory. "
                     "Run: sudo modprobe nvidia_peermem";
            break;
        case ENOMEM:
            reason = "ENOMEM — pinned memory quota exceeded. "
                     "Run: ulimit -l unlimited  (and set in limits.conf)";
            break;
        case EPERM:
            reason = "EPERM — insufficient privileges for MR registration";
            break;
        case EINVAL:
            reason = "EINVAL — address or length not aligned, "
                     "or flags combination unsupported";
            break;
        default:
            reason = errno_str(errno);
        }

        // Clean up the GPU allocation before throwing
        cudaFree(ctx.mem.d_ptr);
        ctx.mem.d_ptr = nullptr;
        cudaFreeHost(ctx.mem.h_ptr);
        ctx.mem.h_ptr = nullptr;

        throw std::runtime_error(
            "[MR] ibv_reg_mr failed for GPU" + std::to_string(gpu_id) +
            " — " + reason);
    }

    ctx.mem.lkey = ctx.mem.mr->lkey;
    ctx.mem.rkey = ctx.mem.mr->rkey;
    ctx.mem.addr = reinterpret_cast<uint64_t>(ctx.mem.d_ptr);

    // Change with logger debug
    Logger::debug("[MR] GPU" + std::to_string(gpu_id) +
                  "  size=" + std::to_string(bytes / (1 << 20)) + " MB" +
                  "  lkey=0x" + std::hex + std::to_string(ctx.mem.lkey) +
                  "  rkey=0x" + std::hex + std::to_string(ctx.mem.rkey) +
                  "  d_ptr=0x" + std::hex + std::to_string(ctx.mem.addr) +
                  std::dec);
}

// ─────────────────────────────────────────────────────────────────────────────
//  create_cqs_and_qp
//
//  Creates two Completion Queues (one for send, one for receive) and one
//  Reliable Connected Queue Pair.  Drives the QP from RESET to INIT.
//
//  Why separate send and recv CQs?
//  A single CQ would work, but mixing send and recv completions on one queue
//  complicates polling: you'd need to inspect wc.opcode on every poll to decide
//  whether it's a send completion (prefill side cares) or a recv completion
//  (decode side cares).  Separate CQs let each side poll exactly the queue it
//  owns with no filtering.
//
//  Why RC (Reliable Connected)?
//  RC provides in-order, lossless delivery with hardware retransmit.  For
//  KV-cache transfer correctness is non-negotiable — a silently dropped or
//  reordered chunk would corrupt the decode attention computation.  UC
//  (Unreliable Connected) would be slightly faster but would require
//  application-level sequence numbering and retransmit logic.  UD
//  (Unreliable Datagram) adds MTU-sized fragmentation overhead on top.
//
//  sq_sig_all = 0:
//  By default every posted WR generates a CQ entry.  With sq_sig_all=0 only
//  WRs explicitly flagged IBV_SEND_SIGNALED produce a completion.  For bulk
//  KV-cache transfer we signal only the last WR in a batch, collapsing N polls
//  into 1 and reducing CQ overhead by ~(N-1)/N.
// ─────────────────────────────────────────────────────────────────────────────

void create_cqs_and_qp(RdmaContext &ctx, const RdmaConfig &rcfg)
{

    // ── Completion Queues ────────────────────────────────────────────────────
    //
    //  cq_depth is capped at the device maximum to avoid EINVAL.
    int cq_depth = std::min(rcfg.cq_depth, ctx.dev_attr.max_cqe);

    ctx.send_cq = ibv_create_cq(ctx.ctx, cq_depth,
                                nullptr, // no user context
                                nullptr, // no completion channel (polling mode)
                                0);
    if (!ctx.send_cq)
        throw std::runtime_error(
            "[QP] ibv_create_cq (send) failed — errno=" +
            errno_str(errno));

    ctx.recv_cq = ibv_create_cq(ctx.ctx, cq_depth,
                                nullptr, nullptr, 0);
    if (!ctx.recv_cq)
    {
        ibv_destroy_cq(ctx.send_cq);
        ctx.send_cq = nullptr;
        throw std::runtime_error(
            "[QP] ibv_create_cq (recv) failed — errno=" +
            errno_str(errno));
    }

    // ── Queue Pair init attributes ───────────────────────────────────────────
    //
    //  max_send_wr / max_recv_wr are capped at device limits.
    //  The device may round up to the next hardware alignment — that is normal.
    ibv_qp_init_attr init_attr{};
    init_attr.send_cq = ctx.send_cq;
    init_attr.recv_cq = ctx.recv_cq;
    init_attr.qp_type = IBV_QPT_RC;
    init_attr.sq_sig_all = 0; // only signal flagged WRs (see above)
    init_attr.cap.max_send_wr =
        std::min(rcfg.qp_max_send_wr, ctx.dev_attr.max_qp_wr);
    init_attr.cap.max_recv_wr =
        std::min(rcfg.qp_max_recv_wr, ctx.dev_attr.max_qp_wr);
    init_attr.cap.max_send_sge =
        std::min(MAX_SGE, ctx.dev_attr.max_sge);
    init_attr.cap.max_recv_sge =
        std::min(MAX_SGE, ctx.dev_attr.max_sge);
    init_attr.cap.max_inline_data =
        static_cast<uint32_t>(rcfg.qp_max_inline_data);

    ctx.qp = ibv_create_qp(ctx.pd, &init_attr);
    if (!ctx.qp)
    {
        ibv_destroy_cq(ctx.send_cq);
        ctx.send_cq = nullptr;
        ibv_destroy_cq(ctx.recv_cq);
        ctx.recv_cq = nullptr;
        throw std::runtime_error(
            "[QP] ibv_create_qp failed — errno=" + errno_str(errno) +
            ".  Check device QP/CQ limits with ibv_devinfo.");
    }

    // ── RESET → INIT ─────────────────────────────────────────────────────────
    //
    //  In INIT the QP is allocated but cannot send or receive yet.
    //  The required attributes at this transition are:
    //    - qp_state        : move to INIT
    //    - pkey_index      : partition key index (0 for default partition)
    //    - port_num        : physical IB port
    //    - qp_access_flags : what remote peers are allowed to do to this QP
    ibv_qp_attr attr{};
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = static_cast<uint8_t>(rcfg.ib_port);
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                           IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_READ;

    int mask = IBV_QP_STATE |
               IBV_QP_PKEY_INDEX |
               IBV_QP_PORT |
               IBV_QP_ACCESS_FLAGS;

    if (ibv_modify_qp(ctx.qp, &attr, mask))
    {
        ibv_destroy_qp(ctx.qp);
        ctx.qp = nullptr;
        ibv_destroy_cq(ctx.send_cq);
        ctx.send_cq = nullptr;
        ibv_destroy_cq(ctx.recv_cq);
        ctx.recv_cq = nullptr;
        throw std::runtime_error("[QP] RESET→INIT failed — errno=" +
                                 errno_str(errno));
    }

    // ── Capture local QP address ─────────────────────────────────────────────
    //
    //  QPN and LID are stable once the QP exists.  PSN is a random 24-bit
    //  value — randomising it prevents stale packets from a previous QP
    //  lifetime (same QPN, different process run) being accepted.
    ctx.local_addr.qpn = ctx.qp->qp_num;
    ctx.local_addr.lid = ctx.port_attr.lid;
    ctx.local_addr.psn = static_cast<uint32_t>(lrand48()) & 0x00FFFFFF;

    // GID — required for RoCEv2 (Ethernet fabric); also valid for IB.
    ibv_gid gid{};
    if (ibv_query_gid(ctx.ctx, rcfg.ib_port, rcfg.gid_index, &gid))
        throw std::runtime_error(
            "[QP] ibv_query_gid failed for GID index " +
            std::to_string(rcfg.gid_index) +
            ".  For RoCEv2 use index 3; for IB use index 0.");
    std::memcpy(ctx.local_addr.gid, gid.raw, 16);

    std::cout << "[QP] Created RC QP"
              << "  QPN=0x" << std::hex << ctx.local_addr.qpn
              << "  LID=0x" << ctx.local_addr.lid
              << std::dec
              << "  PSN=" << ctx.local_addr.psn
              << "  GID=" << fmt_gid(ctx.local_addr.gid)
              << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  connect_qp
//
//  Drives the QP from INIT → RTR → RTS using the remote peer's address
//  (filled into ctx.remote_addr by the OOB exchange in main.cpp).
//
//  INIT → RTR (Ready To Receive):
//  The QP learns the remote endpoint's address: QPN, GID/LID, PSN.
//  At RTR the QP can receive incoming RDMA WRITEs, but cannot send yet.
//  We must reach RTR before the remote side posts its first WRITE.
//
//  RTR → RTS (Ready To Send):
//  Configures retransmit timers and enables the send queue.
//  After this transition both sides can post send WRs.
//
//  Key parameters explained:
//
//  path_mtu — must match or be <= the fabric MTU.  Larger MTU means fewer
//  packets per RDMA WRITE, less header overhead, more bandwidth efficiency.
//  4096 is the maximum for EDR InfiniBand.
//
//  max_dest_rd_atomic / max_rd_atomic — maximum outstanding RDMA READ or
//  atomic operations (from the remote side / from this side).  We set 16 for
//  both to allow pipelining of reads if needed in future; for pure WRITE
//  workloads this is unused but harmless.
//
//  min_rnr_timer — if the receive queue is empty when a message arrives, the
//  HCA sends a RNR (Receiver Not Ready) NAK and waits this long before
//  retransmitting.  12 ≈ 0.64 ms.  Too small and you flood the fabric with
//  retransmits; too large and you add unnecessary latency.
//
//  timeout — local ACK timeout for the send queue.  14 ≈ 67 ms.  If no ACK
//  arrives within this window, the HCA retransmits.  retry_cnt=7 means up to
//  7 retransmits before the QP enters error state.
//
//  ah_attr.is_global = 1 — always set for RoCEv2 (the GRH is the routing
//  header on Ethernet).  For native IB over IB fabric you can use is_global=0
//  and fill only the LID; setting is_global=1 with a valid GID works for both.
// ─────────────────────────────────────────────────────────────────────────────

void connect_qp(RdmaContext &ctx, const RdmaConfig &rcfg)
{

    // ── INIT → RTR ───────────────────────────────────────────────────────────
    {
        ibv_qp_attr attr{};
        attr.qp_state = IBV_QPS_RTR;
        attr.path_mtu = mtu_to_enum(rcfg.mtu);
        attr.dest_qp_num = ctx.remote_addr.qpn;
        attr.rq_psn = ctx.remote_addr.psn;
        attr.max_dest_rd_atomic = static_cast<uint8_t>(rcfg.max_rd_atomic);
        attr.min_rnr_timer = static_cast<uint8_t>(rcfg.min_rnr_timer);

        // Address Handle attributes — describes the path to the remote port
        attr.ah_attr.is_global = 1;
        attr.ah_attr.port_num = static_cast<uint8_t>(rcfg.ib_port);
        attr.ah_attr.sl = static_cast<uint8_t>(rcfg.sl);
        attr.ah_attr.src_path_bits = 0;

        // GRH (Global Routing Header) — used for RoCEv2 and for IB with GIDs
        std::memcpy(attr.ah_attr.grh.dgid.raw,
                    ctx.remote_addr.gid, 16);
        attr.ah_attr.grh.sgid_index = static_cast<uint8_t>(rcfg.gid_index);
        attr.ah_attr.grh.hop_limit = 64;
        attr.ah_attr.grh.traffic_class = 0;
        attr.ah_attr.dlid = ctx.remote_addr.lid;

        int mask = IBV_QP_STATE |
                   IBV_QP_AV |
                   IBV_QP_PATH_MTU |
                   IBV_QP_DEST_QPN |
                   IBV_QP_RQ_PSN |
                   IBV_QP_MAX_DEST_RD_ATOMIC |
                   IBV_QP_MIN_RNR_TIMER;

        if (ibv_modify_qp(ctx.qp, &attr, mask))
            throw std::runtime_error(
                "[QP] INIT→RTR failed — errno=" + errno_str(errno) +
                ".  Common causes: remote QPN/GID wrong, MTU mismatch, "
                "pkey mismatch.");

        std::cout << "[QP] INIT→RTR  remote_QPN=0x"
                  << std::hex << ctx.remote_addr.qpn
                  << "  remote_GID=" << fmt_gid(ctx.remote_addr.gid)
                  << std::dec << "\n";
    }

    // ── RTR → RTS ────────────────────────────────────────────────────────────
    {
        ibv_qp_attr attr{};
        attr.qp_state = IBV_QPS_RTS;
        attr.sq_psn = ctx.local_addr.psn;
        attr.timeout = static_cast<uint8_t>(rcfg.timeout);
        attr.retry_cnt = static_cast<uint8_t>(rcfg.retry_cnt);
        attr.rnr_retry = static_cast<uint8_t>(rcfg.rnr_retry);
        attr.max_rd_atomic = static_cast<uint8_t>(rcfg.max_rd_atomic);

        int mask = IBV_QP_STATE |
                   IBV_QP_SQ_PSN |
                   IBV_QP_TIMEOUT |
                   IBV_QP_RETRY_CNT |
                   IBV_QP_RNR_RETRY |
                   IBV_QP_MAX_QP_RD_ATOMIC;

        if (ibv_modify_qp(ctx.qp, &attr, mask))
            throw std::runtime_error(
                "[QP] RTR→RTS failed — errno=" + errno_str(errno));

        std::cout << "[QP] RTR→RTS  sq_psn=" << ctx.local_addr.psn
                  << "  timeout=" << rcfg.timeout
                  << "  retry=" << rcfg.retry_cnt << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  post_rdma_write
//
//  Posts one RDMA WRITE_WITH_IMM work request to the send queue.
//
//  RDMA WRITE_WITH_IMM vs plain RDMA WRITE:
//  A plain RDMA WRITE is fully one-sided: the remote CPU sees the data appear
//  in its MR but gets no notification — it has to poll the buffer itself.
//  WRITE_WITH_IMM additionally posts a completion to the *remote* recv CQ,
//  consuming one pre-posted recv WR.  This is what lets the decode node know
//  "chunk N has arrived" without a separate send/recv message pair.
//  The immediate value (32-bit, network byte order) is delivered in the
//  recv WC's imm_data field.
//
//  Scatter-Gather Element (SGE):
//  One SGE = one contiguous region.  addr is the source VA in the local MR,
//  length is the byte count, lkey authorises the HCA to read from it.
//  For KV-cache chunks we always use a single SGE.  For future scatter
//  (e.g. key tensor + value tensor in separate allocations) you would supply
//  two SGEs and the HCA concatenates them into a single wire transfer.
//
//  IBV_SEND_SIGNALED:
//  When set, a CQ entry is generated when the WRITE is acknowledged by the
//  remote HCA.  We set this only on the last chunk in a batch so we get
//  exactly one poll per batch regardless of how many chunks are in flight.
// ─────────────────────────────────────────────────────────────────────────────

void post_rdma_write(RdmaContext &ctx,
                     size_t src_offset,
                     size_t remote_offset,
                     size_t len,
                     uint64_t remote_addr,
                     uint32_t remote_rkey,
                     uint32_t imm_data,
                     bool signal)
{
    if (len == 0)
        throw std::invalid_argument("[WRITE] len must be > 0");
    if (src_offset + len > ctx.mem.bytes)
        throw std::invalid_argument(
            "[WRITE] src_offset + len exceeds registered MR size");

    ibv_sge sge{};
    sge.addr = ctx.mem.addr + src_offset;
    sge.length = static_cast<uint32_t>(len);
    sge.lkey = ctx.mem.lkey;

    ibv_send_wr wr{};
    wr.wr_id = static_cast<uint64_t>(imm_data);
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags = signal ? IBV_SEND_SIGNALED : 0;
    wr.imm_data = htonl(imm_data); // always network byte order
    wr.wr.rdma.remote_addr = remote_addr + remote_offset;
    wr.wr.rdma.rkey = remote_rkey;

    ibv_send_wr *bad_wr = nullptr;
    int rc = ibv_post_send(ctx.qp, &wr, &bad_wr);
    if (rc != 0)
        throw std::runtime_error(
            "[WRITE] ibv_post_send failed — rc=" + std::to_string(rc) +
            " errno=" + errno_str(errno) +
            ".  Check: QP in RTS state, send queue not full, "
            "remote_addr/rkey correct.");

    std::cout << "[WRITE] Posted chunk imm=" << imm_data
              << "  src_off=" << src_offset / (1 << 20) << " MB"
              << "  dst=0x" << std::hex << (remote_addr + remote_offset)
              << std::dec
              << "  len=" << len / (1 << 20) << " MB"
              << (signal ? "  [SIGNALED]" : "") << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  poll_send_cq
//
//  Busy-polls the send CQ until `count` successful completions are reaped.
//  Any error status in a WC causes an immediate throw.
//
//  Why busy-poll instead of a completion channel (event-driven)?
//  For latency-sensitive KV-cache transfers the sleep/wake overhead of an
//  event fd adds tens of microseconds.  Busy-polling burns one CPU core but
//  keeps the critical path as short as possible.  If you want to trade
//  latency for CPU efficiency, replace this with ibv_get_cq_event() on a
//  completion channel.
// ─────────────────────────────────────────────────────────────────────────────

void poll_send_cq(RdmaContext &ctx, int count)
{
    std::vector<ibv_wc> wcs(static_cast<size_t>(count));
    int reaped = 0;

    while (reaped < count)
    {
        int n = ibv_poll_cq(ctx.send_cq,
                            count - reaped,
                            wcs.data() + reaped);
        if (n < 0)
            throw std::runtime_error(
                "[CQ] ibv_poll_cq (send) returned " + std::to_string(n));

        for (int i = reaped; i < reaped + n; ++i)
        {
            const ibv_wc &wc = wcs[i];
            if (wc.status != IBV_WC_SUCCESS)
            {
                // QP may have transitioned to error state; log vendor syndrome
                std::string detail =
                    "[CQ] Send WC error: " +
                    std::string(ibv_wc_status_str(wc.status)) +
                    "  wr_id=" + std::to_string(wc.wr_id) +
                    "  vendor_err=0x" + [&]
                {
                    std::ostringstream os;
                    os << std::hex << wc.vendor_err;
                    return os.str();
                }();
                throw std::runtime_error(detail);
            }
            std::cout << "[CQ] Send completed  wr_id=" << wc.wr_id << "\n";
        }
        reaped += n;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  post_recv
//
//  Posts one zero-byte receive WR to the recv queue.
//
//  For RDMA WRITE_WITH_IMM the incoming data is DMA'd directly into the
//  remote MR (no receive buffer needed — the rkey+addr in the WR are the
//  destination).  The recv WR here serves only to consume the immediate value
//  notification: when the WRITE_WITH_IMM arrives, the HCA posts a completion
//  to the recv CQ with opcode IBV_WC_RECV_RDMA_WITH_IMM and the imm_data
//  field populated.  Without a pre-posted recv WR, the HCA would send an RNR
//  NAK and the transfer would stall until one is posted.
//
//  Therefore: always post recv WRs BEFORE the remote side starts sending.
//  In our flow, decode posts them during its INIT phase, before the OOB
//  exchange completes, so by the time prefill reaches RTS the recv queue is
//  already loaded.
// ─────────────────────────────────────────────────────────────────────────────

void post_recv(RdmaContext &ctx)
{
    // Zero SGEs: the HCA does not need a buffer, only the CQ notification slot
    ibv_recv_wr rwr{};
    rwr.wr_id = 0;
    rwr.sg_list = nullptr;
    rwr.num_sge = 0;
    rwr.next = nullptr;

    ibv_recv_wr *bad_rwr = nullptr;
    int rc = ibv_post_recv(ctx.qp, &rwr, &bad_rwr);
    if (rc != 0)
        throw std::runtime_error(
            "[RECV] ibv_post_recv failed — rc=" + std::to_string(rc) +
            " errno=" + errno_str(errno) +
            ".  Check: QP in INIT/RTR/RTS state, recv queue not full.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  destroy_rdma_context
//
//  Tears down every verbs and CUDA object in the correct dependency order:
//
//    QP must be destroyed before the CQs it references.
//    MR must be deregistered before the PD is deallocated.
//    PD must be deallocated before the device context is closed.
//    GPU memory must be freed after the MR is deregistered (the MR holds a
//    reference to the underlying GPU pages via nvidia_peermem; freeing the
//    GPU buffer before deregistering the MR produces a kernel warning and
//    may cause a GPU fault on the next DMA).
//
//  All calls are guarded against nullptr so this function is safe to call
//  on a partially-initialised context (e.g. after an exception mid-init).
// ─────────────────────────────────────────────────────────────────────────────

void destroy_rdma_context(RdmaContext &ctx)
{

    // 1. Destroy QP first — stops any in-flight DMAs referencing the MR
    if (ctx.qp)
    {
        ibv_destroy_qp(ctx.qp);
        ctx.qp = nullptr;
        std::cout << "[CTX] QP destroyed\n";
    }

    // 2. Destroy CQs — no QP references them anymore
    if (ctx.send_cq)
    {
        ibv_destroy_cq(ctx.send_cq);
        ctx.send_cq = nullptr;
    }
    if (ctx.recv_cq)
    {
        ibv_destroy_cq(ctx.recv_cq);
        ctx.recv_cq = nullptr;
    }

    // 3. Deregister MR — unpins the GPU pages from the IOMMU
    //    Must happen before cudaFree; nvidia_peermem reference is released here
    if (ctx.mem.mr)
    {
        ibv_dereg_mr(ctx.mem.mr);
        ctx.mem.mr = nullptr;
        ctx.mem.lkey = 0;
        ctx.mem.rkey = 0;
        std::cout << "[CTX] MR deregistered\n";
    }

    // 4. Deallocate PD
    if (ctx.pd)
    {
        ibv_dealloc_pd(ctx.pd);
        ctx.pd = nullptr;
    }

    // 5. Close verbs device context
    if (ctx.ctx)
    {
        ibv_close_device(ctx.ctx);
        ctx.ctx = nullptr;
        std::cout << "[CTX] IB device closed\n";
    }

    // 6. Free GPU memory — safe now that MR is gone
    if (ctx.mem.d_ptr)
    {
        cudaSetDevice(ctx.gpu_id);
        cudaFree(ctx.mem.d_ptr);
        ctx.mem.d_ptr = nullptr;
    }

    // 7. Free pinned host mirror
    if (ctx.mem.h_ptr)
    {
        cudaFreeHost(ctx.mem.h_ptr);
        ctx.mem.h_ptr = nullptr;
    }

    ctx.mem.bytes = 0;
    ctx.mem.addr = 0;
    std::cout << "[CTX] Context fully destroyed for GPU" << ctx.gpu_id << "\n";
}