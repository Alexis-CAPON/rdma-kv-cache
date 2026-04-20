// cpp/bindings/node_accessor.cpp
#include <pybind11/pybind11.h>
#include "cpp/nodes/node.h"
#include "cpp/rdma/rdma_engine.h"

namespace py = pybind11;

static Node *g_current_node = nullptr;

void set_current_node(Node *node)
{
    g_current_node = node;
}

RDMAEngine *get_node_rdma_engine()
{
    return g_current_node ? g_current_node->get_rdma_engine() : nullptr;
}

PYBIND11_MODULE(node_accessor, m)
{
    m.doc() = "Node accessor for accessing RDMA engine from vLLM Python connector";

    m.def("set_current_node", &set_current_node,
          py::arg("node"),
          "Set the current node instance");

    m.def("get_node_rdma_engine_ptr", &get_node_rdma_engine,
          py::return_value_policy::reference,
          "Get pointer to the node's RDMA engine");
}