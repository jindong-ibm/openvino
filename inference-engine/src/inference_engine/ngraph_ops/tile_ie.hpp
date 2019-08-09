// Copyright (C) 2018-2019 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <memory>

#include "ngraph/op/op.hpp"

namespace ngraph {
namespace op {

class TileIE : public Op {
public:
    TileIE(const std::shared_ptr<Node>& data1,
            const int64_t axis,
            const int64_t tiles);

    void validate_and_infer_types() override;

    std::shared_ptr<Node> copy_with_new_args(const NodeVector& new_args) const override;

    int64_t axis, tiles;
};

}  // namespace op
}  // namespace ngraph
