// cpp/bindings/rdma_bindings.cpp

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "cpp/rdma/rdma_engine.h"
#include "cpp/common/logger.h"
#include <infiniband/verbs.h>
#include <map>

namespace py = pybind11;

class RDMABindings
{
public:
    RDMABindings()
    {
        LOG_INFO("RDMABindings initialized");
    }

    ~RDMABindings()
    {
        // Deregister all memory regions
        for (auto &[name, region] : registered_regions_)
        {
            if (region.mr)
            {
                ibv_dereg_mr(region.mr);
                LOG_DEBUG("Deregistered memory region: " + name);
            }
        }
    }

    // ========================================
    // Initialization
    // ========================================

    bool initialize(
        const std::string &device_name,
        int port = 1,
        int gid_index = 0)
    {
        try
        {
            // Create Config and NodeInfo
            Config config = create_default_config();
            config.rdma.ib_port = port;
            config.rdma.gid_index = gid_index;

            NodeInfo node_info;
            node_info.ib_dev_name = device_name;

            rdma_engine_ = std::make_unique<RDMAEngine>(config, node_info);

            if (!rdma_engine_->initialize())
            {
                LOG_ERROR("RDMA engine initialization failed");
                return false;
            }

            // Store context for memory registration
            ctx_ = &rdma_engine_->get_context();

            LOG_INFO("RDMA engine initialized successfully");
            return true;
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("RDMA initialization failed: " + std::string(e.what()));
            return false;
        }
    }

    // ========================================
    // Memory Registration (NEW)
    // ========================================

    /**
     * Register external GPU memory (from vLLM) with RDMA
     *
     * @param gpu_ptr GPU memory address (from tensor.data_ptr())
     * @param size Size in bytes
     * @param name Identifier (e.g., layer name)
     * @return true if successful
     */
    bool register_gpu_memory(
        uint64_t gpu_ptr,
        size_t size,
        const std::string &name = "unnamed")
    {
        if (!ctx_ || !ctx_->pd)
        {
            LOG_ERROR("RDMA not initialized - call initialize() first");
            return false;
        }

        try
        {
            // Register memory with InfiniBand
            ibv_mr *mr = ibv_reg_mr(
                ctx_->pd,
                (void *)gpu_ptr,
                size,
                IBV_ACCESS_LOCAL_WRITE |
                    IBV_ACCESS_REMOTE_WRITE |
                    IBV_ACCESS_REMOTE_READ |
                    IBV_ACCESS_RELAXED_ORDERING // Critical for GPU performance
            );

            if (!mr)
            {
                LOG_ERROR("ibv_reg_mr failed for " + name + ": " +
                          std::string(strerror(errno)));
                return false;
            }

            // Store registration info
            RegisteredRegion region;
            region.gpu_ptr = gpu_ptr;
            region.size = size;
            region.mr = mr;
            region.lkey = mr->lkey;
            region.rkey = mr->rkey;

            registered_regions_[name] = region;

            LOG_INFO("Registered GPU memory '" + name + "': " +
                     "ptr=0x" + std::to_string(gpu_ptr) +
                     " size=" + std::to_string(size / (1024 * 1024)) + "MB " +
                     "lkey=0x" + std::to_string(mr->lkey) +
                     " rkey=0x" + std::to_string(mr->rkey));

            return true;
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("register_gpu_memory exception: " + std::string(e.what()));
            return false;
        }
    }

    /**
     * Get lkey for a GPU pointer (needed for RDMA send)
     */
    uint32_t get_lkey_for_ptr(uint64_t gpu_ptr)
    {
        for (const auto &[name, region] : registered_regions_)
        {
            if (gpu_ptr >= region.gpu_ptr &&
                gpu_ptr < region.gpu_ptr + region.size)
            {
                return region.lkey;
            }
        }
        LOG_ERROR("No registered region contains ptr 0x" + std::to_string(gpu_ptr));
        return 0;
    }

    // ========================================
    // RDMA Operations
    // ========================================

    bool rdma_write(
        uint64_t local_addr,
        uint64_t remote_addr,
        size_t size,
        uint32_t qp_num,
        uint32_t rkey)
    {
        if (!rdma_engine_)
        {
            LOG_ERROR("RDMA engine not initialized");
            return false;
        }

        // Find lkey for this local address
        uint32_t lkey = get_lkey_for_ptr(local_addr);
        if (lkey == 0)
        {
            LOG_ERROR("Cannot RDMA write from unregistered memory 0x" +
                      std::to_string(local_addr));
            return false;
        }

        try
        {
            // Find peer_id from qp_num
            std::string peer_id = find_peer_id_by_qp(qp_num);
            if (peer_id.empty())
            {
                LOG_ERROR("No peer found for QP " + std::to_string(qp_num));
                return false;
            }

            // Calculate offsets
            size_t src_offset = local_addr - get_base_ptr_for_lkey(lkey);
            size_t dst_offset = remote_addr; // Assume this is already an offset

            bool success = rdma_engine_->post_write(
                peer_id,
                src_offset,
                dst_offset,
                size,
                0,   // imm_data
                true // signal
            );

            if (!success)
            {
                LOG_ERROR("RDMA write failed");
                return false;
            }

            LOG_DEBUG("RDMA write posted: " + std::to_string(size) + " bytes");
            return true;
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("RDMA write exception: " + std::string(e.what()));
            return false;
        }
    }

    bool rdma_wait_completion(int timeout_ms = 0)
    {
        if (!rdma_engine_)
        {
            LOG_ERROR("RDMA engine not initialized");
            return false;
        }

        try
        {
            int completed = rdma_engine_->poll_send_cq(1);
            LOG_DEBUG("RDMA operations completed: " + std::to_string(completed));
            return completed > 0;
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("RDMA wait exception: " + std::string(e.what()));
            return false;
        }
    }

    py::dict get_stats()
    {
        py::dict stats;
        stats["initialized"] = (rdma_engine_ != nullptr);
        stats["num_registered_regions"] = registered_regions_.size();
        return stats;
    }

    // ========================================
    // Decode Node: Pre-allocated Staging Buffer
    // ========================================

    /**
     * Get pre-allocated staging buffer pointer (for decode node)
     */
    uint64_t get_staging_buffer_ptr()
    {
        if (!rdma_engine_)
        {
            LOG_ERROR("RDMA engine not initialized");
            return 0;
        }
        return (uint64_t)rdma_engine_->get_gpu_ptr();
    }

    size_t get_staging_buffer_size()
    {
        if (!rdma_engine_)
        {
            return 0;
        }
        return rdma_engine_->get_memory_size();
    }

private:
    struct RegisteredRegion
    {
        uint64_t gpu_ptr;
        size_t size;
        ibv_mr *mr;
        uint32_t lkey;
        uint32_t rkey;
    };

    std::unique_ptr<RDMAEngine> rdma_engine_;
    RdmaContext *ctx_ = nullptr;
    std::map<std::string, RegisteredRegion> registered_regions_;

    Config create_default_config()
    {
        // Create minimal config
        Config config;
        config.rdma.ib_port = 1;
        config.rdma.gid_index = 0;
        config.memory.kv_buffer_mb = 1024; // 1GB staging buffer
        return config;
    }

    std::string find_peer_id_by_qp(uint32_t qp_num)
    {
        // TODO: Implement QP → peer_id lookup
        return "decode-01"; // Placeholder
    }

    uint64_t get_base_ptr_for_lkey(uint32_t lkey)
    {
        for (const auto &[name, region] : registered_regions_)
        {
            if (region.lkey == lkey)
            {
                return region.gpu_ptr;
            }
        }
        return 0;
    }
};

// ========================================
// pybind11 module definition
// ========================================

PYBIND11_MODULE(rdma_bindings, m)
{
    m.doc() = "Python bindings for C++ RDMA engine (GPUDirect RDMA)";

    py::class_<RDMABindings>(m, "RDMABindings")
        .def(py::init<>())
        .def("initialize", &RDMABindings::initialize,
             py::arg("device_name"),
             py::arg("port") = 1,
             py::arg("gid_index") = 0,
             "Initialize RDMA engine")
        .def("register_gpu_memory", &RDMABindings::register_gpu_memory,
             py::arg("gpu_ptr"),
             py::arg("size"),
             py::arg("name") = "unnamed",
             "Register external GPU memory with RDMA")
        .def("get_lkey_for_ptr", &RDMABindings::get_lkey_for_ptr,
             py::arg("gpu_ptr"),
             "Get lkey for a GPU pointer")
        .def("rdma_write", &RDMABindings::rdma_write,
             py::arg("local_addr"),
             py::arg("remote_addr"),
             py::arg("size"),
             py::arg("qp_num"),
             py::arg("rkey"),
             "Perform RDMA write operation")
        .def("rdma_wait_completion", &RDMABindings::rdma_wait_completion,
             py::arg("timeout_ms") = 0,
             "Wait for RDMA operations to complete")
        .def("get_staging_buffer_ptr", &RDMABindings::get_staging_buffer_ptr,
             "Get pre-allocated staging buffer pointer (decode node)")
        .def("get_staging_buffer_size", &RDMABindings::get_staging_buffer_size,
             "Get staging buffer size (decode node)")
        .def("get_stats", &RDMABindings::get_stats,
             "Get RDMA statistics");

    m.attr("__version__") = "1.0.0";
}