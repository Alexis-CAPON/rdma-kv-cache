#pragma once
// =============================================================================
//  device_probe.h
//  GPU discovery, IB port validation, NUMA affinity check, and GPU↔HCA binding.
// =============================================================================

#include "cpp/common/config.h"
#include <infiniband/verbs.h>
#include <string>
#include <vector>
#include <cstdint>

// ── Result of device probing for one GPU ─────────────────────────────────────

struct BoundDevice
{
    int gpu_id = -1;
    std::string gpu_name;     // e.g. "Tesla V100-SXM2-16GB"
    int gpu_numa = -1;        // NUMA node of the GPU
    size_t gpu_mem_bytes = 0; // total HBM2

    std::string ib_dev_name; // e.g. "mlx5_0"
    int ib_port = 1;
    int ib_numa = -1;           // NUMA node of the HCA
    uint64_t ib_speed_gbps = 0; // link speed in Gb/s
    std::string ib_state;       // "PORT_ACTIVE" etc.
    uint8_t gid[16] = {};       // GID for this port/index
};

// ── IB Port Information ──────────────────────────────────────────────────────

struct IbPortInfo
{
    std::string dev_name;
    int port;
    ibv_port_attr attr;
    ibv_device_attr dev_attr;
    std::string pci_bus_id;
    int numa_node;
    uint64_t speed_gbps;
};

// ── Helper Functions (Internal) ──────────────────────────────────────────────

namespace probe_detail
{
    // Read NUMA node of a PCI device from sysfs
    int pci_numa_node(const std::string &pci_bus_id);

    // Convert ibv_port_attr speed/width to Gb/s
    uint64_t ib_speed_gbps(const ibv_port_attr &pa);

    // Translate ibv_port_state to string
    std::string port_state_str(ibv_port_state s);

    // Get PCI bus ID of an IB device via sysfs
    std::string ib_pci_bus_id(const std::string &dev_name);

    // Get PCI bus ID of a CUDA GPU
    std::string gpu_pci_bus_id(int gpu_id);

} // namespace probe_detail

// ── Public API ───────────────────────────────────────────────────────────────

/**
 * Discover and validate CUDA GPUs
 * @param gcfg GPU configuration
 * @return Vector of valid GPU IDs
 */
std::vector<int> discover_gpus(const GpuConfig &gcfg);

/**
 * Discover and validate IB devices/ports
 * @param rcfg RDMA configuration
 * @return Vector of active IB port info
 */
std::vector<IbPortInfo> discover_ib_ports(const RdmaConfig &rcfg);

/**
 * Bind GPUs to HCAs (NUMA-aware)
 * @param gpu_ids List of GPU IDs
 * @param ib_ports List of IB port info
 * @param rcfg RDMA configuration
 * @param gcfg GPU configuration
 * @return Vector of GPU↔HCA bindings
 */
std::vector<BoundDevice> bind_gpus_to_hcas(
    const std::vector<int> &gpu_ids,
    const std::vector<IbPortInfo> &ib_ports,
    const RdmaConfig &rcfg,
    const GpuConfig &gcfg);

/**
 * Complete device probing workflow
 * @param cfg Full inference configuration
 * @return Vector of GPU↔HCA bindings
 */
std::vector<BoundDevice> probe_and_bind(const Config &cfg);
