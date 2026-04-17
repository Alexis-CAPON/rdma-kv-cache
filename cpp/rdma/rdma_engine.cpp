#include "cpp/rdma/rdma_engine.h"
#include "cpp/common/config.h"
#include "cpp/common/logger.h"

RDMAEngine::RDMAEngine(Config &config, NodeInfo &node_info) : initialized_(false), config_(config), node_info_(node_info)
{
}

bool RDMAEngine::initialize()
{
    Logger::info("RDMATransport: Initializing with any available RDMA device");
}