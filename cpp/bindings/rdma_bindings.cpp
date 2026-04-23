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
    RDMABindings(RDMAEngine *existing_rdma_engine)
        : rdma_engine_(existing_rdma_engine)
    {
        if (!rdma_engine_)
        {
            throw std::runtime_error("RDMAEngine pointer is null");
        }
        if (!rdma_engine_->is_initialized())
        {
            throw std::runtime_error("RDMAEngine not initialized");
        }
        Logger::info("RDMABindings initialized with existing RDMAEngine");
    }

    ~RDMABindings()
    {
        // Deregister all memory regions
        for (auto &[name, region] : registered_regions_)
        {
            if (region.mr)
            {
                ibv_dereg_mr(region.mr);
                Logger::debug("Deregistered memory region: " + name);
            }
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
        RdmaContext &ctx = rdma_engine_->get_context();

        try
        {
            // Register memory with InfiniBand
            ibv_mr *mr = ibv_reg_mr(
                ctx.pd,
                (void *)gpu_ptr,
                size,
                IBV_ACCESS_LOCAL_WRITE |
                    IBV_ACCESS_REMOTE_WRITE |
                    IBV_ACCESS_REMOTE_READ |
                    IBV_ACCESS_RELAXED_ORDERING // Critical for GPU performance
            );

            if (!mr)
            {
                Logger::error("ibv_reg_mr failed for " + name + ": " +
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

            Logger::info("Registered GPU memory '" + name + "': " +
                         "ptr=0x" + std::to_string(gpu_ptr) +
                         " size=" + std::to_string(size / (1024 * 1024)) + "MB " +
                         "lkey=0x" + std::to_string(mr->lkey) +
                         " rkey=0x" + std::to_string(mr->rkey));

            return true;
        }
        catch (const std::exception &e)
        {
            Logger::error("register_gpu_memory exception: " + std::string(e.what()));
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
        Logger::error("No registered region contains ptr 0x" + std::to_string(gpu_ptr));
        return 0;
    }

    // ========================================
    // RDMA Operations
    // ========================================

    bool rdma_write(
        const std::string &peer_id,
        uint64_t local_addr, // Absolute address (vLLM memory)
        uint64_t remote_offset,
        size_t size,
        uint32_t qp_num,
        uint32_t rkey)
    {
        if (!rdma_engine_)
        {
            Logger::error("RDMA engine not initialized");
            return false;
        }

        // Find lkey for this local address
        uint32_t lkey = get_lkey_for_ptr(local_addr);
        if (lkey == 0)
        {
            Logger::error("Cannot RDMA write from unregistered memory 0x" +
                          std::to_string(local_addr));
            return false;
        }

        try
        {

            bool success = rdma_engine_->post_write_external(
                peer_id,
                local_addr,
                lkey,
                remote_offset,
                size,
                0,   // imm_data
                true // signal
            );

            if (!success)
            {
                Logger::error("RDMA write failed");
                return false;
            }

            Logger::debug("RDMA write posted: " + std::to_string(size) + " bytes");
            return true;
        }
        catch (const std::exception &e)
        {
            Logger::error("RDMA write exception: " + std::string(e.what()));
            return false;
        }
    }

    bool rdma_wait_completion(int timeout_ms = 0)
    {
        if (!rdma_engine_)
        {
            Logger::error("RDMA engine not initialized");
            return false;
        }

        try
        {
            int completed = rdma_engine_->poll_send_cq(1);
            Logger::debug("RDMA operations completed: " + std::to_string(completed));
            return completed > 0;
        }
        catch (const std::exception &e)
        {
            Logger::error("RDMA wait exception: " + std::string(e.what()));
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
            Logger::error("RDMA engine not initialized");
            return 0;
        }
        return (uint64_t)rdma_engine_->get_gpu_ptr();
    }

    size_t get_staging_buffer_size()
    {
        if (!rdma_engine_)
        {
            Logger::error("RDMA engine not initialized");
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

    RDMAEngine *rdma_engine_;
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
        .def(py::init<RDMAEngine *>(),
             py::arg("rdma_engine"),
             "Initialize with existing RDMAEngine instance")
        .def("register_gpu_memory", &RDMABindings::register_gpu_memory,
             py::arg("gpu_ptr"),
             py::arg("size"),
             py::arg("name") = "unnamed",
             "Register external GPU memory with RDMA")
        .def("get_lkey_for_ptr", &RDMABindings::get_lkey_for_ptr,
             py::arg("gpu_ptr"),
             "Get lkey for a GPU pointer")
        .def("rdma_write", &RDMABindings::rdma_write,
             py::arg("peer_id"),
             py::arg("local_addr"),
             py::arg("remote_offset"),
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