#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "kv_cache/kv_transfer_engine.h"

namespace py = pybind11;
using namespace rdma_kv;

// Global singleton (set by C++ daemon at startup)
static std::shared_ptr<kv_cache::KVTransferEngine> g_kv_engine;

void set_global_kv_engine(std::shared_ptr<kv_cache::KVTransferEngine> engine)
{
    g_kv_engine = engine;
}

PYBIND11_MODULE(rdma_kv_bindings, m)
{
    m.doc() = "RDMA KV Cache Transfer Engine Python Bindings";

    // Setter for global engine (called from C++ daemon)
    m.def("set_global_kv_engine", &set_global_kv_engine,
          "Set the global KV transfer engine instance");

    // Getter for Python connector
    m.def("get_global_kv_engine", []() -> kv_cache::KVTransferEngine *
          {
          if (!g_kv_engine) {
              throw std::runtime_error("KV engine not initialized. "
                                     "Ensure C++ daemon is running.");
          }
          return g_kv_engine.get(); }, py::return_value_policy::reference, "Get the global KV transfer engine instance");

    // KVTransferEngine class
    py::class_<kv_cache::KVTransferEngine>(m, "KVTransferEngine")
        .def("register_kv_layer",
             &kv_cache::KVTransferEngine::register_kv_layer,
             py::arg("layer_name"),
             py::arg("gpu_ptr"),
             py::arg("size_bytes"),
             py::arg("shape"),
             "Register a KV cache layer from PyTorch")

        .def("save_kv_layer_async",
             &kv_cache::KVTransferEngine::save_kv_layer_async,
             py::arg("request_id"),
             py::arg("layer_name"),
             py::arg("block_ids"),
             py::call_guard<py::gil_scoped_release>(),
             "Save KV cache layer asynchronously via RDMA")

        .def("wait_for_layer_load",
             &kv_cache::KVTransferEngine::wait_for_layer_load,
             py::arg("request_id"),
             py::arg("layer_name"),
             py::call_guard<py::gil_scoped_release>(),
             "Wait for KV cache layer to be loaded")

        .def("wait_for_all_saves",
             &kv_cache::KVTransferEngine::wait_for_all_saves,
             py::call_guard<py::gil_scoped_release>(),
             "Wait for all pending saves to complete")

        .def("get_stats",
             &kv_cache::KVTransferEngine::get_stats,
             "Get transfer statistics");

    // Stats struct
    py::class_<kv_cache::KVTransferEngine::Stats>(m, "KVTransferStats")
        .def_readonly("total_layers_transferred",
                      &kv_cache::KVTransferEngine::Stats::total_layers_transferred)
        .def_readonly("total_bytes_transferred",
                      &kv_cache::KVTransferEngine::Stats::total_bytes_transferred)
        .def_readonly("avg_layer_transfer_time_ms",
                      &kv_cache::KVTransferEngine::Stats::avg_layer_transfer_time_ms)
        .def_readonly("avg_bandwidth_gbps",
                      &kv_cache::KVTransferEngine::Stats::avg_bandwidth_gbps);
}
