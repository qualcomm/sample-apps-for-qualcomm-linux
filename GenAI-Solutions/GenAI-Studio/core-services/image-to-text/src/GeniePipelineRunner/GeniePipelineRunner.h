// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================

#pragma once

#include <string>
#include <stdexcept>

#include "GeniePipeline.h"
#include "GenieNode.h"
#include "GenieCheck.h"   // your GENIE_CHECK macro (or wherever you keep it)

class GeniePipelineRunner {
public:
    GeniePipelineRunner();
    ~GeniePipelineRunner();

    void addNode(GenieNode_Handle_t node);

    void connect(GenieNode_Handle_t producer,
                 GenieNode_IOName_t producerIO,
                 GenieNode_Handle_t consumer,
                 GenieNode_IOName_t consumerIO);

    void execute(void* userData);

    GeniePipeline_Handle_t pipeline_handle_ = nullptr;

private:
    GeniePipelineConfig_Handle_t pipelineConfig_ = nullptr;
    // GeniePipeline_Handle_t pipeline_ = nullptr;
};