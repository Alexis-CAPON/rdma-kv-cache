// cpp/bindings/node_accessor.cpp
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
    m.def("get_rdma_engine_ptr", &get_node_rdma_engine,
          py::return_value_policy::reference);
}