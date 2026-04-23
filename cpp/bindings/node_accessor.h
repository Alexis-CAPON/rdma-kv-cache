#pragma once

// Forward declarations
class Node;
class RDMAEngine;

/**
 * Set the current node instance for Python bindings
 * This allows vLLM's Python connector to access the Node's RDMA engine
 */
void set_current_node(Node *node);

/**
 * Get the RDMA engine from the current node
 * Returns nullptr if no node is set
 */
RDMAEngine *get_node_rdma_engine();
