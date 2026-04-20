#include "cpp/rdma/rdma_engine.h"
#include "cpp/rdma/device_probe.h"
#include "cpp/common/logger.h"
#include <infiniband/verbs.h>
#include <cuda_runtime.h>
#include <cstring>
#include <stdexcept>

RDMAEngine::RDMAEngine(Config &config, NodeInfo &node_info)
    : config_(config),
      node_info_(node_info),
      initialized_(false)
{
    // Initialize rdma_ctx_ to zeros
    std::memset(&rdma_ctx_, 0, sizeof(rdma_ctx_));
}

RDMAEngine::~RDMAEngine()
{
    shutdown();
}

// ══════════════════════════════════════════════════════════════════════════════
//  Lifecycle Methods
// ══════════════════════════════════════════════════════════════════════════════

bool RDMAEngine::initialize()
{
    Logger::info("RDMAEngine: Initializing GPUDirect RDMA");

    try
    {
        // 1. Probe and bind GPU to HCA
        if (!probe_and_bind_devices())
        {
            Logger::error("RDMAEngine: Device probing failed");
            return false;
        }

        // 2. Bind CUDA device
        if (!bind_cuda_device())
        {
            Logger::error("RDMAEngine: CUDA device binding failed");
            return false;
        }

        // 3. Open IB device and create PD
        if (!open_ib_device())
        {
            Logger::error("RDMAEngine: IB device open failed");
            return false;
        }

        // 4. Allocate and register GPU memory
        if (!alloc_and_register_gpu_memory())
        {
            Logger::error("RDMAEngine: GPU memory registration failed");
            return false;
        }

        // 5. Create CQs
        if (!create_cqs())
        {
            Logger::error("RDMAEngine: CQ creation failed");
            return false;
        }

        // 6. Create QPs for each peer and fill NodeInfo.qmaps
        // Determine expected peers based on role
        std::vector<std::string> expected_peers;

        if (node_info_.role == NodeRole::PREFILL)
        {
            // Prefill node connects to all decode nodes
            for (int i = 1; i <= config_.expected_decode_nodes; ++i)
            {
                expected_peers.push_back("decode-" + std::to_string(i));
            }
        }
        else if (node_info_.role == NodeRole::DECODE)
        {
            // Decode node connects to all prefill nodes
            for (int i = 1; i <= config_.expected_prefill_nodes; ++i)
            {
                expected_peers.push_back("prefill-" + std::to_string(i));
            }
        }

        // Create one QP per peer
        for (const auto &peer_id : expected_peers)
        {
            ibv_qp *qp = create_qp_for_peer(peer_id);
            if (!qp)
            {
                Logger::error("RDMAEngine: Failed to create QP for peer " + peer_id);
                return false;
            }

            // Store QP in map
            peer_qps_[peer_id] = qp;

            Logger::info("RDMAEngine: Created QP for peer " + peer_id +
                         " (QPN=" + std::to_string(qp->qp_num) + ")");
        }

        initialized_ = true;
        Logger::info("RDMAEngine: Initialization complete");
        return true;
    }
    catch (const std::exception &ex)
    {
        Logger::error("RDMAEngine: Initialization exception: " + std::string(ex.what()));
        return false;
    }
}

bool RDMAEngine::connect_all_qps()
{
    Logger::info("RDMAEngine: Connecting all QPs to peers");

    for (auto &[peer_id, qp] : peer_qps_)
    {
        if (!connect_qp_to_peer(peer_id, qp))
        {
            Logger::error("RDMAEngine: Failed to connect QP to peer " + peer_id);
            return false;
        }

        Logger::info("RDMAEngine: Connected QP to peer " + peer_id);
    }

    Logger::info("RDMAEngine: All QPs connected successfully");
    return true;
}

void RDMAEngine::shutdown()
{
    if (!initialized_)
        return;

    Logger::info("RDMAEngine: Shutting down");

    // Destroy all QPs
    for (auto &[peer_id, qp] : peer_qps_)
    {
        if (qp)
        {
            ibv_destroy_qp(qp);
            Logger::debug("RDMAEngine: Destroyed QP for peer " + peer_id);
        }
    }
    peer_qps_.clear();

    // Destroy the rest of the RDMA context
    destroy_rdma_context(rdma_ctx_);

    initialized_ = false;
    Logger::info("RDMAEngine: Shutdown complete");
}

// ══════════════════════════════════════════════════════════════════════════════
//  Data Transfer Methods
// ══════════════════════════════════════════════════════════════════════════════

bool RDMAEngine::post_write(const std::string &peer_id,
                             size_t src_offset,
                             size_t dst_offset,
                             size_t length,
                             uint32_t imm_data,
                             bool signal)
{
    // Find the QP for this peer
    auto it = peer_qps_.find(peer_id);
    if (it == peer_qps_.end())
    {
        Logger::error("RDMAEngine: No QP found for peer " + peer_id);
        return false;
    }

    // Find peer info to get remote addr and rkey
    PeerInfoandQPs *peer_info = find_peer_info(peer_id);
    if (!peer_info)
    {
        Logger::error("RDMAEngine: No peer info found for " + peer_id);
        return false;
    }

    try
    {
        // Use the gpudirect_rdma post_rdma_write function
        // We need to temporarily set the QP in rdma_ctx_ for the call
        ibv_qp *saved_qp = rdma_ctx_.qp;
        rdma_ctx_.qp = it->second;

        post_rdma_write(rdma_ctx_,
                        src_offset,
                        dst_offset,
                        length,
                        peer_info->remote_addr,
                        peer_info->remote_rkey,
                        imm_data,
                        signal);

        // Restore original QP
        rdma_ctx_.qp = saved_qp;

        return true;
    }
    catch (const std::exception &ex)
    {
        Logger::error("RDMAEngine: post_write failed: " + std::string(ex.what()));
        return false;
    }
}

bool RDMAEngine::post_recv_wrs(int count)
{
    try
    {
        for (int i = 0; i < count; ++i)
        {
            post_recv(rdma_ctx_);
        }
        Logger::debug("RDMAEngine: Posted " + std::to_string(count) + " recv WRs");
        return true;
    }
    catch (const std::exception &ex)
    {
        Logger::error("RDMAEngine: post_recv_wrs failed: " + std::string(ex.what()));
        return false;
    }
}

int RDMAEngine::poll_send_cq(int count)
{
    try
    {
        poll_send_cq(rdma_ctx_, count);
        return count;
    }
    catch (const std::exception &ex)
    {
        Logger::error("RDMAEngine: poll_send_cq failed: " + std::string(ex.what()));
        return -1;
    }
}

int RDMAEngine::poll_recv_cq(std::vector<ibv_wc> &wcs, int max_count)
{
    if (!rdma_ctx_.recv_cq)
        return -1;

    wcs.resize(max_count);
    int n = ibv_poll_cq(rdma_ctx_.recv_cq, max_count, wcs.data());

    if (n < 0)
    {
        Logger::error("RDMAEngine: ibv_poll_cq (recv) failed");
        return -1;
    }

    // Check for errors in completions
    for (int i = 0; i < n; ++i)
    {
        if (wcs[i].status != IBV_WC_SUCCESS)
        {
            Logger::error("RDMAEngine: Recv WC error: " +
                          std::string(ibv_wc_status_str(wcs[i].status)));
            return -1;
        }
    }

    return n;
}

// ══════════════════════════════════════════════════════════════════════════════
//  Accessors
// ══════════════════════════════════════════════════════════════════════════════

int RDMAEngine::get_send_cq_fd() const
{
    // For future epoll integration: create completion channel
    // For now, return -1 (busy polling mode)
    return -1;
}

int RDMAEngine::get_recv_cq_fd() const
{
    // For future epoll integration: create completion channel
    // For now, return -1 (busy polling mode)
    return -1;
}

// ══════════════════════════════════════════════════════════════════════════════
//  Internal Helper Methods
// ══════════════════════════════════════════════════════════════════════════════

bool RDMAEngine::probe_and_bind_devices()
{
    Logger::info("RDMAEngine: Probing GPU and IB devices");

    try
    {
        // Use device_probe to find GPU and HCA
        // Since we support one GPU per node, we pick the first (or user-specified)
        auto gpu_ids = discover_gpus(config_.gpu);

        if (gpu_ids.empty())
        {
            Logger::error("RDMAEngine: No GPUs found");
            return false;
        }

        // Pick the first GPU
        int gpu_id = gpu_ids[0];
        rdma_ctx_.gpu_id = gpu_id;

        // Query GPU properties
        cudaDeviceProp prop{};
        cudaGetDeviceProperties(&prop, gpu_id);

        node_info_.gpu_id = gpu_id;
        node_info_.gpu_name = prop.name;
        node_info_.gpu_mem_bytes = prop.totalGlobalMem;

        // Get GPU PCI info for NUMA
        std::string gpu_pci = probe_detail::gpu_pci_bus_id(gpu_id);
        node_info_.gpu_numa = probe_detail::pci_numa_node(gpu_pci);

        Logger::info("RDMAEngine: Selected GPU" + std::to_string(gpu_id) +
                     " (" + node_info_.gpu_name + ")" +
                     " NUMA=" + std::to_string(node_info_.gpu_numa));

        // Discover IB ports
        auto ib_ports = discover_ib_ports(config_.rdma);

        if (ib_ports.empty())
        {
            Logger::error("RDMAEngine: No active IB ports found");
            return false;
        }

        // Pick the best IB port (NUMA-aware)
        const IbPortInfo *best = &ib_ports[0];
        for (const auto &port : ib_ports)
        {
            bool same_numa_new = (port.numa_node == node_info_.gpu_numa);
            bool same_numa_best = (best->numa_node == node_info_.gpu_numa);

            if (same_numa_new && !same_numa_best)
            {
                best = &port;
            }
            else if (same_numa_new == same_numa_best &&
                     port.speed_gbps > best->speed_gbps)
            {
                best = &port;
            }
        }

        node_info_.ib_dev_name = best->dev_name;
        node_info_.ib_port = best->port;
        node_info_.ib_numa = best->numa_node;
        node_info_.ib_speed_gbps = best->speed_gbps;

        Logger::info("RDMAEngine: Selected IB device " + node_info_.ib_dev_name +
                     " port " + std::to_string(node_info_.ib_port) +
                     " (" + std::to_string(node_info_.ib_speed_gbps) + " Gb/s)" +
                     " NUMA=" + std::to_string(node_info_.ib_numa));

        if (config_.gpu.require_same_numa &&
            node_info_.gpu_numa != node_info_.ib_numa &&
            node_info_.gpu_numa != -1 && node_info_.ib_numa != -1)
        {
            Logger::warn("RDMAEngine: GPU and HCA are on different NUMA nodes");
        }

        return true;
    }
    catch (const std::exception &ex)
    {
        Logger::error("RDMAEngine: Device probing failed: " + std::string(ex.what()));
        return false;
    }
}

bool RDMAEngine::bind_cuda_device()
{
    Logger::info("RDMAEngine: Binding CUDA device GPU" + std::to_string(rdma_ctx_.gpu_id));

    try
    {
        bind_cuda_device(rdma_ctx_.gpu_id);
        return true;
    }
    catch (const std::exception &ex)
    {
        Logger::error("RDMAEngine: CUDA bind failed: " + std::string(ex.what()));
        return false;
    }
}

bool RDMAEngine::open_ib_device()
{
    Logger::info("RDMAEngine: Opening IB device " + node_info_.ib_dev_name);

    try
    {
        open_ib_device(rdma_ctx_, node_info_.ib_dev_name);
        return true;
    }
    catch (const std::exception &ex)
    {
        Logger::error("RDMAEngine: IB device open failed: " + std::string(ex.what()));
        return false;
    }
}

bool RDMAEngine::alloc_and_register_gpu_memory()
{
    size_t kv_bytes = config_.memory.kv_buffer_mb * 1024UL * 1024UL;

    Logger::info("RDMAEngine: Allocating " + std::to_string(kv_bytes / (1024 * 1024)) +
                 " MB GPU memory");

    try
    {
        alloc_and_register_gpu_mem(rdma_ctx_, kv_bytes, rdma_ctx_.gpu_id, config_.memory);

        // Fill NodeInfo with MR details
        node_info_.gpu_base_addr = rdma_ctx_.mem.addr;
        node_info_.lkey = rdma_ctx_.mem.lkey;
        node_info_.rkey = rdma_ctx_.mem.rkey;
        node_info_.memory_pool_size = rdma_ctx_.mem.bytes;

        Logger::info("RDMAEngine: GPU memory registered successfully" +
                     " rkey=0x" + std::to_string(node_info_.rkey) +
                     " addr=0x" + std::to_string(node_info_.gpu_base_addr));

        return true;
    }
    catch (const std::exception &ex)
    {
        Logger::error("RDMAEngine: GPU memory registration failed: " + std::string(ex.what()));
        return false;
    }
}

bool RDMAEngine::create_cqs()
{
    Logger::info("RDMAEngine: Creating CQs");

    try
    {
        int cq_depth = std::min(config_.rdma.cq_depth, rdma_ctx_.dev_attr.max_cqe);

        rdma_ctx_.send_cq = ibv_create_cq(rdma_ctx_.ctx, cq_depth, nullptr, nullptr, 0);
        if (!rdma_ctx_.send_cq)
        {
            Logger::error("RDMAEngine: Failed to create send CQ");
            return false;
        }

        rdma_ctx_.recv_cq = ibv_create_cq(rdma_ctx_.ctx, cq_depth, nullptr, nullptr, 0);
        if (!rdma_ctx_.recv_cq)
        {
            ibv_destroy_cq(rdma_ctx_.send_cq);
            rdma_ctx_.send_cq = nullptr;
            Logger::error("RDMAEngine: Failed to create recv CQ");
            return false;
        }

        Logger::info("RDMAEngine: CQs created successfully");
        return true;
    }
    catch (const std::exception &ex)
    {
        Logger::error("RDMAEngine: CQ creation failed: " + std::string(ex.what()));
        return false;
    }
}

ibv_qp *RDMAEngine::create_qp_for_peer(const std::string &peer_id)
{
    Logger::debug("RDMAEngine: Creating QP for peer " + peer_id);

    // Create QP init attributes
    ibv_qp_init_attr init_attr{};
    init_attr.send_cq = rdma_ctx_.send_cq;
    init_attr.recv_cq = rdma_ctx_.recv_cq;
    init_attr.qp_type = IBV_QPT_RC;
    init_attr.sq_sig_all = 0; // Only signal flagged WRs
    init_attr.cap.max_send_wr =
        std::min(config_.rdma.qp_max_send_wr, rdma_ctx_.dev_attr.max_qp_wr);
    init_attr.cap.max_recv_wr =
        std::min(config_.rdma.qp_max_recv_wr, rdma_ctx_.dev_attr.max_qp_wr);
    init_attr.cap.max_send_sge = std::min(4, rdma_ctx_.dev_attr.max_sge);
    init_attr.cap.max_recv_sge = std::min(4, rdma_ctx_.dev_attr.max_sge);
    init_attr.cap.max_inline_data = static_cast<uint32_t>(config_.rdma.qp_max_inline_data);

    ibv_qp *qp = ibv_create_qp(rdma_ctx_.pd, &init_attr);
    if (!qp)
    {
        Logger::error("RDMAEngine: ibv_create_qp failed for peer " + peer_id);
        return nullptr;
    }

    // Transition to INIT
    ibv_qp_attr attr{};
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = static_cast<uint8_t>(config_.rdma.ib_port);
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                           IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_READ;

    int mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

    if (ibv_modify_qp(qp, &attr, mask))
    {
        Logger::error("RDMAEngine: QP RESET→INIT failed for peer " + peer_id);
        ibv_destroy_qp(qp);
        return nullptr;
    }

    // Fill local QP info into NodeInfo.qmaps
    PeerInfoandQPs peer_qp_info{};
    peer_qp_info.peer_node_id = peer_id;
    peer_qp_info.local_qp_num = qp->qp_num;
    peer_qp_info.local_lid = rdma_ctx_.port_attr.lid;
    peer_qp_info.local_psn = static_cast<uint32_t>(lrand48()) & 0x00FFFFFF;

    // Query and store local GID
    ibv_gid gid{};
    if (ibv_query_gid(rdma_ctx_.ctx, config_.rdma.ib_port, config_.rdma.gid_index, &gid) == 0)
    {
        std::memcpy(peer_qp_info.local_gid, gid.raw, 16);
    }

    // Add to NodeInfo.qmaps
    node_info_.qmaps.push_back(peer_qp_info);

    Logger::debug("RDMAEngine: Created QP for peer " + peer_id +
                  " QPN=" + std::to_string(qp->qp_num));

    return qp;
}

bool RDMAEngine::connect_qp_to_peer(const std::string &peer_id, ibv_qp *qp)
{
    Logger::debug("RDMAEngine: Connecting QP to peer " + peer_id);

    // Find peer info in qmaps
    PeerInfoandQPs *peer_info = find_peer_info(peer_id);
    if (!peer_info)
    {
        Logger::error("RDMAEngine: No peer info found for " + peer_id);
        return false;
    }

    // Verify remote info is filled
    if (peer_info->remote_qp_num == 0)
    {
        Logger::error("RDMAEngine: Remote QP info not filled for " + peer_id);
        return false;
    }

    try
    {
        // INIT → RTR
        {
            ibv_qp_attr attr{};
            attr.qp_state = IBV_QPS_RTR;
            attr.path_mtu = static_cast<ibv_mtu>([](int mtu) {
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
                    return IBV_MTU_4096;
                }
            }(config_.rdma.mtu));

            attr.dest_qp_num = peer_info->remote_qp_num;
            attr.rq_psn = peer_info->remote_psn;
            attr.max_dest_rd_atomic = static_cast<uint8_t>(config_.rdma.max_rd_atomic);
            attr.min_rnr_timer = static_cast<uint8_t>(config_.rdma.min_rnr_timer);

            // Address Handle
            attr.ah_attr.is_global = 1;
            attr.ah_attr.port_num = static_cast<uint8_t>(config_.rdma.ib_port);
            attr.ah_attr.sl = static_cast<uint8_t>(config_.rdma.sl);
            attr.ah_attr.src_path_bits = 0;

            // GRH
            std::memcpy(attr.ah_attr.grh.dgid.raw, peer_info->remote_gid, 16);
            attr.ah_attr.grh.sgid_index = static_cast<uint8_t>(config_.rdma.gid_index);
            attr.ah_attr.grh.hop_limit = 64;
            attr.ah_attr.grh.traffic_class = 0;
            attr.ah_attr.dlid = peer_info->remote_lid;

            int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                       IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                       IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

            if (ibv_modify_qp(qp, &attr, mask))
            {
                Logger::error("RDMAEngine: QP INIT→RTR failed for peer " + peer_id);
                return false;
            }

            Logger::debug("RDMAEngine: QP INIT→RTR for peer " + peer_id);
        }

        // RTR → RTS
        {
            ibv_qp_attr attr{};
            attr.qp_state = IBV_QPS_RTS;
            attr.sq_psn = peer_info->local_psn;
            attr.timeout = static_cast<uint8_t>(config_.rdma.timeout);
            attr.retry_cnt = static_cast<uint8_t>(config_.rdma.retry_cnt);
            attr.rnr_retry = static_cast<uint8_t>(config_.rdma.rnr_retry);
            attr.max_rd_atomic = static_cast<uint8_t>(config_.rdma.max_rd_atomic);

            int mask = IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                       IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC;

            if (ibv_modify_qp(qp, &attr, mask))
            {
                Logger::error("RDMAEngine: QP RTR→RTS failed for peer " + peer_id);
                return false;
            }

            Logger::debug("RDMAEngine: QP RTR→RTS for peer " + peer_id);
        }

        Logger::info("RDMAEngine: QP connected to peer " + peer_id +
                     " (local_QPN=" + std::to_string(peer_info->local_qp_num) +
                     " remote_QPN=" + std::to_string(peer_info->remote_qp_num) + ")");

        return true;
    }
    catch (const std::exception &ex)
    {
        Logger::error("RDMAEngine: QP connection failed: " + std::string(ex.what()));
        return false;
    }
}

PeerInfoandQPs *RDMAEngine::find_peer_info(const std::string &peer_id)
{
    for (auto &qmap : node_info_.qmaps)
    {
        if (qmap.peer_node_id == peer_id)
        {
            return &qmap;
        }
    }
    return nullptr;
}
