// =============================================================================
//  device_probe.cpp
//  Implementation of GPU discovery, IB port validation, and GPU↔HCA binding.
// =============================================================================

#include "cpp/rdma/device_probe.h"
#include "cpp/common/logger.h"
#include <infiniband/verbs.h>
#include <cuda_runtime.h>

#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  Internal Helper Functions (probe_detail namespace)
// ─────────────────────────────────────────────────────────────────────────────

namespace probe_detail
{

    int pci_numa_node(const std::string &pci_bus_id)
    {
        // CUDA bus IDs look like "0000:03:00.0"; sysfs path is /sys/bus/pci/devices/
        std::string sysfs = "/sys/bus/pci/devices/" + pci_bus_id + "/numa_node";
        std::ifstream f(sysfs);
        if (!f.is_open())
            return -1;
        int node = -1;
        f >> node;
        return node;
    }

    uint64_t ib_speed_gbps(const ibv_port_attr &pa)
    {
        // active_speed is a bitmask: 1=SDR(2.5), 2=DDR(5), 4=QDR(10),
        //   8=FDR10(10), 16=FDR(14), 32=EDR(25), 64=HDR(50), 128=NDR(100)
        uint64_t lane = 0;
        switch (pa.active_speed)
        {
        case 1:
            lane = 2;
            break;
        case 2:
            lane = 5;
            break;
        case 4:
            lane = 10;
            break;
        case 8:
            lane = 10;
            break;
        case 16:
            lane = 14;
            break;
        case 32:
            lane = 25;
            break;
        case 64:
            lane = 50;
            break;
        case 128:
            lane = 100;
            break;
        default:
            lane = 0;
        }

        // active_width: 1=1x, 2=4x, 4=8x, 8=12x
        uint64_t width = 1;
        switch (pa.active_width)
        {
        case 1:
            width = 1;
            break;
        case 2:
            width = 4;
            break;
        case 4:
            width = 8;
            break;
        case 8:
            width = 12;
            break;
        }
        return lane * width;
    }

    std::string port_state_str(ibv_port_state s)
    {
        switch (s)
        {
        case IBV_PORT_NOP:
            return "NOP";
        case IBV_PORT_DOWN:
            return "PORT_DOWN";
        case IBV_PORT_INIT:
            return "PORT_INIT";
        case IBV_PORT_ARMED:
            return "PORT_ARMED";
        case IBV_PORT_ACTIVE:
            return "PORT_ACTIVE";
        default:
            return "UNKNOWN";
        }
    }

    std::string ib_pci_bus_id(const std::string &dev_name)
    {
        std::string link = "/sys/class/infiniband/" + dev_name + "/device";
        char resolved[256] = {};
        if (realpath(link.c_str(), resolved) == nullptr)
            return "";
        // Last component of resolved path is the PCI BDF
        std::string path(resolved);
        auto pos = path.rfind('/');
        return (pos == std::string::npos) ? path : path.substr(pos + 1);
    }

    std::string gpu_pci_bus_id(int gpu_id)
    {
        char buf[64] = {};
        if (cudaDeviceGetPCIBusId(buf, sizeof(buf), gpu_id) != cudaSuccess)
            return "";
        // Lowercase and ensure domain prefix
        std::string s(buf);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        // Pad to "0000:xx:xx.x" if domain missing
        if (s.size() == 7)
            s = "0000:" + s;
        return s;
    }

} // namespace probe_detail

// ─────────────────────────────────────────────────────────────────────────────
//  Step 1 — Enumerate and validate CUDA GPUs
// ─────────────────────────────────────────────────────────────────────────────

std::vector<int> discover_gpus(const GpuConfig &gcfg)
{
    int total = 0;
    if (cudaGetDeviceCount(&total) != cudaSuccess || total == 0)
    {
        Logger::error("[GPU] No CUDA-capable GPUs found");
        throw std::runtime_error("[GPU] No CUDA-capable GPUs found");
    }

    Logger::info("[GPU] Total CUDA devices visible: " + std::to_string(total));

    std::vector<int> candidates;

    if (!gcfg.gpu_ids.empty())
    {
        // User-specified list — validate each
        for (int id : gcfg.gpu_ids)
        {
            if (id < 0 || id >= total)
            {
                Logger::error("[GPU] gpu_ids contains invalid GPU id: " +
                              std::to_string(id) + " (max=" + std::to_string(total - 1) + ")");
                throw std::runtime_error("[GPU] Invalid GPU ID in config");
            }
            candidates.push_back(id);
        }
    }
    else
    {
        // Auto-detect: pick all GPUs
        for (int i = 0; i < total; ++i)
            candidates.push_back(i);
    }

    // Print info and validate each candidate
    std::vector<int> valid;
    for (int id : candidates)
    {
        cudaDeviceProp prop{};
        cudaGetDeviceProperties(&prop, id);

        bool is_v100_or_better = (prop.major > 7) ||
                                 (prop.major == 7 && prop.minor >= 0);
        if (!is_v100_or_better)
        {
            Logger::warn("[GPU] GPU " + std::to_string(id) + " (" +
                         std::string(prop.name) + ") is below Volta (sm_70). " +
                         "GPUDirect RDMA may not work correctly.");
        }

        Logger::debug("[GPU] [" + std::to_string(id) + "] " + std::string(prop.name) +
                      "  cc=" + std::to_string(prop.major) + "." + std::to_string(prop.minor) +
                      "  mem=" + std::to_string(prop.totalGlobalMem / (1 << 20)) + " MB" +
                      "  PCIe=" + probe_detail::gpu_pci_bus_id(id) +
                      "  NUMA=" + std::to_string(probe_detail::pci_numa_node(probe_detail::gpu_pci_bus_id(id))));

        valid.push_back(id);
    }

    if (valid.empty())
    {
        Logger::error("[GPU] No valid GPUs after filtering");
        throw std::runtime_error("[GPU] No valid GPUs");
    }

    // Enable P2P between all pairs
    if (gcfg.enable_peer_access)
    {
        for (int a : valid)
        {
            for (int b : valid)
            {
                if (a == b)
                    continue;
                int can = 0;
                cudaDeviceCanAccessPeer(&can, a, b);
                if (can)
                {
                    cudaSetDevice(a);
                    cudaError_t e = cudaDeviceEnablePeerAccess(b, 0);
                    if (e != cudaSuccess && e != cudaErrorPeerAccessAlreadyEnabled)
                    {
                        Logger::warn("[GPU] P2P enable " + std::to_string(a) + "→" +
                                     std::to_string(b) + " failed: " +
                                     std::string(cudaGetErrorString(e)));
                    }
                    else
                    {
                        Logger::info("[GPU] P2P enabled: GPU" + std::to_string(a) +
                                     " → GPU" + std::to_string(b));
                    }
                }
                else
                {
                    Logger::debug("[GPU] P2P not supported: GPU" + std::to_string(a) +
                                  " ↔ GPU" + std::to_string(b));
                }
            }
        }
    }

    return valid;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Step 2 — Enumerate and validate IB devices / ports
// ─────────────────────────────────────────────────────────────────────────────

std::vector<IbPortInfo> discover_ib_ports(const RdmaConfig &rcfg)
{
    int num_devs = 0;
    ibv_device **dev_list = ibv_get_device_list(&num_devs);
    if (!dev_list || num_devs == 0)
    {
        Logger::error("[IB] No IB/RDMA devices found. Check: ibstat / ibv_devinfo");
        throw std::runtime_error("[IB] No IB devices found");
    }

    std::vector<IbPortInfo> active_ports;

    for (int di = 0; di < num_devs; ++di)
    {
        std::string dname = ibv_get_device_name(dev_list[di]);

        // If user specified devices, skip others
        if (!rcfg.ib_devices.empty())
        {
            bool requested = false;
            for (auto &req : rcfg.ib_devices)
                if (req == dname)
                {
                    requested = true;
                    break;
                }
            if (!requested)
                continue;
        }

        ibv_context *ctx = ibv_open_device(dev_list[di]);
        if (!ctx)
        {
            Logger::warn("[IB] Cannot open device " + dname + " — skipping");
            continue;
        }

        ibv_device_attr dev_attr{};
        ibv_query_device(ctx, &dev_attr);

        for (int port = 1; port <= dev_attr.phys_port_cnt; ++port)
        {
            // If user specified ib_port, only check that port
            if (!rcfg.ib_devices.empty() && port != rcfg.ib_port)
                continue;

            ibv_port_attr pa{};
            if (ibv_query_port(ctx, port, &pa) != 0)
                continue;

            std::string state = probe_detail::port_state_str(pa.state);
            std::string pci = probe_detail::ib_pci_bus_id(dname);
            int numa = probe_detail::pci_numa_node(pci);
            uint64_t spd = probe_detail::ib_speed_gbps(pa);

            std::ostringstream oss;
            oss << "[IB] " << dname << " port " << port
                << "  state=" << state
                << "  speed=" << spd << " Gb/s"
                << "  LID=0x" << std::hex << pa.lid << std::dec
                << "  PCIe=" << pci
                << "  NUMA=" << numa;
            Logger::debug(oss.str());

            if (pa.state == IBV_PORT_ACTIVE)
            {
                IbPortInfo info;
                info.dev_name = dname;
                info.port = port;
                info.attr = pa;
                info.dev_attr = dev_attr;
                info.pci_bus_id = pci;
                info.numa_node = numa;
                info.speed_gbps = spd;
                active_ports.push_back(info);
            }
            else
            {
                Logger::warn("[IB] " + dname + " port " + std::to_string(port) +
                             " is NOT active (" + state + ") — skipping");
            }
        }
        ibv_close_device(ctx);
    }

    ibv_free_device_list(dev_list);

    if (active_ports.empty())
    {
        Logger::error("[IB] No active IB ports found. Check cable / switch / ibstat");
        throw std::runtime_error("[IB] No active ports");
    }

    // Validate GID index exists on each port
    for (auto &info : active_ports)
    {
        ibv_context *ctx = nullptr;
        {
            int nd = 0;
            ibv_device **dl = ibv_get_device_list(&nd);
            for (int i = 0; i < nd; ++i)
            {
                if (ibv_get_device_name(dl[i]) == info.dev_name)
                {
                    ctx = ibv_open_device(dl[i]);
                    break;
                }
            }
            ibv_free_device_list(dl);
        }
        if (!ctx)
            continue;

        ibv_gid gid{};
        int rc = ibv_query_gid(ctx, info.port, rcfg.gid_index, &gid);
        ibv_close_device(ctx);

        if (rc != 0 || (gid.global.interface_id == 0 &&
                        gid.global.subnet_prefix == 0))
        {
            Logger::warn("[IB] " + info.dev_name + " port " +
                         std::to_string(info.port) + " GID index " +
                         std::to_string(rcfg.gid_index) +
                         " is zero/invalid. Check gid_index in config (0=IB, 3=RoCEv2).");
        }
        else
        {
            // Format GID
            const uint8_t *g = gid.raw;
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                     "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                     g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7],
                     g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
            Logger::debug("[IB] " + info.dev_name + " port " +
                          std::to_string(info.port) + " GID[" +
                          std::to_string(rcfg.gid_index) + "]=" + std::string(buf));
        }
    }

    return active_ports;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Step 3 — Bind GPUs to IB ports (NUMA-aware)
// ─────────────────────────────────────────────────────────────────────────────

std::vector<BoundDevice> bind_gpus_to_hcas(
    const std::vector<int> &gpu_ids,
    const std::vector<IbPortInfo> &ib_ports,
    const RdmaConfig &rcfg,
    const GpuConfig &gcfg)
{
    std::vector<BoundDevice> result;

    for (int gpu_id : gpu_ids)
    {
        BoundDevice bd;
        bd.gpu_id = gpu_id;

        cudaDeviceProp prop{};
        cudaGetDeviceProperties(&prop, gpu_id);
        bd.gpu_name = prop.name;
        bd.gpu_mem_bytes = prop.totalGlobalMem;

        std::string gpu_pci = probe_detail::gpu_pci_bus_id(gpu_id);
        bd.gpu_numa = probe_detail::pci_numa_node(gpu_pci);

        // Find best IB port: prefer same NUMA node, then highest speed
        const IbPortInfo *best = nullptr;
        for (const auto &port : ib_ports)
        {
            if (best == nullptr)
            {
                best = &port;
                continue;
            }

            bool same_numa_new = (port.numa_node == bd.gpu_numa);
            bool same_numa_best = (best->numa_node == bd.gpu_numa);

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

        if (!best)
        {
            Logger::error("[BIND] No IB port available for GPU " + std::to_string(gpu_id));
            throw std::runtime_error("[BIND] No IB port for GPU");
        }

        bd.ib_dev_name = best->dev_name;
        bd.ib_port = best->port;
        bd.ib_numa = best->numa_node;
        bd.ib_speed_gbps = best->speed_gbps;
        bd.ib_state = "PORT_ACTIVE";

        // Query and store GID
        {
            int nd = 0;
            ibv_device **dl = ibv_get_device_list(&nd);
            for (int i = 0; i < nd; ++i)
            {
                if (ibv_get_device_name(dl[i]) == bd.ib_dev_name)
                {
                    ibv_context *ctx = ibv_open_device(dl[i]);
                    if (ctx)
                    {
                        ibv_gid gid{};
                        ibv_query_gid(ctx, bd.ib_port, rcfg.gid_index, &gid);
                        std::memcpy(bd.gid, gid.raw, 16);
                        ibv_close_device(ctx);
                    }
                    break;
                }
            }
            ibv_free_device_list(dl);
        }

        if (gcfg.require_same_numa && bd.gpu_numa != bd.ib_numa &&
            bd.gpu_numa != -1 && bd.ib_numa != -1)
        {
            Logger::error("[BIND] GPU" + std::to_string(gpu_id) +
                          " (NUMA " + std::to_string(bd.gpu_numa) + ") and " +
                          bd.ib_dev_name + " port " + std::to_string(bd.ib_port) +
                          " (NUMA " + std::to_string(bd.ib_numa) + ") are on " +
                          "different NUMA nodes. Set require_same_numa=false to override.");
            throw std::runtime_error("[BIND] NUMA mismatch");
        }

        std::ostringstream oss;
        oss << "[BIND] GPU" << gpu_id << " (" << bd.gpu_name
            << "  NUMA=" << bd.gpu_numa << ")"
            << "  →  " << bd.ib_dev_name << " port " << bd.ib_port
            << " (" << bd.ib_speed_gbps << " Gb/s"
            << "  NUMA=" << bd.ib_numa << ")"
            << (bd.gpu_numa == bd.ib_numa ? "  [same NUMA ✓]"
                                          : "  [cross-NUMA !]");
        Logger::info(oss.str());

        result.push_back(bd);
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public entry point
// ─────────────────────────────────────────────────────────────────────────────

std::vector<BoundDevice> probe_and_bind(const Config &cfg)
{
    Logger::info("=== Device probing ===");

    auto gpu_ids = discover_gpus(cfg.gpu);
    auto ib_ports = discover_ib_ports(cfg.rdma);
    auto bindings = bind_gpus_to_hcas(gpu_ids, ib_ports, cfg.rdma, cfg.gpu);

    Logger::info("=== Device probing complete ===");

    return bindings;
}
