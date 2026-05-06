// cpp/nodes/node_accessor.cpp
// C++ implementation of node accessor functions

#include "cpp/nodes/node.h"
#include "cpp/rdma/rdma_engine.h"
#include "cpp/bindings/node_accessor.h"

static Node *g_current_node = nullptr;

void set_current_node(Node *node)
{
    g_current_node = node;
}

RDMAEngine *get_node_rdma_engine()
{
    return g_current_node ? g_current_node->get_rdma_engine() : nullptr;
}
