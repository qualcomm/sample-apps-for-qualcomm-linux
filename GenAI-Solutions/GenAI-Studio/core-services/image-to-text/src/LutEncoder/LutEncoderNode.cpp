// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================

#include "LutEncoderNode.h"
#include <iostream>
#include <cstring>

LutEncoderNode* LutEncoderNode::s_activeInstance_ = nullptr;

LutEncoderNode::LutEncoderNode(const fs::path& textEncJsonPath) {
    mustExistFile(textEncJsonPath);
    std::string json = readTextFile(textEncJsonPath);

    GENIE_CHECK(GenieNodeConfig_createFromJson(json.c_str(), &cfg_));
    GENIE_CHECK(GenieNode_create(cfg_, &node_));
}

LutEncoderNode::~LutEncoderNode() {
    if (node_) GenieNode_free(node_);
    if (cfg_)  GenieNodeConfig_free(cfg_);
}

void LutEncoderNode::setTextInput(const std::string& text, const char* dataConfig) {
    text_ = text; 
    std::cout << "[LUT] text bytes=" <<text<< text_.size() << "\n";

    GENIE_CHECK(GenieNode_setData(
        node_,
        GENIE_NODE_TEXT_ENCODER_TEXT_INPUT,
        text_.data(),
        text_.size(),
        dataConfig
    ));
}

void LutEncoderNode::clearText() {
    std::string().swap(text_); // frees capacity
     
}

void LutEncoderNode::appendTextInput(const std::string& moreText, const char* dataConfig) {


    text_ += moreText;
    std::cout << "[LUT] text bytes=" <<text_<< text_.size() << " (appended)\n";

    GENIE_CHECK(GenieNode_setData(
        node_,
        GENIE_NODE_TEXT_ENCODER_TEXT_INPUT,
        text_.data(),
        text_.size(),
        dataConfig
    ));
}

