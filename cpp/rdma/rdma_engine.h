#pragma once
#include "cpp/common/types.h"
#include "cpp/common/config.h"

class RDMAEngine
{

public:
    RDMAEngine(Config &config, NodeInfo &node_info_);

    bool initialize();
    void shutdown();
    bool is_initialized();

private:
    std::atomic<bool> initialized_;

    NodeInfo &node_info_;
    Config &config_;
};