// Copyright (C) 2018-2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "shape_infer/ie_built_in_impl.hpp"

namespace InferenceEngine {
namespace ShapeInfer {

/**
 * @brief Implementation of Shape inference for Unique layer
 */
class UniqueShapeProp : public BuiltInShapeInferImpl {
public:
    explicit UniqueShapeProp(const std::string& type): BuiltInShapeInferImpl(type) {}

    void inferShapesImpl(const std::vector<Blob::CPtr>& inBlobs, const std::map<std::string, std::string>& params,
                         const std::map<std::string, Blob::Ptr>& blobs, std::vector<SizeVector>& outShapes) override {
        bool return_inverse = GetParamAsBool("return_inverse", params);
        bool return_counts = GetParamAsBool("return_counts", params);

        // compute a number of outputs
        size_t num_outputs = 1;
        if (return_counts) {
            num_outputs++;
        }
        if (return_inverse) {
            num_outputs++;
        }

        // reshape available outputs
        outShapes.resize(num_outputs);
        for (size_t i = 0; i < num_outputs; i++) {
            outShapes[i].resize(1);
            outShapes[i][0] = inShapes[0][0];
        }
    }
};

}  // namespace ShapeInfer
}  // namespace InferenceEngine
