// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================

#include "GeniePipelineRunner.h"

GeniePipelineRunner::GeniePipelineRunner() {
    GENIE_CHECK(GeniePipelineConfig_createFromJson("{}", &pipelineConfig_));
    GENIE_CHECK(GeniePipeline_create(pipelineConfig_, &pipeline_handle_));
}

GeniePipelineRunner::~GeniePipelineRunner() {
    if (pipeline_handle_)       GeniePipeline_free(pipeline_handle_);
    if (pipelineConfig_) GeniePipelineConfig_free(pipelineConfig_);
}

void GeniePipelineRunner::addNode(GenieNode_Handle_t node) {
    GENIE_CHECK(GeniePipeline_addNode(pipeline_handle_, node));
}

void GeniePipelineRunner::connect(GenieNode_Handle_t producer,
                                 GenieNode_IOName_t producerIO,
                                 GenieNode_Handle_t consumer,
                                 GenieNode_IOName_t consumerIO) {
    GENIE_CHECK(GeniePipeline_connect(pipeline_handle_, producer, producerIO, consumer, consumerIO));
}

void GeniePipelineRunner::execute(void* userData) {
    GENIE_CHECK(GeniePipeline_execute(pipeline_handle_, userData));
}