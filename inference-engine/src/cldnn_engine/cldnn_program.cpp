// Copyright (C) 2018-2019 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <list>
#include <set>
#include <unordered_set>
#include <sstream>
#include <functional>
#include <CPP/cldnn_defs.h>
#include <CPP/data.hpp>
#include <CPP/input_layout.hpp>
#include <CPP/reorder.hpp>
#include <CPP/convolution.hpp>
#include <CPP/binary_convolution.hpp>
#include <CPP/pooling.hpp>
#include <CPP/lrn.hpp>
#include <CPP/fully_connected.hpp>
#include <CPP/softmax.hpp>
#include <CPP/activation.hpp>
#include <CPP/concatenation.hpp>
#include <CPP/proposal.hpp>
#include <CPP/roi_pooling.hpp>
#include <CPP/scale.hpp>
#include <CPP/crop.hpp>
#include <CPP/deconvolution.hpp>
#include <CPP/prior_box.hpp>
#include <CPP/detection_output.hpp>
#include <CPP/normalize.hpp>
#include <CPP/reshape.hpp>
#include <CPP/batch_norm.hpp>
#include <CPP/permute.hpp>
#include <CPP/split.hpp>
#include <CPP/upsampling.hpp>
#include <CPP/network.hpp>
#include <CPP/profiling.hpp>
#include <CPP/custom_gpu_primitive.hpp>
#include <CPP/reorg_yolo.hpp>
#include <CPP/region_yolo.hpp>
#include <CPP/mutable_data.hpp>
#include <CPP/max_unpooling.hpp>
#include <CPP/arg_max_min.hpp>
#include <CPP/mvn.hpp>
#include <CPP/tile.hpp>
#include <CPP/border.hpp>
#include <CPP/gather.hpp>
#include <CPP/depth_to_space.hpp>
#include <CPP/shuffle_channels.hpp>
#include <CPP/strided_slice.hpp>
#include <CPP/reverse_sequence.hpp>
#include <CPP/quantize.hpp>
#include <CPP/broadcast.hpp>
#include <CPP/gemm.hpp>
#include <CPP/reduce.hpp>
#include <CPP/one_hot.hpp>
#include <chrono>
#include <cmath>
#include <algorithm>
#include "cldnn_program.h"
#include "simple_math.h"
#include <description_buffer.hpp>
#include <cldnn/cldnn_config.hpp>
#include <graph_tools.hpp>
#include <ie_layers_internal.hpp>
#include <net_pass.h>
#include "cldnn_infer_request.h"
#include <cpp_interfaces/ie_executor_manager.hpp>
#include "details/caseless.hpp"
#include <fstream>
#include <utility>
#include <sys/types.h>
#include <sys/stat.h>
#include <exec_graph_info.hpp>
#include "cnn_network_int8_normalizer.hpp"

using namespace InferenceEngine;
using namespace InferenceEngine::details;

namespace CLDNNPlugin {

const cldnn::primitive_id Program::m_preProcessTag("_cldnn_input_preprocess");
const cldnn::primitive_id Program::m_weightsTag("_cldnn_weights");
const cldnn::primitive_id Program::m_biasesTag("_cldnn_biases");
const cldnn::primitive_id Program::m_meanValuesTag("_cldnn_mean_values");
const cldnn::primitive_id Program::m_postProcessTag("_cldnn_output_postprocess");
const cldnn::primitive_id Program::m_scalesTag("_cldnn_scales");
const cldnn::primitive_id Program::m_preCustomLayerTag("_cldnn_custom_preprocess");
const cldnn::primitive_id Program::m_postCustomLayerTag("_cldnn_custom_postprocess");

static void ValidateLayer(const InferenceEngine::CNNLayerPtr& layer, unsigned inputs) {  // todo: add more checks
    if (inputs && layer->insData.size() != inputs) {
        THROW_CLDNN_EXCEPTION("Invalid number of inputs for layer: " << layer->name);
    }
    if (layer->_fusedWith) {
        THROW_CLDNN_EXCEPTION("Unsupported fuse in layer: " << layer->name << " with: " << layer->_fusedWith->name);
    }
}

static void ValidateEltwiseLayer(const InferenceEngine::CNNLayerPtr& layer) {
    if (layer->_fusedWith) {
        THROW_CLDNN_EXCEPTION("Unsupported fuse in layer: " << layer->name << " with: " << layer->_fusedWith->name);
    }
}

static InferenceEngine::Blob::Ptr getBlobOrNull(const InferenceEngine::CNNLayerPtr& layer, std::string name) {
    auto result = layer->blobs.find(name);
    if (result != layer->blobs.end()) {
        return result->second;
    } else {
        return nullptr;
    }
}

static InferenceEngine::Blob::Ptr getBlob(const InferenceEngine::CNNLayerPtr& layer, std::string name) {
    auto result = getBlobOrNull(layer, name);
    if (result == nullptr) {
        THROW_CLDNN_EXCEPTION("Missing blob " << name << " in layer " << layer->name);
    }

    return result;
}

static cldnn::format defaultFormatForDims(size_t dimensions) {
    switch (dimensions) {
    case 1:
    case 2:
    case 3:
    case 4:
        return cldnn::format::bfyx;
    case 5:
        return cldnn::format::bfzyx;
    case 6:
        return cldnn::format::bfwzyx;
    default:
        THROW_CLDNN_EXCEPTION("Unsupported number of dimensions: " << dimensions);
    }

    return cldnn::format::bfyx;  // Should not get here
}


#if defined(_WIN32)
#define mkdir(dir, mode) _mkdir(dir)
#endif

void Program::changeInputBatch(int batch) {
    m_curBatch = batch;
}

bool Program::CanProcessDynBatch(InferenceEngine::ICNNNetwork &network) const {
    InputsDataMap inputs;
    network.getInputsInfo(inputs);

    CNNLayerSet inputLayers;
    std::unordered_set<CNNLayer *> allLayers;

    if (inputs.empty())
        return false;

    auto & secondLayers = inputs.begin()->second->getInputData()->getInputTo();
    if (secondLayers.empty())
        return false;

    bool check_result = true;
    details::UnorderedDFS(allLayers, secondLayers.begin()->second, [&](CNNLayerPtr layer) {
        auto type = LayerTypeFromStr(layer->type);
        if (SimplerNMS == type ||
            ROIPooling == type ||
            PriorBox == type ||
            DetectionOutput == type ||
            Reshape == type ||
            Permute == type ||
            Flatten == type ||
            Proposal == type ||
            PSROIPooling == type ) {
            check_result = false;
        }

        // check for custom layer
        auto customLayer = m_config.customLayers.find(layer->type);
        if (customLayer != m_config.customLayers.end()) {
            check_result = false;
        }
    }, false);

    return check_result;
}

Program::Program(InferenceEngine::ICNNNetwork& network, std::shared_ptr<const cldnn::engine> engine, const Config& config)
    : m_config(config)
    , m_defaultFormat(cldnn::format::bfyx)
    , m_engine(engine)
    , m_curBatch(-1)
    , p_currentOutputs({}) {
    InitFormat(network);

    if (config.enableInt8) {
        ICNNNetworkStats* pstats = nullptr;
        StatusCode s = network.getStats(&pstats, nullptr);

        // Check for FP32 main precision as further quantization of FP16 seems pointless and is not supported by normalizer
        if (s == StatusCode::OK && pstats && !pstats->isEmpty() && network.getPrecision() == Precision::FP32) {
            CNNNetworkInt8Normalizer normalizer;
            normalizer.NormalizeNetwork(network, *pstats);
        }
    }

    NetPass::CombineRNNSeq(network);
    for (int i = 0; i < 2; i++) {
        NetPass::UnrollTI(network);
        NetPass::UnrollRNN_if(network, [](const RNNCellBase &rnn) -> bool {
            if (rnn.clip != 0.0f)
                return true;
            if (rnn.type == "GRUCell" ||
                rnn.type == "GRUSequence" ||
                rnn.type == "RNNCell" ||
                rnn.type == "RNNSequence")
                return true;
            if (!(rnn.type == "LSTMCell" || rnn.type == "LSTMSequence") ||
                rnn.activations == std::vector<std::string>{"sigmoid", "tanh", "tanh"})
                return false;
            return true;
        });
    }

    if (m_config.max_dynamic_batch > 1) {
        // check topology for applicability
        if (!CanProcessDynBatch(network)) {
            THROW_CLDNN_EXCEPTION("Such topology cannot be compiled for dynamic batch!");
        }
    }

    int m_bv_sz = GetMaxBatchSizeForSingleProgram();

    m_max_batch = config.max_dynamic_batch;

    if (config.max_dynamic_batch > 1) {
        for (int b = m_bv_sz - 1; b >= 0; b--) {
            inputLayouts.clear();
            outputDims.clear();
            primitiveIDs.clear();
            blobMemCache.clear();

            changeInputBatch(1 << b);
            m_programs.insert(m_programs.begin(), BuildProgram(network));
            m_engine->release_pending_memory(0);
        }
    } else {
        m_programs.emplace_back(BuildProgram(network));
        m_engine->release_pending_memory(0);
    }
}

int Program::GetMaxBatchSizeForSingleProgram() {
    if (m_config.max_dynamic_batch > 1) {
        // calculate number of networks necessary based on binary log
        unsigned int tmp = m_config.max_dynamic_batch;
        unsigned int mask = 1 << 31;
        unsigned int ldigit = 31;

        while (!(tmp & mask)) {
            mask >>= 1;
            ldigit--;
        }

        return ldigit + 1;
    }

    return 0;
}

std::shared_ptr<cldnn::program> Program::getCompiledProgram(int program_id) {
    if (program_id >= m_programs.size())
        THROW_CLDNN_EXCEPTION("Invalid program ID");

    return m_programs[program_id];
}

std::vector<InferenceEngine::CNNLayerPtr> Program::GetNextLayers(const InferenceEngine::DataPtr data) {
    std::vector<InferenceEngine::CNNLayerPtr> nextLayers;
    if (data == nullptr) {
        return nextLayers;
    }
    for (auto nl : data->getInputTo()) {
        nextLayers.push_back(nl.second);
    }
    return nextLayers;
}

std::vector<InferenceEngine::CNNLayerPtr> Program::GetNextLayers(const InferenceEngine::CNNLayerPtr layer) {
    std::vector<InferenceEngine::CNNLayerPtr> nextLayers;
    if (layer == nullptr) {
        return nextLayers;
    }
    for (auto od : layer->outData) {
        auto nextLayersVec = GetNextLayers(od);
        for (auto nl : nextLayersVec) {
            nextLayers.push_back(nl);
        }
    }
    return nextLayers;
}

InferenceEngine::CNNLayerPtr Program::GetNextSingleLayer(const InferenceEngine::DataPtr data) {
    if (data == nullptr) {
        return nullptr;
    }
    auto nextLayers = GetNextLayers(data);
    IE_ASSERT(nextLayers.size() == 1);
    return nextLayers[0];
}

InferenceEngine::CNNLayerPtr Program::GetNextSingleLayer(const InferenceEngine::CNNLayerPtr layer) {
    if (layer == nullptr) {
        return nullptr;
    }
    auto nextLayers = GetNextLayers(layer);
    IE_ASSERT(nextLayers.size() == 1);
    return nextLayers[0];
}

void Program::InitFormat(InferenceEngine::ICNNNetwork &network) {
    m_defaultFormat = FormatFromLayout(InferenceEngine::Layout::NCHW);
}

std::shared_ptr<cldnn::program> Program::BuildProgram(InferenceEngine::ICNNNetwork &network) {
    cldnn::build_options options;
    if (!m_config.graph_dumps_dir.empty()) {
        options.set_option(cldnn::build_option::graph_dumps_dir(m_config.graph_dumps_dir));
    }
    options.set_option(cldnn::build_option::optimize_data(true));
    options.set_option(cldnn::build_option::tuning_config(m_config.tuningConfig));

    cldnn::topology topology;

    // 1. create inputs
    InferenceEngine::InputsDataMap networkInputs;
    network.getInputsInfo(networkInputs);

    InferenceEngine::OutputsDataMap networkOutputs;
    network.getOutputsInfo(networkOutputs);
    p_currentOutputs = networkOutputs;

    if (networkInputs.empty()) {
        THROW_CLDNN_EXCEPTION("No inputs detected.");
    }

    using LayerVect = std::vector<InferenceEngine::CNNLayerPtr>;
    std::list<InferenceEngine::CNNLayerPtr> layersToHandle;

    auto push_if = [&](const LayerVect& clist) {
        for (auto& l : clist) {
            if ( (std::find_if( layersToHandle.begin(),
                            layersToHandle.end(),
                            [&](const CNNLayerPtr& x) { return layer_type_name_ID(x) == layer_type_name_ID(l); } )) == layersToHandle.end() )
                layersToHandle.push_back(l);
        }
    };

    auto allInputs = CNNNetGetAllInputLayers(network);
    for (auto input : allInputs) {
        if (LayerTypeFromStr(input->type) == ConstantBlob) {
            AddConstantBlobInput(topology, input);
        } else {
            auto iter = networkInputs.find(input->name);    // regular input
            if (iter != networkInputs.end()) {
                AddInputPrimitive(topology, iter->second, input->precision, layer_type_name_ID(input));
            }
        }
        // collect next layers to process
        push_if(GetNextLayers(input));
    }

    // 2. traverse layers
    unsigned infLoopProtection = 0;
    while (!layersToHandle.empty()) {
        if (infLoopProtection++ >= layersToHandle.size()) {
            THROW_CLDNN_EXCEPTION("Infinite loop during network creation");
            break;
        }
        InferenceEngine::CNNLayerPtr currLayer = layersToHandle.front();
        layersToHandle.pop_front();
        auto layerName = layer_type_name_ID(currLayer);

        if (primitiveIDs.find(layerName) != primitiveIDs.end()) {
            infLoopProtection = 0;
            continue;  // this layer was already added (had multiple inputs)
        }

        bool missingInput = false;
        try {
            GetPrevLayersPrimitives(currLayer);
        } catch (std::exception) {
            missingInput = true;
        }

        if (missingInput) {  // some inputs aren't created yet
            layersToHandle.push_back(currLayer);  // push the current layer to the end of the line
            continue;  // move on to the next layer
        }

        infLoopProtection = 0;  // found a layer with all inputs already existing
        CreateSingleLayerPrimitive(topology, currLayer);  // currLayer will be advanced if layer was skipped or merged
        prevPrimitiveIDs[layerName] = GetPrevLayersPrimitives(currLayer);

        push_if(GetNextLayers(currLayer));
    }

    // 3. Handle output reordering
    for (auto output : networkOutputs) {
        // always reorder and let clDNN remove unneeded reorders
        AddOutputPrimitive(topology, output.first, output.second);
    }

    // 4. ???
    // 5. profit
    p_currentOutputs.clear();

    return std::make_shared<cldnn::program>(*m_engine, topology, options);
}

Program::LayerType Program::LayerTypeFromStr(const std::string &str) {
    static const caseless_map<std::string, Program::LayerType> LayerNameToType = {
        { "Convolution" , Convolution },
        { "DeformableConvolution" , DeformableConvolution },
        { "ReLU" , ReLU },
        { "ReLU6" , ReLU6 },
        { "Sigmoid" , Sigmoid },
        { "Logistic" , Sigmoid },
        { "TanH" , TanH },
        { "ELU" , ELU },
        { "Activation" , Activation },
        { "Exp" , Exp },
        { "Not" , Not },
        { "Norm" , LRN },
        { "Pooling" , Pooling },
        { "FullyConnected" , FullyConnected },
        { "SoftMax" , SoftMax },
        { "Power" , Power },
        { "Split" , Split },
        { "Slice" , Split },
        { "Concat" , Concatenate },
        { "Eltwise" , Eltwise },
        { "SimplerNMS" , SimplerNMS },
        { "ROIPooling" , ROIPooling },
        { "Crop" , Crop },
        { "Deconvolution" , Deconvolution },
        { "PriorBox" , PriorBox },
        { "DetectionOutput" , DetectionOutput },
        { "Normalize" , Normalize },
        { "Reshape" , Reshape },
        { "Permute" , Permute },
        { "Flatten" , Flatten },
        { "BatchNormalization" , BatchNormalization },
        { "PReLU" , PReLU },
        { "ScaleShift" , ScaleShift },
        { "Proposal" , Proposal },
        { "PSROIPooling" , PSROIPooling },
        { "Clamp" , Clamp },
        { "Copy" , Copy },
        { "Upsampling" , Upsampling },
        { "Resample" , Resample },
        { "RegionYolo" , RegionYolo },
        { "ReorgYolo" , ReorgYolo },
        { "Const" , ConstantBlob },
        { "ArgMax" , ArgMax },
        { "ArgMin" , ArgMin },
        { "MVN" , MVN },
        { "Unpooling" , Unpooling },
        { "Tile" , Tile },
        { "Pad" , Pad },
        { "LSTMCell" , LSTMCell },
        { "LSTMSequence" , RNN },
        { "RNNSequence" , RNN },
        { "Gather" , Gather },
        { "DepthToSpace" , DepthToSpace },
        { "ShuffleChannels" , ShuffleChannels },
        { "StridedSlice" , StridedSlice },
        { "ReverseSequence" , ReverseSequence },
        { "BinaryConvolution" , BinaryConvolution },
        { "Quantize" , Quantize },
        { "Broadcast" , Broadcast },
        { "Squeeze" , Squeeze },
        { "Unsqueeze" , Unsqueeze },
        { "ReduceMax" , Reduce },
        { "ReduceMin" , Reduce },
        { "ReduceMean" , Reduce },
        { "ReduceProd" , Reduce },
        { "ReduceSum" , Reduce },
        { "ReduceAnd" , Reduce },
        { "ReduceOr" , Reduce },
        { "ReduceSumSquare" , Reduce },
        { "ReduceL1" , Reduce },
        { "ReduceL2" , Reduce },
        { "ReduceLogSum" , Reduce },
        { "ReduceLogSumExp" , Reduce },
        { "TopK" , TopK },
        { "Asin" , Asin },
        { "Atan" , Atan },
        { "Acos" , Acos },
        { "Abs" , Abs },
        { "Acosh" , Acosh },
        { "Atanh" , Atanh },
        { "Floor" , Floor },
        { "Ceil" , Ceil },
        { "Erf" , Erf },
        { "HardSigmoid" , HardSigmoid },
        { "Log" , Log },
        { "Neg" , Neg },
        { "Reciprocal" , Reciprocal },
        { "Selu" , Selu },
        { "Sign" , Sign },
        { "SoftPlus" , SoftPlus },
        { "SoftSign" , SoftSign },
        { "Tan" , Tan },
        { "GEMM", Gemm },
        { "OneHot", OneHot}
    };
    auto it = LayerNameToType.find(str);
    if (it != LayerNameToType.end())
        return it->second;
    else
        return NO_TYPE;
}

cldnn::pooling_mode Program::PoolingModeFromIEPooling(InferenceEngine::PoolingLayer::PoolType pt, bool excludePadding) {
    switch (pt) {
        case InferenceEngine::PoolingLayer::PoolType::MAX:
            return cldnn::pooling_mode::max;
        case InferenceEngine::PoolingLayer::PoolType::AVG:
            return excludePadding ? cldnn::pooling_mode::average_no_padding : cldnn::pooling_mode::average;
        default:
            THROW_CLDNN_EXCEPTION("Unsupported pooling type: " << pt);
            break;
    }

    return cldnn::pooling_mode::max;  // shouldn't get here
}

cldnn::eltwise_mode Program::EltwiseModeFromIEEltwise(InferenceEngine::EltwiseLayer::eOperation op) {
    switch (op) {
        case InferenceEngine::EltwiseLayer::Sum:
            return cldnn::eltwise_mode::sum;
        case InferenceEngine::EltwiseLayer::Prod:
            return cldnn::eltwise_mode::prod;
        case InferenceEngine::EltwiseLayer::Max:
            return cldnn::eltwise_mode::max;
        case InferenceEngine::EltwiseLayer::Sub:
            return cldnn::eltwise_mode::sub;
        case InferenceEngine::EltwiseLayer::Min:
            return cldnn::eltwise_mode::min;
        case InferenceEngine::EltwiseLayer::Div:
            return cldnn::eltwise_mode::div;
        case InferenceEngine::EltwiseLayer::Squared_diff:
            return cldnn::eltwise_mode::squared_diff;
        case InferenceEngine::EltwiseLayer::Equal:
            return cldnn::eltwise_mode::eq;
        case InferenceEngine::EltwiseLayer::Not_equal:
            return cldnn::eltwise_mode::ne;
        case InferenceEngine::EltwiseLayer::Less:
            return cldnn::eltwise_mode::lt;
        case InferenceEngine::EltwiseLayer::Less_equal:
            return cldnn::eltwise_mode::le;
        case InferenceEngine::EltwiseLayer::Greater:
            return cldnn::eltwise_mode::gt;
        case InferenceEngine::EltwiseLayer::Greater_equal:
            return cldnn::eltwise_mode::ge;
        case InferenceEngine::EltwiseLayer::Logical_AND:
            return cldnn::eltwise_mode::logic_and;
        case InferenceEngine::EltwiseLayer::Logical_OR:
            return cldnn::eltwise_mode::logic_or;
        case InferenceEngine::EltwiseLayer::Logical_XOR:
            return cldnn::eltwise_mode::logic_xor;
        case InferenceEngine::EltwiseLayer::Pow:
            return cldnn::eltwise_mode::pow;
        case InferenceEngine::EltwiseLayer::Floor_mod:
            return cldnn::eltwise_mode::floor_mod;
        default: THROW_CLDNN_EXCEPTION("Unsupported eltwise operation: " << op);
            break;
    }

    return cldnn::eltwise_mode::max;  // shouldn't get here
}

auto CldnnTensorFromIEDims = [](const InferenceEngine::SizeVector& dims, int def = 1) {
    switch (dims.size()) {
    case 1: return cldnn::tensor(cldnn::batch(dims[0]), cldnn::feature(def), cldnn::spatial(def, def));
    case 2: return cldnn::tensor(cldnn::batch(dims[0]), cldnn::feature(dims[1]), cldnn::spatial(def, def));
    case 3: return cldnn::tensor(cldnn::batch(dims[0]), cldnn::feature(dims[1]), cldnn::spatial(def, dims[2]));
    case 4: return cldnn::tensor(cldnn::batch(dims[0]), cldnn::feature(dims[1]), cldnn::spatial(dims[3], dims[2]));
    case 5: return cldnn::tensor(cldnn::batch(dims[0]), cldnn::feature(dims[1]), cldnn::spatial(dims[4], dims[3], dims[2]));
    case 6: return cldnn::tensor(cldnn::batch(dims[0]), cldnn::feature(dims[1]), cldnn::spatial(dims[5], dims[4], dims[3], dims[2]));
    default: THROW_CLDNN_EXCEPTION("Invalid dimensions size(" << dims.size() << ") for clDNN tensor");
    }
};

cldnn::primitive_id Program::CreatePrimitiveFromBlob(cldnn::topology& topology,
                                                     cldnn::primitive_id primID,
                                                     const InferenceEngine::Blob::Ptr pBlob,
                                                     const cldnn::layout& blobLayout,
                                                     size_t blobByteOffset,
                                                     WeightRearrangeType rearrange) {
// The condition below is not valid once we use groups - todo: think of some other size check here
//     if ((pBlob != nullptr) &&
//         (pBlob->size() * (broadcastFeatures ? blobLayout.size.feature[0] : 1)) != blobLayout.count()) {
//         THROW_CLDNN_EXCEPTION("Unexpected blob size");
//     }
    if (pBlob == nullptr) {
        THROW_CLDNN_EXCEPTION("Missing blob data: " << primID);
    }

    auto data = static_cast<const char *>(pBlob->buffer()) + blobByteOffset;

    auto bufIter = blobMemCache.find(data);

    if (bufIter != blobMemCache.end()) {
        return bufIter->second;
    }

    auto mem = cldnn::memory::allocate(*m_engine, blobLayout);
    auto tmpPointer = mem.pointer<char>();  // implicitly maps buffer - unmap in destructor
    auto buf = tmpPointer.data();
    auto bufSize = blobLayout.bytes_count();

    const auto descLayout = pBlob->getTensorDesc().getLayout();
    if ((descLayout != InferenceEngine::OIHW) &&
        (descLayout != InferenceEngine::NCDHW) &&
        (descLayout != InferenceEngine::NCHW) &&
        (descLayout != InferenceEngine::CHW) &&
        (descLayout != InferenceEngine::NC) &&
        (descLayout != InferenceEngine::C)) {
        // TODO: support more layouts
        THROW_CLDNN_EXCEPTION("Unsupported layout (" << DebugOptions::IELayoutToString(descLayout) << ") in blob: " << primID);
    } else if (rearrange == BroadcastFeatures) {
        size_t features = static_cast<size_t>(blobLayout.size.feature[0]);
        if (pBlob->size() != features) {
            THROW_CLDNN_EXCEPTION("Invalid blob dimensions to broadcast: " << primID);
        }
        auto elementSize = cldnn::data_type_traits::size_of(blobLayout.data_type);
        size_t featureElements = blobLayout.count() / static_cast<size_t>(blobLayout.size.feature[0]);
        IE_ASSERT(blobLayout.format == cldnn::format::bfyx);
        for (size_t f = 0; f < features; f++) {
            for (size_t e = 0; e < featureElements; e++) {
                for (size_t b = 0; b < elementSize; b++) {
                    buf[(f*featureElements + e)*elementSize + b] = data[f*elementSize + b];
                }
            }
        }
    } else if (rearrange == FlipDeconvDims) {
        auto elementSize = cldnn::data_type_traits::size_of(blobLayout.data_type);

        size_t inputFeatureElements = static_cast<size_t>(blobLayout.size.feature[0]);
        size_t outputFeatureElements = static_cast<size_t>(blobLayout.size.batch[0]);

        size_t featureSize = elementSize * blobLayout.size.spatial[0] * blobLayout.size.spatial[1];
        if (blobLayout.format == cldnn::format::bfzyx)
            featureSize *= static_cast<size_t>(blobLayout.size.spatial[2]);

        for (size_t i = 0; i < inputFeatureElements; i++) {
            for (size_t o = 0; o < outputFeatureElements; o++) {
                size_t outputShift = (o*inputFeatureElements + i)*featureSize;
                size_t inputShift = (i*outputFeatureElements + o)*featureSize;

                for (size_t b = 0; b < featureSize; b++) {
                    buf[outputShift + b] = data[inputShift + b];
                }
            }
        }
    } else {
        for (size_t i = 0; i < bufSize; i++) {
            buf[i] = data[i];
        }
    }
    topology.add(cldnn::data(primID, mem));
    blobMemCache[data] = primID;
    return primID;
}

void Program::CreateWeightAndBiasPrimitives(cldnn::topology& topology,
                                            const InferenceEngine::CNNLayerPtr& layer,
                                            std::vector<cldnn::primitive_id>& weightsPrimID,
                                            std::vector<cldnn::primitive_id>& biasesPrimID) {
    cldnn::tensor::value_type inFeatures = 1;  // todo: workaround for xyf input, handle general case (xf, xyzf etc...)
    std::shared_ptr<Data> insData0 = layer->insData[0].lock();
    IE_ASSERT(insData0 != nullptr);
    const auto in0dims = insData0->getTensorDesc().getDims();
    if (in0dims.size() > 1) {
        inFeatures = TensorValue(in0dims[1]);
    }
    cldnn::tensor::value_type outFeatures(0);
    std::vector<cldnn::tensor::value_type> weightDimsVec;
    InferenceEngine::Blob::Ptr pWeightsBlob, pBiasBlob;
    unsigned groupSize = 1;
    WeightRearrangeType rearrange = NO_REARRANGE;

    switch (LayerTypeFromStr(layer->type)) {
    case Convolution: {
        auto convLayer = as<InferenceEngine::ConvolutionLayer *> (layer);
        groupSize = convLayer->_group;
        if ((inFeatures % groupSize) || (convLayer->_out_depth % groupSize)) {
            THROW_CLDNN_EXCEPTION("Invalid group size in layer " << convLayer->name);
        }
        if (groupSize >= 16)  // cldnn optimization for 16 and more groups
            groupSize = 1;

        weightDimsVec = { TensorValue(convLayer->_out_depth / groupSize), TensorValue(inFeatures / convLayer->_group) };
        for (size_t i = 0; i < convLayer->_kernel.size(); i++) {
            weightDimsVec.push_back(TensorValue(convLayer->_kernel[i]));
        }
        outFeatures = convLayer->_out_depth;
        pWeightsBlob = getBlob(layer, "weights");
        pBiasBlob = getBlobOrNull(layer, "biases");
        break;
    }
    case Deconvolution: {
        auto deconvLayer = as<InferenceEngine::DeconvolutionLayer *> (layer);
        groupSize = deconvLayer->_group;
        if ((inFeatures % groupSize) || (deconvLayer->_out_depth % groupSize)) {
            THROW_CLDNN_EXCEPTION("Invalid group size in layer " << deconvLayer->name);
        }
        if (groupSize >= 16)  // cldnn optimization for 16 and more groups
            groupSize = 1;

        weightDimsVec = { TensorValue(deconvLayer->_out_depth / groupSize), TensorValue(inFeatures / deconvLayer->_group) };
        for (size_t i = 0; i < deconvLayer->_kernel.size(); i++) {
            weightDimsVec.push_back(TensorValue(deconvLayer->_kernel[i]));
        }
        outFeatures = deconvLayer->_out_depth;
        pWeightsBlob = getBlob(layer, "weights");
        pBiasBlob = getBlobOrNull(layer, "biases");

        if ((groupSize < outFeatures) || (groupSize < inFeatures))
            rearrange = FlipDeconvDims;
        break;
    }
    case DeformableConvolution: {
        auto defConvLayer = as<InferenceEngine::DeformableConvolutionLayer *> (layer);
        groupSize = 1;
        /*if (groupSize >= 16)  // cldnn optimization for 16 and more groups
            groupSize = 1;*/
        weightDimsVec = { TensorValue(defConvLayer->_out_depth), TensorValue(inFeatures / defConvLayer->_group) };
        for (size_t i = 0; i < defConvLayer->_kernel.size(); i++) {
            weightDimsVec.push_back(TensorValue(defConvLayer->_kernel[i]));
        }
        outFeatures = defConvLayer->_out_depth;
        pWeightsBlob = defConvLayer->_weights;
        pBiasBlob = defConvLayer->_biases;
        break;
    }
    default:
        THROW_IE_EXCEPTION << "Wrong weightable layer type";
        break;
    }

    // create weights primitive
    cldnn::format wFmt = m_defaultFormat;
    if (weightDimsVec.size() > 4)
        wFmt = cldnn::format::bfzyx;

    cldnn::layout weightsLayout = cldnn::layout(
        DataTypeFromPrecision(pWeightsBlob->getTensorDesc().getPrecision()),
        wFmt,
        cldnn::tensor(weightDimsVec));
    size_t bytesPerGroup = weightsLayout.bytes_count();

    for (unsigned g = 0; g < groupSize; g++) {
        cldnn::primitive_id weightID = layer_type_name_ID(layer) + m_weightsTag + std::to_string(g);
        weightID = CreatePrimitiveFromBlob(topology,
                                           weightID,
                                           pWeightsBlob,
                                           weightsLayout,
                                           g * bytesPerGroup,
                                           rearrange);
        weightsPrimID.push_back(weightID);
    }

    // create bias primitive
    if (pBiasBlob != nullptr) {
        cldnn::layout biasesLayout = cldnn::layout(
            DataTypeFromPrecision(pBiasBlob->getTensorDesc().getPrecision()),
            FormatFromLayout(pBiasBlob->getTensorDesc().getLayout()),
            (cldnn::tensor) cldnn::spatial(TensorValue(outFeatures / groupSize)));
        size_t bytesPerGroup = biasesLayout.bytes_count();
        for (unsigned g = 0; g < groupSize; g++) {
            cldnn::primitive_id biasID = layer_type_name_ID(layer) + m_biasesTag + std::to_string(g);
            biasID = CreatePrimitiveFromBlob(topology,
                                             biasID,
                                             pBiasBlob,
                                             biasesLayout,
                                             g * bytesPerGroup);
            biasesPrimID.push_back(biasID);
        }
    }
}

void Program::CreateBinaryWeightAndBiasPrimitives(cldnn::topology& topology,
                                                  const InferenceEngine::CNNLayerPtr& layer,
                                                  std::vector<cldnn::primitive_id>& weightsPrimID,
                                                  std::vector<cldnn::primitive_id>& biasesPrimID) {
    cldnn::tensor::value_type inFeatures = 1;  // todo: workaround for xyf input, handle general case (xf, xyzf etc...)
    std::shared_ptr<Data> insData0 = layer->insData[0].lock();
    IE_ASSERT(insData0 != nullptr);
    const auto in0dims = insData0->getTensorDesc().getDims();
    if (in0dims.size() > 1) {
        inFeatures = TensorValue(in0dims[1]);
    }
    std::vector<cldnn::tensor::value_type> weightDimsVec;
    InferenceEngine::Blob::Ptr pWeightsBlob, pBiasBlob;
    uint32_t groupSize = 1;
    WeightRearrangeType rearrange = NO_REARRANGE;

    switch (LayerTypeFromStr(layer->type)) {
    case BinaryConvolution: {
        auto binaryConvLayer = as<InferenceEngine::BinaryConvolutionLayer*>(layer);
        groupSize = binaryConvLayer->_group;
        if ((inFeatures % groupSize) || (binaryConvLayer->_out_depth % groupSize)) {
            THROW_CLDNN_EXCEPTION("Invalid group size in layer " << binaryConvLayer->name);
        }
        weightDimsVec = {
                TensorValue(binaryConvLayer->_out_depth),
                TensorValue(inFeatures),
                TensorValue(binaryConvLayer->_kernel[X_AXIS]),
                TensorValue(binaryConvLayer->_kernel[Y_AXIS])
        };
        pWeightsBlob = binaryConvLayer->_weights;
        pBiasBlob = binaryConvLayer->_biases;
        break;
    }
    default:
        THROW_CLDNN_EXCEPTION("Wrong binary weightable layer type");
    }

    // create weights primitive
    cldnn::layout weightsLayout = cldnn::layout(
        cldnn::data_types::bin,
        cldnn::format::bfyx,
        cldnn::tensor(weightDimsVec));

    cldnn::primitive_id weightID = layer->name + m_weightsTag;
    weightID = CreatePrimitiveFromBlob(topology,
                                       weightID,
                                       pWeightsBlob,
                                       weightsLayout,
                                       0,
                                       rearrange);
    weightsPrimID.push_back(weightID);

    // create bias primitive
    if (pBiasBlob != nullptr) {
        THROW_CLDNN_EXCEPTION("Biases are not supported in BinaryConvolution primitive");
    }
}

void Program::CreateScaleWeightsAndBiasesFromBN(cldnn::topology& topology,
                                                const InferenceEngine::BatchNormalizationLayer* bnLayer,
                                                cldnn::primitive_id& weightsPrimID,
                                                cldnn::primitive_id& biasesPrimID) {
    auto weightTD = bnLayer->_weights->getTensorDesc();
    auto biasTD = bnLayer->_biases->getTensorDesc();
    {
        if (weightTD.getDims() != biasTD.getDims()) {
            THROW_CLDNN_EXCEPTION("mean/variance dimensions mismatch in " << bnLayer->name);
        }
        if (weightTD.getPrecision() != biasTD.getPrecision()) {
            THROW_CLDNN_EXCEPTION("mean/variance precision mismatch in " << bnLayer->name);
        }
    }

    cldnn::tensor blobTensor(0);
    auto outDims = bnLayer->outData[0]->getTensorDesc().getDims();
    if (outDims.size() != 2 && outDims.size() != 4) {
        THROW_CLDNN_EXCEPTION("Batch normalization input doesn't have 2 or 4 dimensions in " << bnLayer->name);
    }
    blobTensor = (cldnn::tensor) cldnn::feature(TensorValue(outDims[1]));
    cldnn::layout blobLayout(
        DataTypeFromPrecision(bnLayer->precision),
        m_defaultFormat,
        blobTensor);

    const auto wPecision = bnLayer->_weights->getTensorDesc().getPrecision();

    switch (wPecision) {
    case Precision::FP16: {
        InferenceEngine::TBlob<uint16_t> weightsBlob(bnLayer->_weights->getTensorDesc());
        weightsBlob.allocate();
        InferenceEngine::TBlob<uint16_t> biasesBlob(bnLayer->_biases->getTensorDesc());
        biasesBlob.allocate();

        auto weightsData = weightsBlob.data();
        auto biasesData = biasesBlob.data();
        auto varianceData = static_cast<const uint16_t *>(bnLayer->_weights->buffer());
        auto meanData = static_cast<const uint16_t *>(bnLayer->_biases->buffer());

        cldnn_status status = CLDNN_SUCCESS;
        for (size_t i = 0; i < weightsBlob.size(); i++) {
            auto variance = cldnn_half_to_float(varianceData[i], &status);
            if (status != CLDNN_SUCCESS) THROW_CLDNN_EXCEPTION("Error during fp16 conversion for layer " << bnLayer->name);
            auto mean = cldnn_half_to_float(meanData[i], &status);
            if (status != CLDNN_SUCCESS) THROW_CLDNN_EXCEPTION("Error during fp16 conversion for layer " << bnLayer->name);

            float scale = 1.0f / sqrt(variance + bnLayer->epsilon);
            weightsData[i] = cldnn_float_to_half(scale, &status);
            if (status != CLDNN_SUCCESS) THROW_CLDNN_EXCEPTION("Error during fp16 conversion for layer " << bnLayer->name);
            biasesData[i] = cldnn_float_to_half((-mean) * scale, &status);
            if (status != CLDNN_SUCCESS) THROW_CLDNN_EXCEPTION("Error during fp16 conversion for layer " << bnLayer->name);
        }
        weightsPrimID = CreatePrimitiveFromBlob(topology, weightsPrimID,
                                                std::make_shared<InferenceEngine::TBlob<uint16_t>>(weightsBlob), blobLayout);
        biasesPrimID = CreatePrimitiveFromBlob(topology, biasesPrimID,
                                               std::make_shared<InferenceEngine::TBlob<uint16_t>>(biasesBlob), blobLayout);
    }
        break;
    case Precision::FP32: {
        InferenceEngine::TBlob<float> weightsBlob(bnLayer->_weights->getTensorDesc());
        weightsBlob.allocate();
        InferenceEngine::TBlob<float> biasesBlob(bnLayer->_biases->getTensorDesc());
        biasesBlob.allocate();

        auto weightsData = weightsBlob.data();
        auto biasesData = biasesBlob.data();
        auto varianceData = static_cast<const float *>(bnLayer->_weights->buffer());
        auto meanData = static_cast<const float *>(bnLayer->_biases->buffer());

        for (size_t i = 0; i < weightsBlob.size(); i++) {
            auto variance = varianceData[i];
            auto mean = meanData[i];
            weightsData[i] = 1.0f / sqrt(variance + bnLayer->epsilon);
            biasesData[i] = (-mean) * weightsData[i];
        }
        weightsPrimID = CreatePrimitiveFromBlob(topology, weightsPrimID,
                                                std::make_shared<InferenceEngine::TBlob<float>>(weightsBlob), blobLayout);
        biasesPrimID = CreatePrimitiveFromBlob(topology, biasesPrimID,
                                               std::make_shared<InferenceEngine::TBlob<float>>(biasesBlob), blobLayout);
    }
        break;
    default:
        THROW_CLDNN_EXCEPTION("Unhandled mean/variance precision in " << bnLayer->name);
        break;
    }
}

void Program::CreateQuantizationPrimitives(cldnn::topology& topology,
                                           const InferenceEngine::CNNLayerPtr& layer,
                                           std::vector<cldnn::primitive_id>& weightsQuantizationPrimID,
                                           bool supportsDequantization,
                                           size_t split) {
    auto wScaleBlob = getBlobOrNull(layer, "w-scale");
    auto oiScaleBlob = getBlobOrNull(layer, "oi-scale");

    auto layerName = layer_type_name_ID(layer);

    if (wScaleBlob != nullptr) {
        auto wScaleDesc = wScaleBlob->getTensorDesc();
        auto wScaleDims = wScaleDesc.getDims();
        if (wScaleDims.size() != 1)
            THROW_CLDNN_EXCEPTION("Incorrect weights scale dimensions number (" << wScaleDims.size() << ") - expected 1");

        auto splitSize = wScaleDims[0] / split;
        auto wScaleTensor = cldnn::tensor(cldnn::batch(splitSize));
        auto wScaleLayout = cldnn::layout(
            DataTypeFromPrecision(wScaleDesc.getPrecision()),
            m_defaultFormat,
            wScaleTensor);
        auto wScalePrimName = layerName + "_cldnn_wScale";

        if (oiScaleBlob != nullptr) {
            std::vector<float> scaleVector;
            float* wScaleData = wScaleBlob->buffer().as<float*>();
            float* oiScaleData = oiScaleBlob->buffer().as<float*>();

            for (size_t si = 0; si < split; ++si) {
                auto splitName = wScalePrimName + std::to_string(si);

                auto mem = cldnn::memory::allocate(*m_engine, wScaleLayout);
                auto ptr = mem.pointer<float>();

                for (size_t c = 0; c < splitSize; ++c) {
                    ptr[c] = wScaleData[si * splitSize + c] / oiScaleData[si * splitSize + c];
                }

                topology.add(cldnn::data(splitName, mem));
                weightsQuantizationPrimID.push_back(splitName);
            }

        } else if (supportsDequantization) {
            auto wScaleBytes = wScaleLayout.bytes_count();

            for (size_t si = 0; si < split; ++si) {
                auto splitName = wScalePrimName + std::to_string(si);
                CreatePrimitiveFromBlob(topology, splitName, wScaleBlob, wScaleLayout, si * wScaleBytes);
                weightsQuantizationPrimID.push_back(splitName);
            }
        }
    }
}


void Program::CreateSingleLayerPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    // Initialize a profiling entry
    InitProfileInfo(layer->name, layer_type_lower(layer));

    // First check for custom layer
    auto customLayer = m_config.customLayers.find(layer->type);
    if (customLayer != m_config.customLayers.end()) {
        CreateCustomLayerPrimitive(topology, layer, customLayer->second);
        return;
    }

    // Otherwise move on to built-in layer types
    switch (LayerTypeFromStr(layer->type)) {
        case Convolution:
            CreateConvolutionPrimitive(topology, layer);
            break;
        case DeformableConvolution:
            CreateDeformableConvolutionPrimitive(topology, layer);
            break;
        case ReLU:
        case ReLU6:
        case Sigmoid:
        case TanH:
        case ELU:
        case Clamp:
        case Activation:
        case Exp:
        case Not:
        case Asin:
        case Atan:
        case Acos:
        case Abs:
        case Asinh:
        case Acosh:
        case Tan:
        case Atanh:
        case Floor:
        case Ceil:
        case Erf:
        case HardSigmoid:
        case Log:
        case Neg:
        case Reciprocal:
        case Selu:
        case Sign:
        case SoftPlus:
        case SoftSign:
            CreateActivationPrimitive(topology, layer, LayerTypeFromStr(layer->type));
            break;
        case LRN: CreateLRNPrimitive(topology, layer);
            break;
        case Pooling: CreatePoolingPrimitive(topology, layer);
            break;
        case Unpooling: CreateMaxUnpoolingPrimitive(topology, layer);
            break;
        case FullyConnected: CreateFullyConnectedPrimitive(topology, layer);
            break;
        case SoftMax: CreateSoftMaxPrimitive(topology, layer);
            break;
        case Power: CreatePowerPrimitive(topology, layer);
            break;
        case Split: CreateSplitPrimitive(topology, layer);
            break;
        case Concatenate: CreateConcatenatePrimitive(topology, layer);
            break;
        case Eltwise: CreateEltwisePrimitive(topology, layer);
            break;
        case SimplerNMS: CreateSimplerNMSPrimitive(topology, layer);
            break;
        case ROIPooling: CreateROIPoolingPrimitive(topology, layer);
            break;
        case Crop: CreateCropPrimitive(topology, layer);
            break;
        case Deconvolution: CreateDeconvolutionPrimitive(topology, layer);
            break;
        case PriorBox: CreatePriorBoxPrimitive(topology, layer);
            break;
        case DetectionOutput: CreateDetectionOutputPrimitive(topology, layer);
            break;
        case Normalize: CreateNormalizePrimitive(topology, layer);
            break;
        case Reshape: CreateReshapePrimitive(topology, layer);
            break;
        case Permute: CreatePermutePrimitive(topology, layer);
            break;
        case Flatten: CreateFlattenPrimitive(topology, layer);
            break;
        case BatchNormalization: CreateBatchNormalizationPrimitive(topology, layer);
            break;
        case PReLU: CreatePReLUPrimitive(topology, layer);
            break;
        case ScaleShift: CreateScaleShiftPrimitive(topology, layer);
            break;
        case Proposal: CreateProposalPrimitive(topology, layer);
            break;
        case PSROIPooling: CreatePSROIPoolingPrimitive(topology, layer);
            break;
        case Copy: CreateCopyPrimitive(topology, layer);
            break;
        case Upsampling: CreateUpsamplingPrimitive(topology, layer);
            break;
        case Resample: CreateResamplePrimitive(topology, layer);
            break;
        case ArgMax:
        case ArgMin:
            CreateArgMaxMinPrimitive(topology, layer, LayerTypeFromStr(layer->type));
            break;
        case MVN: CreateMVNPrimitive(topology, layer);
            break;
        case LSTMCell: CreateLSTMCellPrimitive(topology, layer);
            break;
        case RNN: CreateRNNPrimitive(topology, layer);
            break;
        case RegionYolo: CreateYOLO2RegionPrimitive(topology, layer);
            break;
        case ReorgYolo: CreateYOLO2ReorgPrimitive(topology, layer);
            break;
        case Tile: CreateTilePrimitive(topology, layer);
            break;
        case Pad: CreatePadPrimitive(topology, layer);
            break;
        case Gather: CreateGatherPrimitive(topology, layer);
            break;
        case DepthToSpace: CreateDepthToSpacePrimitive(topology, layer);
            break;
        case ShuffleChannels: CreateShuffleChannelsPrimitive(topology, layer);
            break;
        case StridedSlice: CreateStridedSlicePrimitive(topology, layer);
            break;
        case Broadcast: CreateBroadcastPrimitive(topology, layer);
            break;
        case ReverseSequence: CreateReverseSequencePrimitive(topology, layer);
            break;
        case BinaryConvolution: CreateBinaryConvolutionPrimitive(topology, layer);
            break;
        case Quantize: CreateQuantizePrimitive(topology, layer);
            break;
        case Squeeze: CreateReshapePrimitive(topology, layer);
            break;
        case Unsqueeze: CreateReshapePrimitive(topology, layer);
            break;
        case Reduce: CreateReducePrimitive(topology, layer);
            break;
        case TopK: CreateTopKPrimitive(topology, layer);
            break;
        case Gemm: CreateGemmPrimitive(topology, layer);
            break;
        case OneHot: CreateOneHotPrimitive(topology, layer);
            break;
        default: THROW_CLDNN_EXCEPTION("Unknown Layer Type: " << layer->type);
    }
}

void Program::CreateScaleShiftPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto scaleShiftLayer = as<InferenceEngine::ScaleShiftLayer*> (layer);

    // create scales and biases
    cldnn::primitive_id scalePrimID = scaleShiftLayer->name + m_scalesTag;
    cldnn::primitive_id biasPrimID = scaleShiftLayer->name + m_biasesTag;

    const auto& wDims = scaleShiftLayer->_weights->getTensorDesc().getDims();
    cldnn::tensor weightTensor(1);
    switch (wDims.size()) {
    case 1: weightTensor = (cldnn::tensor) cldnn::feature(TensorValue(wDims[0]));  // value per feature (or 1 global value)
        break;
    case 4: weightTensor = cldnn::tensor(TensorValue(wDims[0]), TensorValue(wDims[1]), TensorValue(wDims[3]), TensorValue(wDims[2]));  // value per pixel
        break;
    case 5: weightTensor = cldnn::tensor(TensorValue(wDims[0]), TensorValue(wDims[1]), TensorValue(wDims[4]), TensorValue(wDims[3]),
            TensorValue(wDims[2]));  // value per pixel
        break;
    default: THROW_CLDNN_EXCEPTION("Invalid weights dimensions in layer " << layer->name);
        break;
    }
    cldnn::layout blobLayout(DataTypeFromPrecision(layer->precision), m_defaultFormat, weightTensor);
    scalePrimID = CreatePrimitiveFromBlob(topology, scalePrimID, scaleShiftLayer->_weights, blobLayout);
    if (scaleShiftLayer->_biases != nullptr) {
        const auto& bDims = scaleShiftLayer->_biases->getTensorDesc().getDims();
        if (bDims != wDims) {
            THROW_CLDNN_EXCEPTION("Invalid bias blob dimensions in layer " << layer->name);
        }
        biasPrimID = CreatePrimitiveFromBlob(topology, biasPrimID, scaleShiftLayer->_biases, blobLayout);
    } else {
        biasPrimID = "";  // 0-bias
    }

    auto inPrecision = layer->insData[0].lock()->getPrecision();
    auto outPrecision = layer->outData[0]->getPrecision();
    auto layerPrecision = layer->precision;
    auto dimensionsNumber = layer->outData[0]->getTensorDesc().getDims().size();

    std::string scaleShiftLayerName = layer_type_name_ID(layer);

    std::string prevLayerName = inputPrimitives[0];

    // Cast input data if it doesn't match operating precision
    if (inPrecision != layerPrecision) {
        std::string inReorderName = scaleShiftLayerName + "_cldnn_in_cast";

        auto inReorderPrim = cldnn::reorder(
            inReorderName,
            prevLayerName,
            defaultFormatForDims(dimensionsNumber),
            DataTypeFromPrecision(layerPrecision));

        topology.add(inReorderPrim);
        profilingIDs.push_back(inReorderName);
        primitivesToIRLayersMap[inReorderName] = { layer->name };
        primitiveIDs[inReorderName] = inReorderName;

        prevLayerName = inReorderName;
    }

    auto scaleShiftPrim = cldnn::scale(
        scaleShiftLayerName,
        prevLayerName,
        scalePrimID,
        biasPrimID);

    prevLayerName = scaleShiftLayerName;

    topology.add(scaleShiftPrim);
    profilingIDs.push_back(scaleShiftLayerName);
    primitivesToIRLayersMap[scaleShiftLayerName] = { layer->name };

    // Cast output data if it doesn't match operating precision
    if (outPrecision != layerPrecision) {
        std::string outReorderName = scaleShiftLayerName + "_cldnn_out_cast";

        auto outReorderPrim = cldnn::reorder(
            outReorderName,
            prevLayerName,
            defaultFormatForDims(dimensionsNumber),
            DataTypeFromPrecision(outPrecision));

        topology.add(outReorderPrim);
        profilingIDs.push_back(outReorderName);
        primitivesToIRLayersMap[outReorderName] = { layer->name };
        primitiveIDs[outReorderName] = outReorderName;

        prevLayerName = outReorderName;
    }

    primitiveIDs[scaleShiftLayerName] = prevLayerName;
}

void Program::CreateProposalPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr & layer) {
    ValidateLayer(layer, 3);
    auto proposalLayer = as<InferenceEngine::GenericLayer*> (layer);

    float nms_thresh = proposalLayer->GetParamAsFloat("nms_thresh", 0.7f);
    int min_size = proposalLayer->GetParamAsInt("min_size", 16);
    int feature_stride = proposalLayer->GetParamAsInt("feat_stride", 16);
    int pre_nms_topn = proposalLayer->GetParamAsInt("pre_nms_topn", 6000);
    int post_nms_topn = proposalLayer->GetParamAsInt("post_nms_topn", 300);
    const std::vector<float> ratio = proposalLayer->GetParamAsFloats("ratio");
    const std::vector<float> scale = proposalLayer->GetParamAsFloats("scale");
    float box_coordinate_scale = proposalLayer->GetParamAsFloat("box_coordinate_scale", 1.0f);
    float box_size_scale = proposalLayer->GetParamAsFloat("box_size_scale", 1.0f);
    int base_size = proposalLayer->GetParamAsInt("base_size", 16);
    std::string framework = proposalLayer->GetParamAsString("framework", "");
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    bool normalize = layer->GetParamAsBool("normalize", false);
    bool clip_before_nms = layer->GetParamAsBool("clip_before_nms", true);
    bool clip_after_nms = layer->GetParamAsBool("clip_after_nms", false);

    float coordinates_offset;
    bool swap_xy;
    bool initial_clip;
    bool round_ratios;
    bool shift_anchors;

    if (framework == "tensorflow") {
        coordinates_offset = 0.0f;
        initial_clip = true;
        shift_anchors = true;
        round_ratios = false;
        swap_xy = true;
    } else {
        coordinates_offset = 1.0f;
        initial_clip = false;
        shift_anchors = false;
        round_ratios = true;
        swap_xy = false;
    }

    const bool for_deformable = layer->GetParamAsBool("for_deformable", 0);

    if (layer->outData.size() == 2) {
        cldnn::layout mutableLayout = cldnn::layout(
                DataTypeFromPrecision(layer->outData[1]->getPrecision()),
                m_defaultFormat,
                CldnnTensorFromIEDims(layer->outData[1]->getDims()));

        auto shared_memory = cldnn::memory::allocate(*m_engine, mutableLayout);

        cldnn::primitive_id proposal_mutable_id_w = layer_type_name_ID(layer) + "_md_write";
        auto argmax_mutable_prim = cldnn::mutable_data(proposal_mutable_id_w, shared_memory);
        primitivesToIRLayersMap[proposal_mutable_id_w] = { layer->name };
        primitiveIDs[proposal_mutable_id_w] = proposal_mutable_id_w;
        topology.add(argmax_mutable_prim);
        inputPrimitives.push_back(proposal_mutable_id_w);

        std::string proposalLayerName = layer_type_lower(layer) + ":" + layer->outData[0]->getName();

        auto proposalPrim = cldnn::proposal(
                proposalLayerName,
                inputPrimitives[0],  // cls_score
                inputPrimitives[1],  // bbox_pred
                inputPrimitives[2],  // im_info
                inputPrimitives[3],  // second_output
                0,                   // max_num_proposals is unused
                nms_thresh,
                base_size,
                min_size,
                feature_stride,
                pre_nms_topn,
                post_nms_topn,
                ratio,
                scale,
                coordinates_offset,
                box_coordinate_scale,
                box_size_scale,
                for_deformable,
                swap_xy,
                initial_clip,
                clip_before_nms,
                clip_after_nms,
                round_ratios,
                shift_anchors,
                normalize);

        primitivesToIRLayersMap[proposalLayerName] = { layer->name };
        primitiveIDs[proposalLayerName] = proposalLayerName;
        topology.add(proposalPrim);

        cldnn::primitive_id proposal_mutable_id_r = layer_type_lower(layer) + ":" + layer->outData[1]->getName();
        auto argmax_mutable_prim_r = cldnn::mutable_data(proposal_mutable_id_r, { proposalLayerName }, shared_memory);
        primitivesToIRLayersMap[proposal_mutable_id_r] = { layer->name };
        primitiveIDs[proposal_mutable_id_r] = proposal_mutable_id_r;
        topology.add(argmax_mutable_prim_r);

        profilingIDs.push_back(proposalLayerName);
        return;
    }

    std::string proposalLayerName = layer_type_name_ID(layer);
    auto proposalPrim = cldnn::proposal(
        proposalLayerName,
        inputPrimitives[0],  // cls_score
        inputPrimitives[1],  // bbox_pred
        inputPrimitives[2],  // im_info
        0,                   // max_num_proposals is unused
        nms_thresh,
        base_size,
        min_size,
        feature_stride,
        pre_nms_topn,
        post_nms_topn,
        ratio,
        scale,
        coordinates_offset,
        box_coordinate_scale,
        box_size_scale,
        for_deformable,
        swap_xy,
        initial_clip,
        clip_before_nms,
        clip_after_nms,
        round_ratios,
        shift_anchors,
        normalize);

    primitivesToIRLayersMap[proposalLayerName] = { layer->name };
    primitiveIDs[proposalLayerName] = proposalLayerName;
    topology.add(proposalPrim);
    profilingIDs.push_back(proposalLayerName);
}

void Program::CreatePReLUPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto preluLayer = as<InferenceEngine::GenericLayer*> (layer);

    std::string preluLayerName = layer_type_name_ID(layer);
    auto inDataPtr = preluLayer->insData[0].lock();
    if (!inDataPtr) {
        THROW_CLDNN_EXCEPTION("Data inserted into PreLu " << preluLayer->name << " is nullptr");
    }

    static const std::string blobName("weights");
    ValidateGenericLayerBlobs(preluLayer, { blobName });

    bool channel_shared = preluLayer->GetParamAsBool("channel_shared", false);

    auto slopeBlob = preluLayer->blobs.at(blobName);
    const auto slopeBlobDesc = slopeBlob->getTensorDesc();
    const auto dim0 = slopeBlobDesc.getDims().back();
    if (channel_shared) {
        if (dim0 != 1) {  // slopeBlob->dims()[0] != 1
            THROW_CLDNN_EXCEPTION("PReLU slope blob with wrong dimensions in " << preluLayer->name);
        }
        float slope(0.0f);
        switch (slopeBlobDesc.getPrecision()) {
        case InferenceEngine::Precision::FP32:
            slope = *static_cast<const float *>(slopeBlob->buffer());
            break;
        case InferenceEngine::Precision::FP16:
        {
            cldnn_status status = CLDNN_SUCCESS;
            slope = cldnn_half_to_float(*static_cast<const uint16_t *>(slopeBlob->buffer()), &status);
            if (status != CLDNN_SUCCESS) {
                THROW_CLDNN_EXCEPTION("Error converting fp16 value in " << preluLayer->name);
            }
        }
            break;
        default: THROW_CLDNN_EXCEPTION("Invalid PReLU slope blob precision in " << preluLayer->name);
        }
        topology.add(cldnn::activation(preluLayerName, inputPrimitives[0], activation_relu_negative_slope, { slope, 0.f }));
    } else {
        cldnn::primitive_id slopePrimID(preluLayerName + "_" + blobName + m_weightsTag);
        auto map = CreateGenericLayerBlobPrimitives(topology, preluLayer);
        topology.add(cldnn::activation(preluLayerName, inputPrimitives[0], map.at(slopePrimID), activation_relu_negative_slope));
    }

    primitivesToIRLayersMap[preluLayerName] = { layer->name };
    primitiveIDs[preluLayerName] = preluLayerName;
    profilingIDs.push_back(preluLayerName);
}

void Program::CreateBatchNormalizationPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr & layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    std::string bnLayerName = layer_type_name_ID(layer);

    auto bnLayer = as<InferenceEngine::BatchNormalizationLayer *> (layer);
    cldnn::primitive_id weightID = bnLayerName + "_" + m_scalesTag;
    cldnn::primitive_id biasID = bnLayerName + "_" + m_biasesTag;

#define _SCALE_BN_OPT
#ifdef _SCALE_BN_OPT
    // Using scale as an optimization (1 mad instead of mad+rsq)
    // create new blobs for scale shift
    CreateScaleWeightsAndBiasesFromBN(topology, bnLayer, weightID, biasID);
    auto scalePrim = cldnn::scale(bnLayerName, inputPrimitives[0], weightID, biasID);

    primitivesToIRLayersMap[bnLayerName] = { layer->name };
    primitiveIDs[bnLayerName] = bnLayerName;
    topology.add(scalePrim);
    profilingIDs.push_back(bnLayerName);
    return;
#else
    cldnn::tensor blobTensor(0);
    const auto bnDims = bnLayer->outData[0]->getTensorDesc().getDims();
    switch (bnDims.size()) {
    case 2:
        blobTensor = cldnn::feature(TensorValue(bnDims[1]));
        break;
    case 4:
        blobTensor = cldnn::feature(TensorValue(bnDims[1]));
        break;
    default:
        THROW_CLDNN_EXCEPTION("Batch normalization input doesn't have 2 or 4 dimensions in " << bnLayer->name);
    }
    cldnn::layout blobLayout(
        DataTypeFromPrecision(layer->precision),
        m_defaultFormat,
        blobTensor);

    // Create variance primitive
    cldnn::primitive_id varianceID = bnLayerName + "_" + m_weightsTag;
    varianceID = CreatePrimitiveFromBlob(topology, varianceID, bnLayer->_weights, blobLayout);

    // Create mean primitive
    cldnn::primitive_id meanID = bnLayerName + "_" + m_biasesTag;
    meanID = CreatePrimitiveFromBlob(topology, meanID, bnLayer->_biases, blobLayout);

    auto bnPrim = cldnn::batch_norm(
        bnLayerName,
        inputPrimitives[0],
        meanID,
        varianceID,
        bnLayer->epsilon);

    primitivesToIRLayersMap[bnLayerName] = { layer->name };
    primitiveIDs[bnLayerName] = bnLayerName;
    topology.add(bnPrim);
    profilingIDs.push_back(bnLayerName);
#endif  // _SCALE_BN_OPT
}

void Program::CreateFlattenPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto flattenLayer = as<InferenceEngine::GenericLayer*> (layer);
    std::string flattenLayerName = layer_type_name_ID(layer);

    auto flattenPrim = cldnn::reshape(
        flattenLayerName,
        inputPrimitives[0],
        CldnnTensorFromIEDims(flattenLayer->outData[0]->getTensorDesc().getDims()));

    primitivesToIRLayersMap[flattenLayerName] = { layer->name };
    primitiveIDs[flattenLayerName] = flattenLayerName;
    topology.add(flattenPrim);
    profilingIDs.push_back(flattenLayerName);
}

void Program::CreatePermutePrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto permuteLayer = as<InferenceEngine::GenericLayer*>(layer);
    std::vector<uint16_t> ie_order;
    for (auto& a : permuteLayer->GetParamAsInts("order"))
        ie_order.push_back(static_cast<uint16_t>(a));

    // if order size is less than 4 - fill the rest with just copy
    for (auto o = ie_order.size(); o < 4; o++)
        ie_order.push_back((uint16_t)o);

    /*
        Because of the cldnn ordering: bfxy, and IE ordering: bfyx
        we need to adjust the permute order.
    */
    std::vector<uint16_t> cldnn_permute_order;
    // 1. Switch permute order values for spatial dims
    for (auto const& o : ie_order) {
        if (o >= 2)
            cldnn_permute_order.push_back(1 + ie_order.size() - o);
        else
            cldnn_permute_order.push_back(o);
    }
    // 2. Swap spatial positions
    for (int i = 0; i < (cldnn_permute_order.size() - 2) / 2; i++) {
        std::swap(cldnn_permute_order[2 + i], cldnn_permute_order[1 + cldnn_permute_order.size() - (2 + i)]);
    }

    std::string permuteLayerName = layer_type_name_ID(layer);

    auto permutePrim = cldnn::permute(
        permuteLayerName,
        inputPrimitives[0],
        cldnn_permute_order);

    primitivesToIRLayersMap[permuteLayerName] = { layer->name };
    primitiveIDs[permuteLayerName] = permuteLayerName;
    topology.add(permutePrim);
    profilingIDs.push_back(permuteLayerName);
}

void Program::CreateReshapePrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    if (layer->insData.size() != 1 && layer->insData.size() != 2)
        THROW_CLDNN_EXCEPTION("Invalid number of inputs for layer: " << layer->name);

    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto reshapeLayer = as<InferenceEngine::GenericLayer*>(layer);
    IE_ASSERT(reshapeLayer->outData.size());
    std::string reshapeLayerName = layer_type_name_ID(layer);

    auto outDesc = reshapeLayer->outData[0]->getTensorDesc();
    auto inDims = reshapeLayer->input()->getTensorDesc().getDims();
    auto outDims = outDesc.getDims();
    auto outTensor = CldnnTensorFromIEDims(outDims);

    // if we convert from or to 5D/6D, additional reorder also required to change format
    cldnn::primitive_id reshapeInputId = inputPrimitives[0];
    if (inDims.size() != outDims.size()) {
        cldnn::primitive_id reorderId = reshapeLayerName + "_reorder";
        cldnn::format outputFormat = cldnn::format::bfyx;

        switch (outDims.size()) {
        case 5: outputFormat = cldnn::format::bfzyx; break;
        case 6: outputFormat = cldnn::format::bfwzyx; break;
        default: break;
        }

        cldnn::layout outputLayout(DataTypeFromPrecision(outDesc.getPrecision()), outputFormat, outTensor);
        topology.add(cldnn::reorder(reorderId, reshapeInputId, outputLayout));
        reshapeInputId = reorderId;
        primitivesToIRLayersMap[reorderId] = { layer->name };
    }

    auto reshapePrim = cldnn::reshape(
        reshapeLayerName,
        reshapeInputId,
        outTensor);

    primitivesToIRLayersMap[reshapeLayerName] = { layer->name };
    primitiveIDs[reshapeLayerName] = reshapeLayerName;
    topology.add(reshapePrim);
    profilingIDs.push_back(reshapeLayerName);
}

void Program::CreateNormalizePrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto normLayer = as<InferenceEngine::GenericLayer*> (layer);
    ValidateGenericLayerBlobs(normLayer, { "weights" });
    auto map = CreateGenericLayerBlobPrimitives(topology, normLayer);

    // params
    bool across_spatial = normLayer->GetParamAsBool("across_spatial", true);
    float eps = normLayer->GetParamAsFloat("eps", 0.0f);

    // WA for MO outputting %.6f
    if (eps == 0.0f) {
        eps = 1e-10f;
    }

    std::string normLayerName = layer_type_name_ID(layer);
    auto normPrim = cldnn::normalize(
        normLayerName,
        inputPrimitives[0],
        map.at(normLayerName + "_weights" + m_weightsTag),
        across_spatial,
        eps);

    primitivesToIRLayersMap[normLayerName] = { layer->name };
    primitiveIDs[normLayerName] = normLayerName;
    topology.add(normPrim);
    profilingIDs.push_back(normLayerName);
}

void Program::CreateDetectionOutputPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 3);
    auto detectionLayer = as<InferenceEngine::GenericLayer*> (layer);

    uint32_t num_classes            = detectionLayer->GetParamAsUInt("num_classes", 1);
    bool share_location             = detectionLayer->GetParamAsBool("share_location", true);
    int background_label_id         = detectionLayer->GetParamAsInt("background_label_id", 0);
    float nms_threshold             = detectionLayer->GetParamAsFloat("nms_threshold", 0.3f);
    int top_k                       = detectionLayer->GetParamAsInt("top_k", -1);
    float confidence_threshold      = detectionLayer->GetParamAsFloat("confidence_threshold", -FLT_MAX);
    float eta                       = detectionLayer->GetParamAsFloat("eta", 1.0f);
    int keep_top_k                  = detectionLayer->GetParamAsInt("keep_top_k", -1);
    bool variance_encoded_in_target = detectionLayer->GetParamAsBool("variance_encoded_in_target", false);
    int input_width                 = detectionLayer->GetParamAsInt("input_width", -1);
    int input_height                = detectionLayer->GetParamAsInt("input_height", -1);
    bool normalized                 = detectionLayer->GetParamAsBool("normalized", true);
    std::string code_type           = detectionLayer->GetParamAsString("code_type", "caffe.PriorBoxParameter.CORNER");
    bool clip_before_nms            = detectionLayer->GetParamAsBool("clip_before_nms", false) ||
                                      detectionLayer->GetParamAsBool("clip", false);  // For backward compatibility
    bool clip_after_nms             = detectionLayer->GetParamAsBool("clip_after_nms", false);
    bool decrease_label_id          = detectionLayer->GetParamAsBool("decrease_label_id", false);

    cldnn::prior_box_code_type cldnnCodeType = PriorBoxCodeFromString(code_type);
    int32_t prior_info_size = normalized != 0 ? 4 : 5;
    int32_t prior_coordinates_offset = normalized != 0 ? 0 : 1;

    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    std::string detectionLayerName = layer_type_name_ID(layer);
    auto detectionPrim = cldnn::detection_output(detectionLayerName,
                                                 inputPrimitives[0],
                                                 inputPrimitives[1],
                                                 inputPrimitives[2],
                                                 num_classes,
                                                 keep_top_k,
                                                 share_location,
                                                 background_label_id,
                                                 nms_threshold,
                                                 top_k,
                                                 eta,
                                                 cldnnCodeType,
                                                 variance_encoded_in_target,
                                                 confidence_threshold,
                                                 prior_info_size,
                                                 prior_coordinates_offset,
                                                 normalized,
                                                 input_width,
                                                 input_height,
                                                 decrease_label_id,
                                                 clip_before_nms,
                                                 clip_after_nms);

    primitivesToIRLayersMap[detectionLayerName] = { layer->name };
    primitiveIDs[detectionLayerName] = detectionLayerName;
    topology.add(detectionPrim);
    profilingIDs.push_back(detectionLayerName);
}

void Program::CreatePriorBoxPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 2);
    auto priorBoxLayer = as<InferenceEngine::GenericLayer*> (layer);

    // params
    std::vector<float> min_size = priorBoxLayer->GetParamAsFloats("min_size");
    std::vector<float> max_size = priorBoxLayer->GetParamAsFloats("max_size", {});
    std::vector<float> aspect_ratio = priorBoxLayer->GetParamAsFloats("aspect_ratio", {});
    std::vector<float> variance = priorBoxLayer->GetParamAsFloats("variance");
    bool flip = priorBoxLayer->GetParamAsBool("flip", true);
    bool clip = priorBoxLayer->GetParamAsBool("clip", false);
    bool scale_all_sizes = priorBoxLayer->GetParamAsBool("scale_all_sizes", true);
    float offset = priorBoxLayer->GetParamAsFloat("offset", 0.5f);

    auto step_w = priorBoxLayer->GetParamAsFloat("step_w", 0.0f);
    auto step_h = priorBoxLayer->GetParamAsFloat("step_h", 0.0f);
    auto step   = priorBoxLayer->GetParamAsFloat("step", 0.0f);

    float _step_w = 0.0f;
    float _step_h = 0.0f;
    if (HasParam(priorBoxLayer->params, "step_w") && step_w != 0.0f &&
        HasParam(priorBoxLayer->params, "step_h") && step_h != 0.0f) {
        _step_w = step_w;
        _step_h = step_h;
    } else if (HasParam(priorBoxLayer->params, "step") && step != 0.0f) {
        _step_w = step;
        _step_h = step;
    }

    int img = priorBoxLayer->GetParamAsInt("img_size", 0);
    int img_w = priorBoxLayer->GetParamAsInt("img_w", 0);
    int img_h = priorBoxLayer->GetParamAsInt("img_h", 0);
    if ((img != 0) || (img_w != 0) || (img_h != 0)) {
        // unsupported mode
        THROW_CLDNN_EXCEPTION("Unsupported image sizes in prior box " + layer->name + " (use an image blob instead of dimensions)");
    }

    IE_ASSERT(layer->insData[1].lock());
    auto img_dims = layer->insData[1].lock()->getTensorDesc().getDims();

    auto wdim = img_dims.back();
    auto hdim = img_dims.at(img_dims.size()-2);

    cldnn::tensor img_size = (cldnn::tensor) cldnn::spatial(TensorValue(wdim), TensorValue(hdim));
    std::vector<cldnn::primitive_id> inputPrimitives = GetPrevLayersPrimitives(layer);
    // second input isn't used by value - only dimensions taken from the layer input

    if (_step_w == 0.0f || _step_h == 0.0f) {
        _step_w = static_cast<float>(img_w) / static_cast<float>(wdim);
        _step_h = static_cast<float>(img_h) / static_cast<float>(hdim);
    }

    std::string priorBoxLayerName = layer_type_name_ID(layer);
    auto priorBoxPrim = cldnn::prior_box(
        priorBoxLayerName,
        inputPrimitives[0],
        img_size,
        min_size,
        max_size,
        aspect_ratio,
        flip,
        clip,
        variance,
        _step_w,
        _step_h,
        offset,
        scale_all_sizes);

    primitivesToIRLayersMap[priorBoxLayerName] = { layer->name };
    primitiveIDs[priorBoxLayerName] = priorBoxLayerName;
    topology.add(priorBoxPrim);
    profilingIDs.push_back(priorBoxLayerName);
}

void Program::CreateDeconvolutionPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto deconvLayer = as<InferenceEngine::DeconvolutionLayer *> (layer);

    if (deconvLayer->_dilation[X_AXIS] != 1 || deconvLayer->_dilation[Y_AXIS] != 1) {
        THROW_CLDNN_EXCEPTION("Unsupported dilation in deconvolution " << layer->name);
    }

    std::vector<cldnn::primitive_id> weightPrimID;
    std::vector<cldnn::primitive_id> biasPrimID;
    CreateWeightAndBiasPrimitives(topology, layer, weightPrimID, biasPrimID);

    auto allPad = getPaddings(*deconvLayer);
    int x_pad = allPad.begin[X_AXIS], y_pad = allPad.begin[Y_AXIS];
    cldnn::tensor stride, padding, dilation;
    if (deconvLayer->input()->getTensorDesc().getDims().size() > 4) {
        stride = cldnn::tensor(cldnn::batch(1), cldnn::feature(1),
            cldnn::spatial(deconvLayer->_stride[X_AXIS],
                deconvLayer->_stride[Y_AXIS],
                deconvLayer->_stride[Z_AXIS]));
        int z_pad = allPad.begin[Z_AXIS];
        padding = cldnn::tensor(cldnn::batch(0), cldnn::feature(0),
            cldnn::spatial(-x_pad, -y_pad, -z_pad));
        dilation = cldnn::tensor(cldnn::batch(1), cldnn::feature(1),
            cldnn::spatial(deconvLayer->_dilation[X_AXIS],
                deconvLayer->_dilation[Y_AXIS],
                deconvLayer->_dilation[Z_AXIS]));
    } else {
        stride = cldnn::tensor(cldnn::batch(1), cldnn::feature(1),
            cldnn::spatial(deconvLayer->_stride[X_AXIS], deconvLayer->_stride[Y_AXIS]));
        padding = cldnn::tensor(cldnn::batch(0), cldnn::feature(0),
            cldnn::spatial(-x_pad, -y_pad, 0));
        dilation = cldnn::tensor(cldnn::batch(1), cldnn::feature(1),
            cldnn::spatial(deconvLayer->_dilation[X_AXIS], deconvLayer->_dilation[Y_AXIS]));
    }

    std::string deconvLayerName = layer_type_name_ID(layer);

    if (deconvLayer->_group >= 16) {
        auto deconvPrim = cldnn::deconvolution(deconvLayerName,
            inputPrimitives[0],
            weightPrimID,
            biasPrimID,
            deconvLayer->_group,
            stride,
            padding,
            false,
            0.0f,
            CldnnTensorFromIEDims(deconvLayer->outData[0]->getTensorDesc().getDims()));
        topology.add(deconvPrim);
    } else {
        auto deconvPrim = cldnn::deconvolution(deconvLayerName,
            inputPrimitives[0],
            weightPrimID,
            biasPrimID,
            stride,
            padding,
            false,
            0.0f,
            CldnnTensorFromIEDims(deconvLayer->outData[0]->getTensorDesc().getDims()));
        topology.add(deconvPrim);
    }
    primitivesToIRLayersMap[deconvLayerName] = { layer->name };
    primitiveIDs[deconvLayerName] = deconvLayerName;
    profilingIDs.push_back(deconvLayerName);
}

void Program::CreateCropPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    if (layer->insData.size() != 1 && layer->insData.size() != 2) {
        THROW_CLDNN_EXCEPTION("Invalid number of inputs for layer: " << layer->name);
    }
    if (layer->_fusedWith) {
        THROW_CLDNN_EXCEPTION("Unsupported fuse in layer: " << layer->name << " with: " << layer->_fusedWith->name);
    }
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto cropLayer = as<InferenceEngine::CropLayer*> (layer);
    IE_ASSERT(cropLayer->axis.size() == cropLayer->offset.size());
    // IE_ASSERT(cropLayer->outData[0] && cropLayer->outData[0]->dims.size() == 4);

    std::vector<cldnn::tensor::value_type> offset{ 0, 0, 0, 0 };
    for (size_t i = 0; i < cropLayer->axis.size(); i++) {
        if (cropLayer->axis[i] < 0 || cropLayer->axis[i] > 3) {
            THROW_CLDNN_EXCEPTION("Invalid crop axis: " + std::to_string(cropLayer->axis[i]) + " in layer " + cropLayer->name);
        }
        offset[cropLayer->axis[i]] = cropLayer->offset[i];
    }
    auto outputDims = cropLayer->outData[0]->getTensorDesc().getDims();
    const size_t ods = outputDims.size();
    cldnn::tensor refSize(
        TensorValue(ods > 0 ? outputDims[0] : 1),
        TensorValue(ods > 1 ? outputDims[1] : 1),
        TensorValue(ods > 3 ? outputDims[3] : 1),
        TensorValue(ods > 2 ? outputDims[2] : 1));

    cldnn::tensor offSize(
        TensorValue(offset[0]),
        TensorValue(offset[1]),
        TensorValue(offset[3]),
        TensorValue(offset[2]));

    std::string cropLayerName = layer_type_name_ID(layer);
    auto cropPrim = cldnn::crop(
        cropLayerName,
        inputPrimitives[0],
        refSize,
        offSize);

    primitivesToIRLayersMap[cropLayerName] = { layer->name };
    primitiveIDs[cropLayerName] = cropLayerName;
    topology.add(cropPrim);
    profilingIDs.push_back(cropLayerName);
}

void Program::CreateROIPoolingPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 2);
    auto roiPoolingLayer = as<InferenceEngine::GenericLayer*> (layer);

    // params
    int pooled_width = roiPoolingLayer->GetParamAsInt("pooled_w", 0);
    int pooled_height = roiPoolingLayer->GetParamAsInt("pooled_h", 0);
    float spatial_scale = roiPoolingLayer->GetParamAsFloat("spatial_scale", 1.0f);
    std::string method = roiPoolingLayer->GetParamAsString("method", "max");
    bool position_sensitive = false;

    cldnn::pooling_mode mode = cldnn::pooling_mode::max;
    if (method == "bilinear") {
        mode = cldnn::pooling_mode::bilinear;
    }
    auto inputPrimitives = GetPrevLayersPrimitives(layer);

    std::string roiPoolingLayerName = layer_type_name_ID(layer);
    auto roiPoolingPrim = cldnn::roi_pooling(roiPoolingLayerName,
                                             inputPrimitives[0],  // input data
                                             inputPrimitives[1],  // input rois
                                             mode,
                                             position_sensitive,
                                             pooled_width,
                                             pooled_height,
                                             spatial_scale);

    primitivesToIRLayersMap[roiPoolingLayerName] = { layer->name };
    primitiveIDs[roiPoolingLayerName] = roiPoolingLayerName;
    topology.add(roiPoolingPrim);
    profilingIDs.push_back(roiPoolingLayerName);
}

void Program::CreatePSROIPoolingPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    auto psROIPoolingLayer = as<InferenceEngine::GenericLayer*> (layer);

    // params
    std::string mode_str = psROIPoolingLayer->GetParamAsString("mode", "average");
    cldnn::pooling_mode mode = mode_str == "average" ? cldnn::pooling_mode::average :
                               mode_str == "bilinear" ? cldnn::pooling_mode::bilinear : cldnn::pooling_mode::deformable_bilinear;
    bool no_trans = psROIPoolingLayer->GetParamAsBool("no_trans", true);
    if (mode != cldnn::pooling_mode::deformable_bilinear || no_trans)
        ValidateLayer(layer, 2);
    else
        ValidateLayer(layer, 3);
    int group_size = psROIPoolingLayer->GetParamAsInt("group_size");
    int output_dim = psROIPoolingLayer->GetParamAsInt("output_dim");
    float spatial_scale = psROIPoolingLayer->GetParamAsFloat("spatial_scale");
    int spatial_bins_x = psROIPoolingLayer->GetParamAsInt("spatial_bins_x", 1);
    int spatial_bins_y = psROIPoolingLayer->GetParamAsInt("spatial_bins_y", 1);
    bool position_sensitive = true;

    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    std::string psROIPoolingLayerName = layer_type_name_ID(layer);

    if (mode != cldnn::pooling_mode::deformable_bilinear) {
        auto psROIPoolingPrim = cldnn::roi_pooling(psROIPoolingLayerName,
                                                   inputPrimitives[0],  // input data
                                                   inputPrimitives[1],  // input rois
                                                   mode,
                                                   position_sensitive,
                                                   group_size,
                                                   group_size,
                                                   spatial_scale,
                                                   output_dim,
                                                   spatial_bins_x,
                                                   spatial_bins_y);
        topology.add(psROIPoolingPrim);
    } else {
        float trans_std = psROIPoolingLayer->GetParamAsFloat("trans_std", 1);
        int part_size = psROIPoolingLayer->GetParamAsInt("part_size", 1);
        int pooled_width = psROIPoolingLayer->GetParamAsInt("pooled_width", 1);
        int pooled_height = psROIPoolingLayer->GetParamAsInt("pooled_height", 1);

        auto psROIPoolingPrim = cldnn::roi_pooling(psROIPoolingLayerName,
                                                   inputPrimitives,
                                                   mode,
                                                   position_sensitive,
                                                   pooled_width,
                                                   pooled_height,
                                                   spatial_scale,
                                                   trans_std,
                                                   no_trans,
                                                   part_size,
                                                   group_size,
                                                   output_dim,
                                                   spatial_bins_x,
                                                   spatial_bins_y);
        topology.add(psROIPoolingPrim);
    }
    primitivesToIRLayersMap[psROIPoolingLayerName] = {layer->name};
    primitiveIDs[psROIPoolingLayerName] = psROIPoolingLayerName;
    profilingIDs.push_back(psROIPoolingLayerName);
}

void Program::CreateCustomLayerPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr & layer, CLDNNCustomLayerPtr customLayer) {
    ValidateLayer(layer, 0);
    // todo: handling fusing
    auto genericLayer = as<InferenceEngine::GenericLayer*> (layer);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);

    // Handle defines
    std::string layerDefines;
    for (const auto& def : customLayer->Defines()) {
        std::string singleDefine("#define " + def.name + " " + def.prefix);
        if (genericLayer->params.find(def.param) != genericLayer->params.end()) {
            singleDefine += genericLayer->params.at(def.param);
        } else {
            singleDefine += def.default_value;
        }
        singleDefine += def.postfix + "\n";
        layerDefines.append(singleDefine);
    }

    // reserve
    std::vector<cldnn::primitive_id> reorderedInputs;
    reorderedInputs.resize(inputPrimitives.size());

    // Handle Blobs
    std::map<std::string, size_t> blobIndex;
    for (auto& blob : genericLayer->blobs) {
        const auto blobDims = blob.second->getTensorDesc().getDims();
        // create primitive from blob (always 1d)
        cldnn::primitive_id blobId = genericLayer->name + "_" + blob.first;
        if (blobDims.size() != 1) {
            THROW_CLDNN_EXCEPTION("Invalid dimensions for blob " << blob.first << " in layer " << genericLayer->name);
        }
        cldnn::layout genericBlobLayout(DataTypeFromPrecision(blob.second->getTensorDesc().getPrecision()),
                                        m_defaultFormat,
                                        cldnn::tensor(1, 1, TensorValue(blobDims.back()), 1));
        blobId = CreatePrimitiveFromBlob(topology, blobId, blob.second, genericBlobLayout);
        // save index in blobIndex
        blobIndex[blob.first] = reorderedInputs.size();
        // add to reorderedInputs
        reorderedInputs.push_back(blobId);
    }

    // Handle kernel parameters
    std::vector<cldnn_arg> kernelParameters;
    cldnn::format outputFormat(cldnn::format::any);
    for (const auto& param : customLayer->KernelParams()) {
        switch (param.type) {
        case CLDNNCustomLayer::ParamType::Input: {
            kernelParameters.resize(kernelParameters.size() > size_t(param.paramIndex + 1) ? kernelParameters.size() : size_t(param.paramIndex + 1));
            kernelParameters[param.paramIndex].arg_type = cldnn_arg_type::arg_input;
            kernelParameters[param.paramIndex].index = static_cast<cldnn_arg_index>((param.portIndex >= inputPrimitives.size()) ? -1 : param.portIndex);

            // Handle input reorder
            if (param.portIndex < inputPrimitives.size() && reorderedInputs[param.portIndex].empty()) {
                // todo: add support for multiple reorders of the same input? (read as bfyx for one arg and yxfb for another)
                if (param.format != cldnn::format::any) {
                    auto reorderPrimName = inputPrimitives[param.portIndex] + "_" + layer->name + m_preCustomLayerTag;
                    auto preprocessPrim = cldnn::reorder(
                        reorderPrimName,
                        inputPrimitives[param.portIndex],
                        param.format,
                        DataTypeFromPrecision(layer->precision));

                    primitivesToIRLayersMap[reorderPrimName] = { layer->name };
                    topology.add(preprocessPrim);
                    profilingIDs.push_back(reorderPrimName);
                    InitProfileInfo(reorderPrimName, "Reorder");
                    reorderedInputs[param.portIndex] = (reorderPrimName);
                } else {
                    reorderedInputs[param.portIndex] = inputPrimitives[param.portIndex];
                }
            }
        }
            break;
        case CLDNNCustomLayer::ParamType::Output: {
            kernelParameters.resize(kernelParameters.size() > size_t(param.paramIndex + 1) ? kernelParameters.size() : size_t(param.paramIndex + 1));
            kernelParameters[param.paramIndex].arg_type = cldnn_arg_type::arg_output;
            kernelParameters[param.paramIndex].index =
                static_cast<cldnn_arg_index>((param.portIndex >= inputPrimitives.size()) ? -1 : param.portIndex);
            outputFormat = param.format;
        }
            break;
        case CLDNNCustomLayer::ParamType::Data: {
            kernelParameters.resize(kernelParameters.size() > size_t(param.paramIndex + 1) ? kernelParameters.size() : size_t(param.paramIndex + 1));
            kernelParameters[param.paramIndex].arg_type = cldnn_arg_type::arg_input;
            kernelParameters[param.paramIndex].index =
                static_cast<cldnn_arg_index>((blobIndex.find(param.blobName) == blobIndex.end()) ? -1 : blobIndex.at(param.blobName));
        }
            break;
        default:
            THROW_CLDNN_EXCEPTION("Invalid custom layer param type: " << param.type << " in layer: " << genericLayer->name);
        }
    }
    const std::string layerTitle("\n// Layer " + layer->name + " using Custom Layer " + customLayer->Name() + "\n");
    const std::string defineTitle("// Custom Layer User Defines\n");

    auto dims = genericLayer->outData[0]->getTensorDesc().getDims();
    size_t N = (dims.size() > 0) ? dims[0] : 1;
    size_t C = (dims.size() > 1) ? dims[1] : 1;
    size_t H = (dims.size() > 2) ? dims[2] : 1;
    size_t W = (dims.size() > 3) ? dims[3] : 1;
    cldnn::tensor outputTensor = cldnn::tensor(cldnn::batch(N), cldnn::feature(C), cldnn::spatial(W, H));

    cldnn::layout outputLayout = cldnn::layout(DataTypeFromPrecision(genericLayer->precision), outputFormat, outputTensor);

    // evaluate work sizes rules
    std::vector<size_t> gws, lws;

    // assume output tensor is dimension source by default
    int batchDim = outputTensor.batch[0];
    int featureDim = outputTensor.feature[0];
    int yDim = outputTensor.spatial[1];
    int xDim = outputTensor.spatial[0];
    int iidx = customLayer->InputDimSourceIndex();

    std::string genericLayerName = layer_type_name_ID(layer);
    // if input index is greater than -1, take dimension from input
    if (iidx >= 0) {
        if (iidx >= genericLayer->insData.size())
            THROW_CLDNN_EXCEPTION("Invalid input tensor for index: " << iidx);
        // get dimensions from one of the input tensors
        auto inDataPtr = genericLayer->insData[iidx].lock();
        if (!inDataPtr) {
            THROW_CLDNN_EXCEPTION("Data inserted into generic layer " << genericLayer->name << " is nullptr");
        }
        SizeVector inputDims = inDataPtr->getTensorDesc().getDims();

        xDim = inputDims[inputDims.size() - 1];
        yDim = dims.size() > 1 ? inputDims[inputDims.size() - 2] : 0;
        featureDim = dims.size() > 2 ? inputDims[inputDims.size() - 3] : 0;
        batchDim = dims.size() > 3 ? inputDims[inputDims.size() - 4]: 0;
    }
    const std::map<char, int> vars = {
        { 'b', batchDim }  , { 'B', batchDim },
        { 'f', featureDim }, { 'F', featureDim },
        { 'y', yDim },       { 'Y', yDim },
        { 'x', xDim },       { 'X', xDim },
    };
    for (auto rule : customLayer->GlobalSizeRules()) {
        SimpleMathExpression expr;
        expr.SetVariables(vars);
        expr.SetExpression(rule);
        gws.push_back(expr.Evaluate());
    }
    for (auto rule : customLayer->LocalSizeRules()) {
        SimpleMathExpression expr;
        expr.SetVariables(vars);
        expr.SetExpression(rule);
        lws.push_back(expr.Evaluate());
    }

    auto customPrim = cldnn::custom_gpu_primitive(
        genericLayerName,
        reorderedInputs,
        { layerTitle, defineTitle, layerDefines, customLayer->KernelSource() },
        customLayer->KernelEntry(),
        kernelParameters,
        customLayer->CompilerOptions(),
        outputLayout,
        gws,
        lws);

    if (outputLayout.format != cldnn::format::any &&
        p_currentOutputs.find(genericLayerName) == p_currentOutputs.end()) {
        // Handle output reorder
        auto reorderPrimName = genericLayerName + m_postCustomLayerTag;
        topology.add(
            cldnn::reorder(
                reorderPrimName,
                genericLayerName,
                m_defaultFormat,
                customPrim.output_layout.data_type));
        primitivesToIRLayersMap[reorderPrimName] = { layer->name };
        primitiveIDs[genericLayerName] = reorderPrimName;
        primitiveIDs[reorderPrimName] = reorderPrimName;
        profilingIDs.push_back(reorderPrimName);
        InitProfileInfo(reorderPrimName, "Reorder");
    } else {
        primitiveIDs[genericLayerName] = genericLayerName;
    }
    primitivesToIRLayersMap[genericLayerName] = { layer->name };
    topology.add(customPrim);
    profilingIDs.push_back(genericLayerName);
}

void Program::CreateSimplerNMSPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 3);
    IE_ASSERT(layer->insData[0].lock()->getTensorDesc().getDims().front() == 1);  // only handling input batch size 1
    IE_ASSERT(layer->insData[1].lock()->getTensorDesc().getDims().front() == 1);  // only handling input batch size 1
    auto simpleNMSLayer = as<InferenceEngine::GenericLayer*> (layer);

    int max_num_proposals = simpleNMSLayer->GetParamAsInt("max_num_proposals");
    float iou_threshold = simpleNMSLayer->GetParamAsFloat("iou_threshold", 0.7f);
    int min_bbox_size = simpleNMSLayer->GetParamAsInt("min_bbox_size", 16);
    int feature_stride = simpleNMSLayer->GetParamAsInt("feat_stride", 16);
    int pre_nms_topn = simpleNMSLayer->GetParamAsInt("pre_nms_topn");
    int post_nms_topn = simpleNMSLayer->GetParamAsInt("post_nms_topn");
    std::vector<float> scale = simpleNMSLayer->GetParamAsFloats("scale");
    auto inputPrimitives = GetPrevLayersPrimitives(layer);

    std::string simpleNMSLayerName = layer_type_name_ID(layer);
    auto simpleNMSPrim = cldnn::proposal(
        simpleNMSLayerName,
        inputPrimitives[0],  // cls_score
        inputPrimitives[1],  // bbox_pred
        inputPrimitives[2],  // im_info
        max_num_proposals,
        iou_threshold,
        min_bbox_size,
        feature_stride,
        pre_nms_topn,
        post_nms_topn,
        { 0.5f, 1.0f, 2.0f },  // ratios for the SimplerNMS variant
        scale);

    primitivesToIRLayersMap[simpleNMSLayerName] = { layer->name };
    primitiveIDs[simpleNMSLayerName] = simpleNMSLayerName;
    topology.add(simpleNMSPrim);
    profilingIDs.push_back(simpleNMSLayerName);
}

void Program::CreateEltwisePrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateEltwiseLayer(layer);

    auto eltwiseLayer = as<InferenceEngine::EltwiseLayer *> (layer);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);

    std::vector<float> coefficients = eltwiseLayer->coeff;
    if (eltwiseLayer->_operation != InferenceEngine::EltwiseLayer::Sum && !coefficients.empty()) {
        THROW_IE_EXCEPTION << "Only sum operation supports operands coefficients";
    }

    if (!coefficients.empty() && coefficients.size() != inputPrimitives.size()) {
        THROW_IE_EXCEPTION << "Number of provided coefficients is not equal to number of operands";
    }

    std::string eltwiseLayerName = layer_type_name_ID(layer);
    auto eltwisePrim = cldnn::eltwise(
        eltwiseLayerName,
        inputPrimitives,
        EltwiseModeFromIEEltwise(eltwiseLayer->_operation),
        coefficients);

    auto sumScaleBlob = getBlobOrNull(layer, "eltwise-sum-scale");

    if (sumScaleBlob != nullptr) {
        // Quantization for eltwise is currently supported only for two inputs, where one of them is convolution
        if (layer->insData.size() != 2) {
            THROW_CLDNN_EXCEPTION("Too many inputs (" << layer->insData.size() << ") for quantized Eltwise layer " << layer->name);
        }

        auto input0Type = LayerTypeFromStr(layer->insData[0].lock()->getCreatorLayer().lock()->type);
        auto input1Type = LayerTypeFromStr(layer->insData[1].lock()->getCreatorLayer().lock()->type);

        size_t otherInputIdx;
        if (input0Type == LayerType::Convolution) {
            otherInputIdx = 1;
        } else if (input1Type == LayerType::Convolution) {
            otherInputIdx = 0;
        } else {
            THROW_CLDNN_EXCEPTION("Could not find Convolution to fuse " << layer->name << " with - required for quantized Eltwise");
        }

        eltwisePrim.input_quantization_factors.resize(2, 1.f);
        // For now only per tensor quantization is supported
        eltwisePrim.input_quantization_factors[otherInputIdx] = sumScaleBlob->buffer().as<float*>()[0];
    }

    topology.add(eltwisePrim);
    primitivesToIRLayersMap[eltwiseLayerName] = { layer->name };
    profilingIDs.push_back(eltwiseLayerName);

    // Cast output data type if it differs from operation precision
    auto operationPrecision = layer->precision;
    auto outputPrecision = layer->outData[0]->getPrecision();

    auto lastLayerName = eltwiseLayerName;

    if (operationPrecision != outputPrecision) {
        auto reorderLayerName = eltwiseLayerName + "_cldnn_out_cast";
        auto reorderPrim = cldnn::reorder(
            reorderLayerName,
            eltwiseLayerName,
            defaultFormatForDims(layer->outData[0]->getTensorDesc().getDims().size()),
            DataTypeFromPrecision(outputPrecision));

        topology.add(reorderPrim);
        primitivesToIRLayersMap[reorderLayerName] = { layer->name };
        profilingIDs.push_back(reorderLayerName);
        primitiveIDs[reorderLayerName] = reorderLayerName;

        lastLayerName = reorderLayerName;
    }

    primitiveIDs[eltwiseLayerName] = lastLayerName;
}

inline cldnn::concatenation::concatenation_axis ConcatAxisFromIEAxis(unsigned axis, unsigned sz) {
    if (axis >= sz)
        THROW_CLDNN_EXCEPTION("Concatenation axis exceeds number of dimensions");

    // Difference in dimension ordering between IE and clDNN,
    // reverse spatial dimensions after batch and feature.
    unsigned cldnn_axis = axis;
    if (axis >= 2) {
        auto spatial_axis = axis - 2;
        // Default and minimum number of dimensions is 4
        auto spatial_size = std::max(sz, 4u) - 2;
        cldnn_axis = spatial_size - spatial_axis - 1 + 2;
    }

    switch (cldnn_axis) {
        case 0:
            return cldnn::concatenation::concatenation_axis::along_b;
        case 1:
            return cldnn::concatenation::concatenation_axis::along_f;
        case 2:
            return cldnn::concatenation::concatenation_axis::along_x;
        case 3:
            return cldnn::concatenation::concatenation_axis::along_y;
        case 4:
            return cldnn::concatenation::concatenation_axis::along_z;
        case 5:
            return cldnn::concatenation::concatenation_axis::along_w;
        default: THROW_CLDNN_EXCEPTION("Unsupported concatenation axis: " << axis);
            break;
    }

    return cldnn::concatenation::concatenation_axis::along_f;  // shouldn't get here
}

void Program::CreateConcatenatePrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 0);
    auto concatLayer = as<InferenceEngine::ConcatLayer *> (layer);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    std::string concatLayerName = layer_type_name_ID(layer);
    auto concatPrim = cldnn::concatenation(
        concatLayerName,
        inputPrimitives,
        ConcatAxisFromIEAxis(concatLayer->_axis,
                             concatLayer->input().get()->getTensorDesc().getDims().size()));

    primitivesToIRLayersMap[concatLayerName] = { layer->name };
    primitiveIDs[concatLayerName] = concatLayerName;
    topology.add(concatPrim);
    profilingIDs.push_back(concatLayerName);
}

void Program::CreateSplitPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto splitLayer = as<InferenceEngine::SplitLayer *> (layer);
    if (IsValidSplitConvMerge(splitLayer)) {
        // AlextNet style split->conv*2->merge
        CreateFusedSplitConvMergePrimitive(topology, layer);
    } else {
#ifdef _USE_SPLIT_PRIMITIVE
        auto inputPrimitives = GetPrevLayersPrimitives(layer);
        auto inputDims = splitLayer->insData[0].lock()->getTensorDesc().getDims();
        InferenceEngine::SizeVector startOffset(inputDims.size());
        std::vector<std::pair<cldnn::primitive_id, cldnn::tensor>> outputOffsets;

        std::string splitLayerName = layer_type_name_ID(layer);
        for (auto& outLayer : splitLayer->outData) {
            if (outLayer->dims.size() != startOffset.size()) {
                THROW_CLDNN_EXCEPTION("Invalid dimesions in split layer: " << splitLayer->name << " output: " << outLayer->name);
            }
            for (size_t i = 0; i < inputDims.size(); i++) {
                if ((outLayer->dims[i] + startOffset[i]) > inputDims[i]) {
                    THROW_CLDNN_EXCEPTION("Invalid dimesions in split layer: " << splitLayer->name << " output: " << outLayer->name);
                }
            }
            auto outTensor = CldnnTensorFromIEDims(outLayer->getTensorDesc().getDims());
            std::string outLayerName = splitLayer->type + ":" + outLayer->name;

            auto cropPrim = cldnn::crop(outLayerName, inputPrimitives[0], outTensor, CldnnTensorFromIEDims(startOffset));
            topology.add(cropPrim);

            primitivesToIRLayersMap[outLayerName] = { layer->name };
            primitiveIDs[outLayerName] = outLayerName;
            profilingIDs.push_back(outLayerName);
            outputOffsets.emplace_back(outLayerName, CldnnTensorFromIEDims(startOffset));
            for (size_t i = 0; i < inputDims.size(); i++) {
                if (outLayer->dims[i] != inputDims[i]) {
                    startOffset[i] += outLayer->dims[i];
                }
            }
        }

        auto splitPrim = cldnn::split(
            splitLayerName,
            inputPrimitives[0],
            outputOffsets);
        topology.add(splitPrim);

        // set split as not_run
        InitProfileInfo(splitLayerName, layer->type, "None", InferenceEngine::InferenceEngineProfileInfo::OPTIMIZED_OUT);  // Mark this layer as optimized out

#else  // _USE_SPLIT_PRIMITIVE
        // TODO: replace with clDNN split when it's implemented
        auto inputPrimitives = GetPrevLayersPrimitives(layer);
        auto inDataPtr = splitLayer->insData[0].lock();
        if (!inDataPtr) {
            THROW_CLDNN_EXCEPTION("Data inserts into split layer " << splitLayer->name << " is nullptr");
        }
        auto inputDims = inDataPtr->getTensorDesc().getDims();
        InferenceEngine::SizeVector startOffset(inputDims.size());

        for (auto& outLayer : splitLayer->outData) {
            std::string outLayerName = layer_type_lower(splitLayer) + ":" + outLayer->getName();
            const auto outLayerDims = outLayer->getTensorDesc().getDims();
            if (outLayerDims.size() != startOffset.size()) {
                THROW_CLDNN_EXCEPTION("Invalid dimesions in split layer: " << splitLayer->name << " output: " << outLayer->getName());
            }
            for (size_t i = 0; i < inputDims.size(); i++) {
                if ((outLayerDims[i] + startOffset[i]) > inputDims[i]) {
                    THROW_CLDNN_EXCEPTION("Invalid dimesions in split layer: " << splitLayer->name << " output: " << outLayer->getName());
                }
            }

            auto outTensor = CldnnTensorFromIEDims(outLayerDims, 1);
            auto offsetTensor = CldnnTensorFromIEDims(startOffset, 0);

            auto cropPrim = cldnn::crop(outLayerName, inputPrimitives[0], outTensor, offsetTensor);
            primitivesToIRLayersMap[outLayerName] = { layer->name };
            primitiveIDs[outLayerName] = outLayerName;
            topology.add(cropPrim);
            profilingIDs.push_back(outLayerName);
            InitProfileInfo(outLayerName, "Crop");

            for (size_t i = 0; i < inputDims.size(); i++) {
                if (outLayerDims[i] != inputDims[i]) {
                    startOffset[i] += outLayerDims[i];
                }
            }
        }

        // set split as not_run
        InitProfileInfo(layer->name, layer->type, false, InferenceEngine::InferenceEngineProfileInfo::OPTIMIZED_OUT);  // Mark this layer as optimized out
#endif  // _USE_SPLIT_PRIMITIVE
    }
}

void Program::CreateFusedSplitConvMergePrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    // only handle the split->conv->merge topology for now
    auto splitLayer = as<InferenceEngine::SplitLayer *> (layer);
    IE_ASSERT(IsValidSplitConvMerge(splitLayer));

    auto convLayer1 =
        as<InferenceEngine::ConvolutionLayer *> (GetNextSingleLayer(splitLayer->outData[0]));
    auto convLayer2 =
        as<InferenceEngine::ConvolutionLayer *> (GetNextSingleLayer(splitLayer->outData[1]));
    auto concatLayer =
        as<InferenceEngine::ConcatLayer *> (GetNextSingleLayer(GetNextSingleLayer(splitLayer->outData[0])));

    // Mark these layers as optimized out
    InitProfileInfo(convLayer1->name, convLayer1->type, false, InferenceEngine::InferenceEngineProfileInfo::OPTIMIZED_OUT);
    InitProfileInfo(convLayer2->name, convLayer2->type, false, InferenceEngine::InferenceEngineProfileInfo::OPTIMIZED_OUT);
    InitProfileInfo(concatLayer->name, concatLayer->type, false, InferenceEngine::InferenceEngineProfileInfo::OPTIMIZED_OUT);

    // build the split conv primitive
    std::vector<cldnn::primitive_id> weightPrimID;
    std::vector<cldnn::primitive_id> biasPrimID;
    CreateWeightAndBiasPrimitives(topology, GetNextSingleLayer(splitLayer->outData[0]), weightPrimID, biasPrimID);
    CreateWeightAndBiasPrimitives(topology, GetNextSingleLayer(splitLayer->outData[1]), weightPrimID, biasPrimID);

    auto concatLayerPtr = std::make_shared<InferenceEngine::CNNLayer>(*concatLayer);

    cldnn::tensor stride = cldnn::tensor(cldnn::batch(1), cldnn::feature(1),
                                         cldnn::spatial(convLayer1->_stride[X_AXIS], convLayer1->_stride[Y_AXIS]));
    auto allPad = getPaddings(*convLayer1);
    int x_pad = allPad.begin[X_AXIS], y_pad = allPad.begin[Y_AXIS];
    cldnn::tensor padding = cldnn::tensor(cldnn::batch(0), cldnn::feature(0),
                                          cldnn::spatial(-x_pad, -y_pad, 0));

    cldnn::tensor dilation = cldnn::tensor(cldnn::batch(1), cldnn::feature(1),
                                           cldnn::spatial(convLayer1->_dilation[X_AXIS], convLayer1->_dilation[Y_AXIS]));

    std::string splitLayerName = layer_type_name_ID(layer);
    auto splitPrim = cldnn::convolution(splitLayerName,
                                        inputPrimitives[0],
                                        weightPrimID,
                                        biasPrimID,
                                        stride,
                                        padding,
                                        dilation,
                                        false,
                                        0.0f,
                                        CldnnTensorFromIEDims(concatLayer->outData[0]->getTensorDesc().getDims()));

    layer = concatLayerPtr;

    primitivesToIRLayersMap[splitLayerName] = {convLayer1->name, convLayer2->name, concatLayer->name};
    primitiveIDs[splitLayerName]  = splitLayerName;
    primitiveIDs[layer_type_name_ID(convLayer1)]  = splitLayerName;
    primitiveIDs[layer_type_name_ID(convLayer2)]  = splitLayerName;
    primitiveIDs[layer_type_name_ID(concatLayer)] = splitLayerName;  // pair the last merged layer (concat or relu) with
                                                               // this primitive name to be used as
                                                              // input prim for subsequent layers
    topology.add(splitPrim);
    profilingIDs.push_back(splitLayerName);
}

void Program::CreatePowerPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto powerLayer = as<InferenceEngine::PowerLayer *> (layer);
    if (powerLayer->power != 1.0f && powerLayer->power != 0.5f) {
        auto power = powerLayer->power;
        auto scale = powerLayer->scale;
        auto shift = powerLayer->offset;

        std::string powerLayerName = layer_type_name_ID(layer);
        std::string linearLayerName = powerLayerName + "_linear_activation";
        auto linearActivationPrim = cldnn::activation(linearLayerName, inputPrimitives[0], activation_linear, { scale, shift });
        topology.add(linearActivationPrim);
        profilingIDs.push_back(linearLayerName);
        primitiveIDs[linearLayerName] = linearLayerName;

        auto powActivationPrim = cldnn::activation(powerLayerName, linearLayerName, activation_pow, { power, 0.f });
        topology.add(powActivationPrim);
        profilingIDs.push_back(powerLayerName);
        primitiveIDs[powerLayerName] = powerLayerName;
    } else {
        std::string powerLayerName = layer_type_name_ID(layer);
        if ((powerLayer->scale == 1.0f) && (powerLayer->offset == 0.0f)) {
            if (powerLayer->power == 0.5f) {
                auto activationPrim = cldnn::activation(powerLayerName, inputPrimitives[0], activation_sqrt);
                topology.add(activationPrim);
                profilingIDs.push_back(powerLayerName);
                primitiveIDs[powerLayerName] = powerLayerName;
            }  else {
                // skip this layer
                primitiveIDs[powerLayerName] = inputPrimitives[0];  // register the previous primID for this layer too
                InitProfileInfo(layer->name, layer->type, false, InferenceEngine::InferenceEngineProfileInfo::NOT_RUN);  // Mark this layer as not run
            }
        } else {
            // create scale primitive
            auto scaleValuePrimName = powerLayerName + m_scalesTag;
            AddSingleValuePrimitive(topology, scaleValuePrimName,
                DataTypeFromPrecision(powerLayer->precision),
                powerLayer->scale);

            cldnn::primitive_id biasValuePrimName = "";
            if (powerLayer->offset != 0.0f) {
                biasValuePrimName = powerLayerName + m_biasesTag;
                AddSingleValuePrimitive(topology, biasValuePrimName,
                    DataTypeFromPrecision(powerLayer->precision),
                    powerLayer->offset);
            }
            auto scalePrim = cldnn::scale(
                powerLayerName,
                inputPrimitives[0],
                scaleValuePrimName,
                biasValuePrimName);

            primitiveIDs[powerLayerName] = powerLayerName;
            topology.add(scalePrim);
            profilingIDs.push_back(powerLayerName);

            if (powerLayer->power == 0.5f) {
                auto activationPrim = cldnn::activation(powerLayerName + "_sqrt", powerLayerName, activation_sqrt);
                topology.add(activationPrim);
                profilingIDs.push_back(powerLayerName + "_sqrt");
            }
        }
    }
}

void Program::CreateSoftMaxPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto softmaxLayer = as<InferenceEngine::SoftMaxLayer *> (layer);

    std::string softmaxLayerName = layer_type_name_ID(layer);
    auto softmaxPrim = cldnn::softmax(softmaxLayerName,
                                      inputPrimitives[0],
                                      SoftmaxDimensionFromIEAxis(softmaxLayer));
    primitivesToIRLayersMap[softmaxLayerName] = { layer->name };
    primitiveIDs[softmaxLayerName] = softmaxLayerName;
    topology.add(softmaxPrim);
    profilingIDs.push_back(softmaxLayerName);
}

void Program::CreateFullyConnectedPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto fcLayer = as<InferenceEngine::FullyConnectedLayer *> (layer);

    std::string fcLayerName = layer_type_name_ID(layer);
    // create bias primitive
    cldnn::primitive_id biasesPrimID = "";
    if (fcLayer->blobs.count("biases") != 0) {
        biasesPrimID = fcLayerName + m_biasesTag;
        auto biasesBlob = getBlob(layer, "biases");
        cldnn::layout fcbLayout(
            DataTypeFromPrecision(biasesBlob->getTensorDesc().getPrecision()),
            m_defaultFormat,
            (cldnn::tensor) cldnn::spatial(TensorValue(fcLayer->_out_num)));
        biasesPrimID = CreatePrimitiveFromBlob(topology, biasesPrimID, biasesBlob, fcbLayout);
    }

    // create weights primitive
    // gcc bug to resolve auto, at least for 5.4 version
    std::shared_ptr<Data> insData0 = fcLayer->insData[0].lock();
    IE_ASSERT(insData0 != nullptr);
    cldnn::primitive_id weightsPrimID = fcLayerName + m_weightsTag;
    cldnn::tensor weightsDims;
    InferenceEngine::SizeVector insData0dims(insData0->getTensorDesc().getDims());
    switch (insData0dims.size()) {
    case 4:
        weightsDims = { TensorValue(fcLayer->outData[0]->getTensorDesc().getDims().back()),
                        TensorValue(insData0dims[1]),
                        TensorValue(insData0dims[3]),
                        TensorValue(insData0dims[2]) };
        break;
    case 3:
        weightsDims = { TensorValue(fcLayer->outData[0]->getTensorDesc().getDims().back()),
                        TensorValue(insData0dims[1]),
                        1,
                        TensorValue(insData0dims[2])};
        break;
    case 2:
        weightsDims = { TensorValue(fcLayer->outData[0]->getTensorDesc().getDims().back()), TensorValue(insData0dims[1]), 1, 1 };
        break;
    default: THROW_CLDNN_EXCEPTION("Invalid data dimensions");
    }
    auto weightsBlob = getBlob(layer, "weights");
    cldnn::layout fcwLayout(
        DataTypeFromPrecision(weightsBlob->getTensorDesc().getPrecision()),
        m_defaultFormat,
        weightsDims);
    weightsPrimID = CreatePrimitiveFromBlob(topology, weightsPrimID, weightsBlob, fcwLayout);

    auto inputPrecision = layer->insData[0].lock()->getPrecision();
    auto inputQuantized =
        inputPrecision == InferenceEngine::Precision::I8 ||
        inputPrecision == InferenceEngine::Precision::U8;

    auto outputPrecision = layer->outData[0]->getPrecision();
    auto outputQuantized =
        outputPrecision == InferenceEngine::Precision::I8 ||
        outputPrecision == InferenceEngine::Precision::U8;

    std::vector<cldnn::primitive_id> wQuantizationPrimID;

    CreateQuantizationPrimitives(topology, layer, wQuantizationPrimID, true);

    if (!inputQuantized || outputQuantized) {
        // Either no quantization or output is also quantized
        auto fcPrim = cldnn::fully_connected(fcLayerName,
                                             inputPrimitives[0],
                                             weightsPrimID,
                                             biasesPrimID,
                                             false,
                                             0.0f);

        // Add quantization
        if (!wQuantizationPrimID.empty()) {
            fcPrim.weights_quantization_factors = wQuantizationPrimID[0];
        }

        topology.add(fcPrim);
    } else {
        // Output is supposed to be float.
        // Currently fully_connected does not support output data type forcing,
        // so 1x1 convolution is used instead.

        if (wQuantizationPrimID.empty())
            THROW_CLDNN_EXCEPTION("Could not find dequantization scales for " << fcLayerName);

        // We need to flatten weights for 1x1 kernel
        if (insData0dims.size() != 2) {
            auto newWeightsPrimID = weightsPrimID + "_cldnn_w_flatten";
            auto newShape = cldnn::tensor(
                cldnn::batch(weightsDims.batch[0]),
                cldnn::feature(weightsDims.feature[0] * weightsDims.spatial[0] * weightsDims.spatial[1]));
            auto reshapePrim = cldnn::reshape(newWeightsPrimID, weightsPrimID, newShape);

            topology.add(reshapePrim);
            weightsPrimID = newWeightsPrimID;
        }

        auto convPrim = cldnn::convolution(fcLayerName,
                                           inputPrimitives[0],
                                           { weightsPrimID },
                                           { biasesPrimID },
                                           cldnn::tensor(1),
                                           cldnn::tensor(0),
                                           cldnn::tensor(1),
                                           false,
                                           0.f);

        convPrim.output_data_type = DataTypeFromPrecision(outputPrecision);

        // TODO Fix in clDNN - there is no reason this should be immutable, most other fields are mutable
        auto& wq = const_cast<std::vector<cldnn::primitive_id>&>(convPrim.weights_quantization_factors.ref());
        wq.insert(wq.end(), wQuantizationPrimID.begin(), wQuantizationPrimID.end());

        topology.add(convPrim);
    }

    primitivesToIRLayersMap[fcLayerName] = { layer->name };
    primitiveIDs[fcLayerName] = fcLayerName;
    profilingIDs.push_back(fcLayerName);
}

void Program::CreatePoolingPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto poolLayer = as<InferenceEngine::PoolingLayer*>(layer);

    std::string poolLayerName = layer_type_name_ID(layer);
    auto allPads = getPaddings(*poolLayer);
    if (poolLayer->outData.size() > 1) {
        // max pooling with argmax
        SizeVector argmaxDims;

        std::string realOutputID, argmaxOutputID;
        int outputOrder = 0;

        for (auto out : poolLayer->outData) {
            auto layersMap = out->getInputTo();

            for (auto item : layersMap) {
                bool isUpooling = (LayerTypeFromStr(item.second->type) == Unpooling);
                if (outputOrder == 1 && isUpooling) {
                    argmaxDims = InferenceEngine::SizeVector(out->getTensorDesc().getDims());
                    argmaxOutputID = out->getName();
                } else {
                    realOutputID = out->getName();
                }
                outputOrder++;
            }
        }

        // create mutable_data primitive for storing argmax data
        cldnn::tensor mutableTensor;
        switch (argmaxDims.size()) {
        case 4: mutableTensor = cldnn::tensor(TensorValue(argmaxDims[0]), TensorValue(argmaxDims[1]),
                                              TensorValue(argmaxDims[3]), TensorValue(argmaxDims[2]));
            break;
        case 3: mutableTensor = cldnn::tensor(TensorValue(argmaxDims[0]), TensorValue(argmaxDims[1]),
                                              1, TensorValue(argmaxDims[2]));
            break;
        case 2: mutableTensor = cldnn::tensor(TensorValue(argmaxDims[0]), TensorValue(argmaxDims[1]), 1, 1);
            break;
        case 1:  // not implemented yet.
        default: THROW_CLDNN_EXCEPTION("Invalid constant blob dimensions");
        }

        cldnn::layout mutableLayout = cldnn::layout(
            cldnn::data_types::f32,
            m_defaultFormat,
            mutableTensor);

        cldnn::primitive_id argmaxPrimID = layer->name + "_argmax_mutable";

        auto mem = cldnn::memory::allocate(*m_engine, mutableLayout);
        auto argmax_mutable_prim = cldnn::mutable_data(argmaxPrimID, mem);
        topology.add(argmax_mutable_prim);
        primitivesToIRLayersMap[argmaxPrimID] = { layer->name };
        primitivesToIRLayersMap[argmaxOutputID] = { layer->name };
        primitiveIDs[argmaxPrimID] = argmaxPrimID;
        primitiveIDs[argmaxOutputID] = argmaxPrimID;

        // create pooling primitive itself
        auto poolPrim = cldnn::pooling(poolLayerName,
            inputPrimitives[0],
            argmaxPrimID,
            cldnn::pooling_mode::max_with_argmax,
            (cldnn::tensor) cldnn::spatial(TensorValue(poolLayer->_kernel[X_AXIS]), TensorValue(poolLayer->_kernel[Y_AXIS])),  // size
            (cldnn::tensor) cldnn::spatial(TensorValue(poolLayer->_stride[X_AXIS]), TensorValue(poolLayer->_stride[Y_AXIS])),  // stride
            // input offset (padding) - explicit tensor for 0 bf
            cldnn::tensor { 0, 0, -TensorValue(allPads.begin[X_AXIS]), -TensorValue(allPads.begin[Y_AXIS]), 0 },
            CldnnTensorFromIEDims(poolLayer->outData[0]->getTensorDesc().getDims()));

        topology.add(poolPrim);
        primitiveIDs[realOutputID] = poolLayerName;
    } else {
        // regular pooling
        cldnn::tensor size, stride, input_offset;

        if (poolLayer->input()->getTensorDesc().getDims().size() > 4) {
            size = (cldnn::tensor) cldnn::spatial(TensorValue(poolLayer->_kernel[X_AXIS]),
                                  TensorValue(poolLayer->_kernel[Y_AXIS]),
                                  TensorValue(poolLayer->_kernel[Z_AXIS]));
            stride = (cldnn::tensor) cldnn::spatial(TensorValue(poolLayer->_stride[X_AXIS]),
                                    TensorValue(poolLayer->_stride[Y_AXIS]),
                                    TensorValue(poolLayer->_stride[Z_AXIS]));
            input_offset = { 0, 0, -TensorValue(allPads.begin[X_AXIS]),
                                   -TensorValue(allPads.begin[Y_AXIS]),
                                   -TensorValue(allPads.begin[Z_AXIS]) };
        } else {
            size = (cldnn::tensor) cldnn::spatial(TensorValue(poolLayer->_kernel[X_AXIS]), TensorValue(poolLayer->_kernel[Y_AXIS]));
            stride = (cldnn::tensor) cldnn::spatial(TensorValue(poolLayer->_stride[X_AXIS]), TensorValue(poolLayer->_stride[Y_AXIS]));
            input_offset = { 0, 0, -TensorValue(allPads.begin[X_AXIS]), -TensorValue(allPads.begin[Y_AXIS]) };
        }

        auto poolPrim = cldnn::pooling(poolLayerName,
            inputPrimitives[0],
            PoolingModeFromIEPooling(poolLayer->_type, poolLayer->_exclude_pad),
            size,
            stride,
            input_offset,
            CldnnTensorFromIEDims(poolLayer->outData[0]->getTensorDesc().getDims()));
        topology.add(poolPrim);
        primitiveIDs[poolLayerName] = poolLayerName;
    }

    primitivesToIRLayersMap[poolLayerName] = { layer->name };
    profilingIDs.push_back(poolLayerName);
}

void Program::CreateLRNPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto lrnLayer = as<InferenceEngine::NormLayer *> (layer);
    std::string lrnLayerName = layer_type_name_ID(layer);
    auto lrnPrim = cldnn::lrn(
        lrnLayerName,
        inputPrimitives[0],
        lrnLayer->_size,
        static_cast<float>(lrnLayer->_k),
        lrnLayer->_alpha,
        lrnLayer->_beta,
        lrnLayer->_isAcrossMaps ? cldnn_lrn_norm_region_across_channel : cldnn_lrn_norm_region_within_channel);

    primitivesToIRLayersMap[lrnLayerName] = { layer->name };
    primitiveIDs[lrnLayerName] = lrnLayerName;
    topology.add(lrnPrim);
    profilingIDs.push_back(lrnLayerName);
}

void Program::CreateActivationPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer, const LayerType type) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    cldnn_activation_additional_params params{ 0.0f, 0.0f };
    cldnn_activation_func func = cldnn_activation_func_t::activation_none;

    LayerType activationType;
    if (type == Activation) {
        std::string activation_type = layer->GetParamAsString("type");
        if (activation_type == "tanh") {
            activationType = TanH;
        } else if (activation_type == "sigmoid" || activation_type == "logistic")  {
            activationType = Sigmoid;
        } else if (activation_type == "elu")  {
            activationType = ELU;
        } else if (activation_type == "relu")  {
            activationType = ReLU;
        } else if (activation_type == "relu6")  {
            activationType = ReLU6;
        } else if (activation_type == "clamp")  {
            activationType = Clamp;
        } else if (activation_type == "exp")  {
            activationType = Exp;
        } else if (activation_type == "not")  {
            activationType = Not;
        } else {
            THROW_CLDNN_EXCEPTION("Unsupported activation type (" + activation_type +
                                  ") in layer " + layer->name);
        }
    } else {
        activationType = type;
    }

    switch (activationType) {
    case TanH:
    {
        func = cldnn_activation_func_t::activation_hyperbolic_tan;
        break;
    }
    case ELU:
    {
        func = cldnn_activation_func_t::activation_elu;
        params.a = layer->GetParamAsFloat("alpha", 1.0f);
        break;
    }
    case Sigmoid:
    {
        func = cldnn_activation_func_t::activation_logistic;
        break;
    }
    case ReLU:
    {
        auto negative_slope = layer->GetParamAsFloat("negative_slope", 0.0f);
        if (negative_slope == 0.f) {
            func = cldnn_activation_func_t::activation_relu;
        } else {
            func = cldnn_activation_func_t::activation_relu_negative_slope;
            params.a = negative_slope;
        }
        break;
    }
    case ReLU6:
    {
        func = cldnn_activation_func_t::activation_clamp;
        params.b = layer->GetParamAsFloat("n", 6.0f);
        break;
    }
    case Clamp:
    {
        func = cldnn_activation_func_t::activation_clamp;
        params.a = layer->GetParamAsFloat("min");
        params.b = layer->GetParamAsFloat("max");
        break;
    }
    case Exp:
    {
        func = cldnn_activation_func_t::activation_exp;
        break;
    }
    case Not:
    {
        func = cldnn_activation_func_t::activation_not;
        break;
    }
    case Asin:
    {
        func = cldnn_activation_func_t::activation_asin;
        break;
    }
    case Asinh:
    {
        func = cldnn_activation_func_t::activation_asinh;
        break;
    }
    case Acos:
    {
        func = cldnn_activation_func_t::activation_acos;
        break;
    }
    case Acosh:
    {
        func = cldnn_activation_func_t::activation_acosh;
        break;
    }
    case Atan:
    {
        func = cldnn_activation_func_t::activation_atan;
        break;
    }
    case Atanh:
    {
        func = cldnn_activation_func_t::activation_atanh;
        break;
    }
    case Abs:
    {
        func = cldnn_activation_func_t::activation_abs;
        break;
    }
    case Floor:
    {
        func = cldnn_activation_func_t::activation_floor;
        break;
    }
    case Ceil:
    {
        func = cldnn_activation_func_t::activation_ceil;
        break;
    }
    case Erf:
    {
        func = cldnn_activation_func_t::activation_erf;
        break;
    }
    case HardSigmoid:
    {
        func = cldnn_activation_func_t::activation_hard_sigmoid;
        break;
    }
    case Log:
    {
        func = cldnn_activation_func_t::activation_log;
        break;
    }
    case Neg:
    {
        func = cldnn_activation_func_t::activation_negative;
        break;
    }
    case Reciprocal:
    {
        func = cldnn_activation_func_t::activation_reciprocal;
        break;
    }
    case Selu:
    {
        func = cldnn_activation_func_t::activation_selu;
        break;
    }
    case SoftPlus:
    {
        func = cldnn_activation_func_t::activation_softplus;
        break;
    }
    case SoftSign:
    {
        func = cldnn_activation_func_t::activation_softsign;
        break;
    }
    case Tan:
    {
        func = cldnn_activation_func_t::activation_tan;
        break;
    }
    default:
        THROW_CLDNN_EXCEPTION("Unsupported activation type (" + layer->type +
                              ") in layer " + layer->name);
    }

    std::string layerName = layer_type_name_ID(layer);
    auto activationPrimitive = cldnn::activation(layerName, inputPrimitives[0], func, params);
    primitivesToIRLayersMap[layerName] = { layer->name };
    primitiveIDs[layerName] = layerName;
    topology.add(activationPrimitive);
    profilingIDs.push_back(layerName);
}

void Program::CreateCopyPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);

    // Optimize out and just update references
    std::string layerName = layer_type_name_ID(layer);
    primitivesToIRLayersMap[layerName] = { layer->name };
    primitiveIDs[layerName] = inputPrimitives[0];
    InitProfileInfo(layerName, layer->type, false, InferenceEngine::InferenceEngineProfileInfo::OPTIMIZED_OUT);  // Mark this layer as optimized out
}

void Program::CreateUpsamplingPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    // Assuming multi-input will be handled by prev concat/eltwise layers
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto upsamplingLayer = as<InferenceEngine::GenericLayer*> (layer);
    uint32_t scale = upsamplingLayer->GetParamAsUInt("scale");
    uint32_t numFilter = upsamplingLayer->GetParamAsUInt("num_filter");
    std::string sampleType = upsamplingLayer->GetParamAsString("sample_type");

    std::string upsamplingLayerName = layer_type_name_ID(layer);
    auto upsamplingPrim = cldnn::upsampling(
        upsamplingLayerName,
        inputPrimitives[0],
        scale,
        numFilter,
        UpsamplingTypeFromString(sampleType));

    primitivesToIRLayersMap[upsamplingLayerName] = { layer->name };
    primitiveIDs[upsamplingLayerName] = upsamplingLayerName;
    topology.add(upsamplingPrim);
    profilingIDs.push_back(upsamplingLayerName);
}

void Program::CreateResamplePrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto resampleLayer = as<InferenceEngine::GenericLayer*> (layer);

    size_t inFeatures = 1;
    float scale = 1.0f;
    std::shared_ptr<Data> insData0 = layer->insData[0].lock();
    IE_ASSERT(insData0 != nullptr);
    auto insData0dims = insData0->getTensorDesc().getDims();
    auto outDims = layer->outData[0]->getTensorDesc().getDims();

    if (insData0dims.size() > 1) {
        inFeatures = insData0dims[1];
        scale = static_cast<float>(outDims.back()) / static_cast<float>(insData0dims.back());
        if (scale < 1.0f) {
            THROW_CLDNN_EXCEPTION("Unsupported scale in layer " + layer->name);
        }
    }
    std::string sampleType = resampleLayer->GetParamAsString("type");
    std::string resampleLayerName = layer_type_name_ID(layer);

    cldnn::upsampling_sample_type cldnnSampleType;
    if (sampleType == "caffe.ResampleParameter.NEAREST") {
        cldnnSampleType = cldnn::upsampling_sample_type::nearest;
    } else if (sampleType == "caffe.ResampleParameter.LINEAR") {
        cldnnSampleType = cldnn::upsampling_sample_type::bilinear;
    } else {
        THROW_CLDNN_EXCEPTION("Unsupported resampling type (" + sampleType + ") in layer " + layer->name);
    }
    auto upsamplingPrim = cldnn::upsampling(
        resampleLayerName,
        inputPrimitives[0],
        scale,
        inFeatures,
        cldnnSampleType);

    primitivesToIRLayersMap[resampleLayerName] = { layer->name };
    primitiveIDs[resampleLayerName] = resampleLayerName;
    topology.add(upsamplingPrim);
    profilingIDs.push_back(resampleLayerName);
}

void Program::CreateYOLO2RegionPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto YOLOregionLayer = as<InferenceEngine::GenericLayer*> (layer);

    uint32_t coords = YOLOregionLayer->GetParamAsUInt("coords", 4);
    uint32_t classes = YOLOregionLayer->GetParamAsUInt("classes", 20);
    uint32_t num = YOLOregionLayer->GetParamAsUInt("num", 1);
    bool do_softmax = YOLOregionLayer->GetParamAsBool("do_softmax", true);

    uint32_t mask_size = 0;
    if (HasParam(YOLOregionLayer->params, "mask")) {
        const auto mask = YOLOregionLayer->GetParamAsInts("mask");
        mask_size = static_cast<uint32_t>(mask.size());
    }

    std::string YOLOregionLayerName = layer_type_name_ID(layer);
    auto regionPrim = cldnn::region_yolo(
        YOLOregionLayerName,
        inputPrimitives[0],
        coords,
        classes,
        num,
        mask_size,
        do_softmax);

    primitivesToIRLayersMap[YOLOregionLayerName] = { layer->name };
    primitiveIDs[YOLOregionLayerName] = YOLOregionLayerName;
    topology.add(regionPrim);
    profilingIDs.push_back(YOLOregionLayerName);
}

void Program::CreateYOLO2ReorgPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto YOLOreorgLayer = as<InferenceEngine::GenericLayer*> (layer);
    uint32_t stride = YOLOreorgLayer->GetParamAsUInt("stride");

    std::string YOLOreorgLayerName = layer_type_name_ID(layer);
    auto reorgPrim = cldnn::reorg_yolo(
        YOLOreorgLayerName,
        inputPrimitives[0],
        stride);

    primitivesToIRLayersMap[YOLOreorgLayerName] = { layer->name };
    primitiveIDs[YOLOreorgLayerName] = YOLOreorgLayerName;
    topology.add(reorgPrim);
    profilingIDs.push_back(YOLOreorgLayerName);
}

void Program::CreateArgMaxMinPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer, const LayerType type) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto ArgMaxLayer = as<InferenceEngine::GenericLayer*> (layer);
    const cldnn::arg_max_min::out_type otype = type == ArgMin ? cldnn::arg_max_min::out_type::min : cldnn::arg_max_min::out_type::max;

    if (HasParam(ArgMaxLayer->params, "out_max_val")) {
        int32_t out_max_val_flag = ArgMaxLayer->GetParamAsInt("out_max_val");
        if (out_max_val_flag != 0) {
            THROW_IE_EXCEPTION << NOT_IMPLEMENTED_str << "ArgMax: out_max_val param is not supported for layer: " << layer->name;
        }
    }

    uint32_t top_k = ArgMaxLayer->GetParamAsUInt("top_k", 1);

    cldnn::arg_max_min::axis_name chosen_axis = cldnn::arg_max_min::axis_name::xyf;

    if (HasParam(ArgMaxLayer->params, "axis")) {
        int32_t axis_param = ArgMaxLayer->GetParamAsInt("axis", 1);

        int32_t axis = axis_param;
        if (ArgMaxLayer->outData[0]->getTensorDesc().getDims().size() == 5) {
            if (-5 <= axis && axis <= -1)
                axis += 5;

            switch (axis) {
                case 0: chosen_axis = cldnn::arg_max_min::axis_name::batch; break;
                case 1: chosen_axis = cldnn::arg_max_min::axis_name::feature; break;
                case 2: chosen_axis = cldnn::arg_max_min::axis_name::z; break;
                case 3: chosen_axis = cldnn::arg_max_min::axis_name::y; break;
                case 4: chosen_axis = cldnn::arg_max_min::axis_name::x; break;
            }
        } else {
            if (-4 <= axis && axis <= -1)
                axis += 4;

            switch (axis) {
                case 0: chosen_axis = cldnn::arg_max_min::axis_name::batch; break;
                case 1: chosen_axis = cldnn::arg_max_min::axis_name::feature; break;
                case 2: chosen_axis = cldnn::arg_max_min::axis_name::y; break;
                case 3: chosen_axis = cldnn::arg_max_min::axis_name::x; break;
            }
        }
    }

    std::string ArgMaxLayerName = layer_type_name_ID(layer);
    auto argmaxPrim = cldnn::arg_max_min(
        ArgMaxLayerName,
        inputPrimitives,
        otype,
        top_k,
        chosen_axis);

    primitivesToIRLayersMap[ArgMaxLayerName] = { layer->name };
    primitiveIDs[ArgMaxLayerName] = ArgMaxLayerName;
    topology.add(argmaxPrim);
    profilingIDs.push_back(ArgMaxLayerName);
}

void Program::CreateTopKPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 2);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto ArgMaxLayer = as<InferenceEngine::TopKLayer*> (layer);

    cldnn::arg_max_min::out_type otype;
    cldnn::arg_max_min::sort_type stype;

    if (layer->GetParamAsString("mode", "max") == "max")
        otype = cldnn::arg_max_min::out_type::max;
    else
        otype = cldnn::arg_max_min::out_type::min;

    if (layer->GetParamAsString("sort", "value") == "value")
        stype = cldnn::arg_max_min::sort_type::sort_by_values;
    else
        stype = cldnn::arg_max_min::sort_type::sort_by_indices;

    auto topKInput = layer->insData[1].lock();
    auto topKInputCreator = topKInput->getCreatorLayer().lock();

    std::vector<int32_t> topk;
    if (topKInputCreator->blobs.size() == 1) {
        auto constantBlob = topKInputCreator->blobs.begin()->second;
        auto axesPrecision = constantBlob->getTensorDesc().getPrecision();
        if (axesPrecision == InferenceEngine::Precision::FP32) {
            auto data = constantBlob->buffer().as<float*>();
            for (size_t i = 0; i < constantBlob->size(); ++i)
                topk.push_back(data[i]);
        } else if (axesPrecision == InferenceEngine::Precision::I32) {
            auto data = constantBlob->buffer().as<int32_t*>();
            for (size_t i = 0; i < constantBlob->size(); ++i)
                topk.push_back(data[i]);
        } else {
            THROW_IE_EXCEPTION << layer->name << " Incorrect TopK input Precision";
        }
    }

    uint32_t top_k = topk[0];

    cldnn::arg_max_min::axis_name chosen_axis = cldnn::arg_max_min::axis_name::batch;

    if (HasParam(ArgMaxLayer->params, "axis")) {
        int32_t axis_param = ArgMaxLayer->GetParamAsInt("axis", -1);

        auto input_dims_num = ArgMaxLayer->outData[0]->getTensorDesc().getDims().size();
        int32_t axis = axis_param;
        if (input_dims_num == 5) {
            if (-5 <= axis && axis <= -1)
                axis += 5;

            switch (axis) {
                case 0: chosen_axis = cldnn::arg_max_min::axis_name::batch; break;
                case 1: chosen_axis = cldnn::arg_max_min::axis_name::feature; break;
                case 2: chosen_axis = cldnn::arg_max_min::axis_name::z; break;
                case 3: chosen_axis = cldnn::arg_max_min::axis_name::y; break;
                case 4: chosen_axis = cldnn::arg_max_min::axis_name::x; break;
            }
        } else {
            if (-input_dims_num <= axis && axis <= -1)
                axis += input_dims_num;

            switch (axis) {
                case 0: chosen_axis = cldnn::arg_max_min::axis_name::batch; break;
                case 1: chosen_axis = cldnn::arg_max_min::axis_name::feature; break;
                case 2: chosen_axis = cldnn::arg_max_min::axis_name::y; break;
                case 3: chosen_axis = cldnn::arg_max_min::axis_name::x; break;
            }
        }
    }

    if (layer->outData.size() == 2) {
        cldnn::layout mutableLayout = cldnn::layout(
                DataTypeFromPrecision(layer->outData[1]->getPrecision()),
                m_defaultFormat,
                CldnnTensorFromIEDims(layer->outData[1]->getDims()));

        auto shared_memory = cldnn::memory::allocate(*m_engine, mutableLayout);

        cldnn::primitive_id argmax_mutable_id_w = layer_type_name_ID(layer) + "_md_write";
        auto argmax_mutable_prim = cldnn::mutable_data(argmax_mutable_id_w, shared_memory);
        primitivesToIRLayersMap[argmax_mutable_id_w] = {layer->name};
        primitiveIDs[argmax_mutable_id_w] = argmax_mutable_id_w;
        topology.add(argmax_mutable_prim);
        inputPrimitives.push_back(argmax_mutable_id_w);

        std::string ArgMaxLayerName = layer_type_lower(layer) + ":" + layer->outData[1]->getName();
        auto argmaxPrim = cldnn::arg_max_min(
                ArgMaxLayerName,
                inputPrimitives,
                otype,
                top_k,
                chosen_axis,
                stype,
                true);

        primitivesToIRLayersMap[ArgMaxLayerName] = {layer->name};
        primitiveIDs[ArgMaxLayerName] = ArgMaxLayerName;
        topology.add(argmaxPrim);

        cldnn::primitive_id argmax_mutable_id_r = layer_type_lower(layer) + ":" + layer->outData[0]->getName();
        auto argmax_mutable_prim_r = cldnn::mutable_data(argmax_mutable_id_r, {ArgMaxLayerName}, shared_memory);
        primitivesToIRLayersMap[argmax_mutable_id_r] = {layer->name};
        primitiveIDs[argmax_mutable_id_r] = argmax_mutable_id_r;
        topology.add(argmax_mutable_prim_r);

        profilingIDs.push_back(ArgMaxLayerName);
    } else if (layer->outData.size() == 1) {
        std::string ArgMaxLayerName = layer_type_lower(layer) + ":" + layer->outData[0]->getName();
        auto argmaxPrim = cldnn::arg_max_min(
                ArgMaxLayerName,
                inputPrimitives,
                otype,
                top_k,
                chosen_axis,
                stype,
                true);

        primitivesToIRLayersMap[ArgMaxLayerName] = {layer->name};
        primitiveIDs[ArgMaxLayerName] = ArgMaxLayerName;
        topology.add(argmaxPrim);
        profilingIDs.push_back(ArgMaxLayerName);
    } else {
        THROW_IE_EXCEPTION << layer->name << " Incorrect TopK outputs number";
    }
}

void Program::CreateMaxUnpoolingPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 2);

    auto UnpoolingLayer = as<InferenceEngine::GenericLayer*> (layer);

    cldnn::primitive_id real_input, argmax_mutable;

    // locate ArgMax primitive
    int inputOrder = 0;
    for (auto inputData : layer->insData) {
        auto prevData = inputData.lock();

        if (prevData == nullptr) {
            THROW_CLDNN_EXCEPTION("MaxUnpooling: nonexistent input for layer: " << layer->name);
        }

        auto prevCreator = prevData->getCreatorLayer().lock();

        if (prevCreator &&
            (LayerTypeFromStr(prevCreator->type) == Pooling) &&
            prevCreator->outData.size() > 1 &&
            inputOrder == 1) {
            argmax_mutable = primitiveIDs.at(prevCreator->name + "_argmax_mutable");
        } else {
            real_input = primitiveIDs.at(prevData->getName());
        }
        inputOrder++;
    }

    uint32_t stride = UnpoolingLayer->GetParamAsUInt("stride");
    uint32_t kernel_size = UnpoolingLayer->GetParamAsUInt("kernel_size");

    std::string UnpoolingLayerName = layer_type_name_ID(layer);
    auto unpoolingPrim = cldnn::max_unpooling(
        UnpoolingLayerName,
        real_input,
        argmax_mutable,
        (cldnn::tensor) cldnn::spatial(kernel_size, kernel_size),  // size
        (cldnn::tensor) cldnn::spatial(stride, stride) );          // stride

    primitivesToIRLayersMap[UnpoolingLayerName] = { layer->name };
    primitiveIDs[UnpoolingLayerName] = UnpoolingLayerName;
    topology.add(unpoolingPrim);
    profilingIDs.push_back(UnpoolingLayerName);
}

void Program::CreateMVNPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto MvnLayer = as<InferenceEngine::GenericLayer*> (layer);

    bool across_channels = MvnLayer->GetParamAsBool("across_channels", false);
    bool normalize_variance = MvnLayer->GetParamAsBool("normalize_variance", true);
    float eps = MvnLayer->GetParamAsFloat("eps", 1e-10f);

    std::string MvnLayerName = layer_type_name_ID(layer);
    auto mvnPrim = cldnn::mvn(
        MvnLayerName,
        inputPrimitives[0],
        across_channels,
        normalize_variance,
        eps);

    primitivesToIRLayersMap[MvnLayerName] = { layer->name };
    primitiveIDs[MvnLayerName] = MvnLayerName;
    topology.add(mvnPrim);
    profilingIDs.push_back(MvnLayerName);
}

void Program::CreateTilePrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto tileLayer = as<InferenceEngine::GenericLayer*> (layer);

    int axis = tileLayer->GetParamAsInt("axis", 1);
    int tiles = tileLayer->GetParamAsInt("tiles");

    auto sz = tileLayer->input().get()->getTensorDesc().getDims().size();

    auto cldnnAxisFromIE = [&](int axis) {
        switch (axis) {
            case 0: return cldnn::tile::tile_axis::along_b;
            case 1: return cldnn::tile::tile_axis::along_f;
            case 2:
                if (sz > 4)
                    return cldnn::tile::tile_axis::along_z;
                else
                    return cldnn::tile::tile_axis::along_y;
            case 3:
                if (sz > 4)
                    return cldnn::tile::tile_axis::along_y;
                else
                    return cldnn::tile::tile_axis::along_x;
            case 4: return cldnn::tile::tile_axis::along_x;
            default: THROW_CLDNN_EXCEPTION("Unsupported tile axis: " << axis);
        }
    };

    std::string tileLayerName = layer_type_name_ID(layer);
    auto tilePrim = cldnn::tile(
        tileLayerName,
        inputPrimitives[0],
        cldnnAxisFromIE(axis),
        tiles);

    primitivesToIRLayersMap[tileLayerName] = { layer->name };
    primitiveIDs[tileLayerName] = tileLayerName;
    topology.add(tilePrim);
    profilingIDs.push_back(tileLayerName);
}

void Program::CreatePadPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto padLayer = as<InferenceEngine::GenericLayer*> (layer);

    auto PadTensorFromArgs = [](const std::string &s) -> cldnn::tensor {
        std::stringstream ss(s);
        std::string item;
        std::vector<cldnn::tensor::value_type> elems;
        while (std::getline(ss, item, ',')) {
            elems.push_back(static_cast<cldnn::tensor::value_type>(std::atoll(item.c_str())));
        }

        while (elems.size() < 4) {
            elems.push_back(0);
        }

        // Swap x and y
        auto tmp = elems[2];
        elems[2] = elems[3];
        elems[3] = tmp;

        return cldnn::tensor(elems, 0);
    };

    auto pads_begin = PadTensorFromArgs(padLayer->GetParamAsString("pads_begin"));
    auto pads_end = PadTensorFromArgs(padLayer->GetParamAsString("pads_end"));
    std::string mode = padLayer->GetParamAsString("pad_mode");
    float pad_value = padLayer->GetParamAsFloat("pad_value", 0.0f);

    cldnn::border_type border_mode;
    if (mode == "constant")
        border_mode = cldnn::border_type::constant;
    else if (mode == "edge")
        border_mode = cldnn::border_type::edge;
    else if (mode == "symmetric")
        border_mode = cldnn::border_type::mirror;
    else if (mode == "reflect")
        border_mode = cldnn::border_type::mirror_101;
    else
        THROW_CLDNN_EXCEPTION("Invalid border mode " << mode << " in layer " << padLayer->name);

    std::string padLayerName = layer_type_name_ID(layer);
    auto tilePrim = cldnn::border(
            padLayerName,
            inputPrimitives[0],
            pads_begin,
            pads_end,
            border_mode,
            pad_value);

    primitivesToIRLayersMap[padLayerName] = { layer->name };
    primitiveIDs[padLayerName] = padLayerName;
    topology.add(tilePrim);
    profilingIDs.push_back(padLayerName);
}

void Program::AddConstantBlobInput(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    if (layer->blobs.empty())
        THROW_IE_EXCEPTION << "No blobs found in const layer " << layer->name;
    auto constBlob = layer->blobs.begin()->second;
    SizeVector constDims(layer->outData[0]->getTensorDesc().getDims());

    cldnn::tensor constTensor;
    switch (constDims.size()) {
    case 6: constTensor = cldnn::tensor(TensorValue(constDims[0]), TensorValue(constDims[1]),
        TensorValue(constDims[5]), TensorValue(constDims[4]),
        TensorValue(constDims[3]), TensorValue(constDims[2]));
        break;
    case 5: constTensor = cldnn::tensor(TensorValue(constDims[0]), TensorValue(constDims[1]),
        TensorValue(constDims[4]), TensorValue(constDims[3]), TensorValue(constDims[2]));
        break;
    case 4: constTensor = cldnn::tensor(TensorValue(constDims[0]), TensorValue(constDims[1]),
        TensorValue(constDims[3]), TensorValue(constDims[2]));
        break;
    case 3: constTensor = cldnn::tensor(TensorValue(constDims[0]), TensorValue(constDims[1]),
        1, TensorValue(constDims[2]));
        break;
    case 2: constTensor = cldnn::tensor(TensorValue(constDims[0]), TensorValue(constDims[1]), 1, 1);
        break;
    case 1: constTensor = cldnn::tensor(1, TensorValue(constDims[0]), 1, 1);
        break;
    default: THROW_CLDNN_EXCEPTION("Invalid constant blob dimensions");
    }
    cldnn::layout constLayout = cldnn::layout(
        DataTypeFromPrecision(layer->blobs.begin()->second->getTensorDesc().getPrecision()),
        FormatFromLayout(constBlob->getTensorDesc().getLayout()),
        constTensor);

    cldnn::primitive_id initialconstPrimID = layer_type_name_ID(layer);
    cldnn::primitive_id constPrimID = CreatePrimitiveFromBlob(topology, initialconstPrimID, constBlob, constLayout);
    primitiveIDs[initialconstPrimID] = constPrimID;
    primitivesToIRLayersMap[initialconstPrimID] = { layer->name };
}

void Program::CreateConvolutionPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto convLayer = as<InferenceEngine::ConvolutionLayer*>(layer);
    std::string convLayerName = layer_type_name_ID(layer);

    std::vector<cldnn::primitive_id> weightPrimID;
    std::vector<cldnn::primitive_id> biasPrimID;
    CreateWeightAndBiasPrimitives(topology, layer, weightPrimID, biasPrimID);

    auto allPads = getPaddings(*convLayer);
    int x_pad = allPads.begin[X_AXIS], y_pad = allPads.begin[Y_AXIS];
    cldnn::tensor stride, padding, dilation;
    if (convLayer->input()->getTensorDesc().getDims().size() > 4) {
        stride = cldnn::tensor(cldnn::batch(1), cldnn::feature(1),
            cldnn::spatial(convLayer->_stride[X_AXIS],
                            convLayer->_stride[Y_AXIS],
                            convLayer->_stride[Z_AXIS]));
        int z_pad = allPads.begin[Z_AXIS];
        padding = cldnn::tensor(cldnn::batch(0), cldnn::feature(0),
            cldnn::spatial(-x_pad, -y_pad, -z_pad));
        dilation = cldnn::tensor(cldnn::batch(1), cldnn::feature(1),
            cldnn::spatial(convLayer->_dilation[X_AXIS],
                           convLayer->_dilation[Y_AXIS],
                           convLayer->_dilation[Z_AXIS]));

    } else {
        stride = cldnn::tensor(cldnn::batch(1), cldnn::feature(1),
            cldnn::spatial(convLayer->_stride[X_AXIS], convLayer->_stride[Y_AXIS]));
        padding = cldnn::tensor(cldnn::batch(0), cldnn::feature(0),
            cldnn::spatial(-x_pad, -y_pad, 0));
        dilation = cldnn::tensor(cldnn::batch(1), cldnn::feature(1),
            cldnn::spatial(convLayer->_dilation[X_AXIS], convLayer->_dilation[Y_AXIS]));
    }

    auto convPrim = cldnn::convolution(convLayerName,
                                       inputPrimitives[0],
                                       weightPrimID,
                                       biasPrimID,
                                       stride,
                                       padding,
                                       dilation,
                                       false,
                                       0.0f,
                                       CldnnTensorFromIEDims(convLayer->outData[0]->getTensorDesc().getDims()));

    if (convLayer->precision == Precision::I8 || convLayer->precision == Precision::U8) {
        // Output data type forcing is possible for u8/i8 -> fp32 only
        convPrim.output_data_type = DataTypeFromPrecision(convLayer->outData[0]->getTensorDesc().getPrecision());
    }

    if (convLayer->_group >= 16) {
        convPrim.groups = convLayer->_group;
    }

    std::vector<cldnn::primitive_id> wScalePrimID;
    CreateQuantizationPrimitives(topology, layer, wScalePrimID, true, weightPrimID.size());

    if (!wScalePrimID.empty()) {
        // TODO Fix in clDNN - there is no reason this should be immutable, most other fields are mutable
        auto& wq = const_cast<std::vector<cldnn::primitive_id>&>(convPrim.weights_quantization_factors.ref());
        wq.insert(wq.end(), wScalePrimID.begin(), wScalePrimID.end());
    }

    topology.add(convPrim);
    primitivesToIRLayersMap[convLayerName] = { layer->name };
    primitiveIDs[convLayerName] = convLayerName;
    profilingIDs.push_back(convLayerName);
}

void Program::CreateDeformableConvolutionPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 2);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto defConvLayer = as<InferenceEngine::DeformableConvolutionLayer*>(layer);

    std::vector<cldnn::primitive_id> weightPrimID;
    std::vector<cldnn::primitive_id> biasPrimID;
    CreateWeightAndBiasPrimitives(topology, layer, weightPrimID, biasPrimID);

    cldnn::tensor stride = cldnn::tensor(cldnn::batch(1), cldnn::feature(1),
                                         cldnn::spatial(defConvLayer->_stride[X_AXIS], defConvLayer->_stride[Y_AXIS], 1));
    auto allPad = getPaddings(*defConvLayer);
    cldnn::tensor padding = cldnn::tensor(cldnn::batch(0), cldnn::feature(0),
                                          cldnn::spatial(-allPad.begin[X_AXIS], -allPad.begin[Y_AXIS], 0));
    cldnn::tensor dilation = cldnn::tensor(cldnn::batch(1), cldnn::feature(1),
                                           cldnn::spatial(defConvLayer->_dilation[X_AXIS], defConvLayer->_dilation[Y_AXIS], 1));

    cldnn::tensor kernel = cldnn::tensor(cldnn::batch(1), cldnn::feature(1),
                                         cldnn::spatial(defConvLayer->_kernel[X_AXIS], defConvLayer->_kernel[Y_AXIS], 1));

    const uint32_t deformable_group = defConvLayer->GetParamAsUInt("deformable_group", 1);
    if (defConvLayer->_group > 1) {
        std::string defConvLayerName = layer_type_name_ID(layer);
        auto defConvPrim = cldnn::convolution(defConvLayerName,
                                              inputPrimitives[0],
                                              inputPrimitives[1],
                                              weightPrimID,
                                              biasPrimID,
                                              defConvLayer->_group,
                                              deformable_group,
                                              stride,
                                              padding,
                                              dilation,
                                              CldnnTensorFromIEDims(defConvLayer->outData[0]->getTensorDesc().getDims()));
        topology.add(defConvPrim);
        primitivesToIRLayersMap[defConvLayerName] = { layer->name };
        primitiveIDs[defConvLayerName] = defConvLayerName;
        profilingIDs.push_back(defConvLayerName);
    } else {
        std::string defConvLayerNameInterp = layer_type_name_ID(layer)+"_interp";
        std::string defConvLayerNameConv = layer_type_name_ID(layer);
        auto defConvPrimInterp = cldnn::deformable_interp(defConvLayerNameInterp,
                                                          inputPrimitives[0],
                                                          inputPrimitives[1],
                                                          defConvLayer->_group,
                                                          deformable_group,
                                                          stride,
                                                          padding,
                                                          dilation,
                                                          CldnnTensorFromIEDims(defConvLayer->outData[0]->getTensorDesc().getDims()),
                                                          kernel);
        topology.add(defConvPrimInterp);
        primitivesToIRLayersMap[defConvLayerNameInterp] = { layer->name };
        primitiveIDs[defConvLayerNameInterp] = defConvLayerNameInterp;
        profilingIDs.push_back(defConvLayerNameInterp);
        auto defConvPrim = cldnn::deformable_conv(defConvLayerNameConv,
                                                  defConvLayerNameInterp,
                                                  weightPrimID,
                                                  biasPrimID,
                                                  defConvLayer->_group,
                                                  CldnnTensorFromIEDims(defConvLayer->outData[0]->getTensorDesc().getDims()));
        topology.add(defConvPrim);
        primitivesToIRLayersMap[defConvLayerNameConv] = { layer->name };
        primitiveIDs[defConvLayerNameConv] = defConvLayerNameConv;
        profilingIDs.push_back(defConvLayerNameConv);
    }
}

void Program::CreateBinaryConvolutionPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto binaryConvLayer = as<InferenceEngine::BinaryConvolutionLayer*>(layer);

    if (binaryConvLayer->_group != 1)
        THROW_CLDNN_EXCEPTION("BinaryConvolution with groups is not supported yet");

    std::vector<cldnn::primitive_id> weightPrimID;
    std::vector<cldnn::primitive_id> biasPrimID;
    CreateBinaryWeightAndBiasPrimitives(topology, layer, weightPrimID, biasPrimID);
    cldnn::tensor stride = cldnn::tensor(cldnn::batch(1), cldnn::feature(1),
                                         cldnn::spatial(binaryConvLayer->_stride[X_AXIS], binaryConvLayer->_stride[Y_AXIS]));
    auto allPad = getPaddings(*binaryConvLayer);
    int x_pad = allPad.begin[X_AXIS], y_pad = allPad.begin[Y_AXIS];
    cldnn::tensor padding = cldnn::tensor(cldnn::batch(0), cldnn::feature(0),
                                          cldnn::spatial(-x_pad, -y_pad));
    cldnn::tensor dilation = cldnn::tensor(cldnn::batch(1), cldnn::feature(1),
                                           cldnn::spatial(binaryConvLayer->_dilation[X_AXIS], binaryConvLayer->_dilation[Y_AXIS]));

    cldnn::data_types calc_precision = DataTypeFromPrecision(binaryConvLayer->precision);
    std::string binaryConvLayerName = layer_type_name_ID(layer);
    auto binaryConvPrim = cldnn::binary_convolution(binaryConvLayerName,
                                                    inputPrimitives[0],
                                                    weightPrimID,
                                                    stride,
                                                    padding,
                                                    dilation,
                                                    CldnnTensorFromIEDims(binaryConvLayer->outData[0]->getTensorDesc().getDims()),
                                                    binaryConvLayer->_group,
                                                    binaryConvLayer->_pad_value,
                                                    calc_precision);

    primitivesToIRLayersMap[binaryConvLayerName] = { layer->name };
    primitiveIDs[binaryConvLayerName] = binaryConvLayerName;
    topology.add(binaryConvPrim);
    profilingIDs.push_back(binaryConvLayerName);
}

void Program::CreateQuantizePrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 5);

    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto quantizationLayer = as<InferenceEngine::QuantizeLayer*>(layer);

    auto input_low_id   = inputPrimitives[1];
    auto input_high_id  = inputPrimitives[2];
    auto output_low_id  = inputPrimitives[3];
    auto output_high_id = inputPrimitives[4];

    int levels = quantizationLayer->levels;
    std::string quantizeLayerName = layer_type_name_ID(layer);
    auto quantizationPrim = cldnn::quantize(quantizeLayerName,
                                            inputPrimitives[0],
                                            input_low_id,
                                            input_high_id,
                                            output_low_id,
                                            output_high_id,
                                            levels);

    primitivesToIRLayersMap[quantizeLayerName] = { layer->name };
    primitiveIDs[quantizeLayerName] = quantizeLayerName;
    topology.add(quantizationPrim);
    profilingIDs.push_back(quantizeLayerName);
}

void Program::CreateGatherPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 2);

    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto gatherLayer = as<InferenceEngine::GenericLayer*> (layer);

    int axis = gatherLayer->GetParamAsInt("axis", 0);

    // Be careful, TensorFlow consist negative axis interpretation bug. Here: -3 = b, -2 = f, -1 = y, but must be -3 = f, -2 = y, -1 = x
    auto cldnnAxisFromIE = [](int axis) {
        switch (axis) {
            case 0: return cldnn::gather::gather_axis::along_b;
            case 1: return cldnn::gather::gather_axis::along_f;
            case 2: return cldnn::gather::gather_axis::along_y;
            case 3: return cldnn::gather::gather_axis::along_x;
            case -1: return cldnn::gather::gather_axis::along_y;
            case -2: return cldnn::gather::gather_axis::along_f;
            case -3: return cldnn::gather::gather_axis::along_b;
            default: THROW_CLDNN_EXCEPTION("Unsupported gather axis: " << axis);
        }
    };

    std::string gatherLayerName = layer_type_name_ID(layer);
    auto gatherPrim = cldnn::gather(
            gatherLayerName,
            inputPrimitives[0],
            inputPrimitives[1],
            cldnnAxisFromIE(axis),
            CldnnTensorFromIEDims(gatherLayer->outData[0]->getTensorDesc().getDims()));

    primitivesToIRLayersMap[gatherLayerName] = { layer->name };
    primitiveIDs[gatherLayerName] = gatherLayerName;
    topology.add(gatherPrim);
    profilingIDs.push_back(gatherLayerName);
}

void Program::CreateDepthToSpacePrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);

    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto depthToSpace = as<InferenceEngine::GenericLayer*> (layer);

    size_t blockSize = depthToSpace->GetParamAsInt("block_size", 2);

    auto inputDim = depthToSpace->input().get()->getTensorDesc().getDims();
    if (inputDim.size() != 4)
        THROW_CLDNN_EXCEPTION("Unsupported size of tensor " << inputDim.size());

    size_t blockSizeSquare = blockSize * blockSize;

    if (inputDim[1] % blockSizeSquare != 0)
        THROW_CLDNN_EXCEPTION("The depth of the input tensor must be divisible by squared block size = " << blockSizeSquare);

    std::string depthToSpaceName = layer_type_name_ID(layer);
    auto depthToSpacePrim = cldnn::depth_to_space(
            depthToSpaceName,
            inputPrimitives[0],
            blockSize);

    primitivesToIRLayersMap[depthToSpaceName] = { layer->name };
    primitiveIDs[depthToSpaceName] = depthToSpaceName;
    topology.add(depthToSpacePrim);
    profilingIDs.push_back(depthToSpaceName);
}

void Program::CreateShuffleChannelsPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);

    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto shuffleChannels = as<InferenceEngine::GenericLayer*> (layer);
    const int32_t numberOfDims = shuffleChannels->input()->getDims().size();

    int32_t group = shuffleChannels->GetParamAsInt("group", 1);
    int32_t axis = shuffleChannels->GetParamAsInt("axis", 1);

    if (axis < 0)
        axis += numberOfDims;

    if (axis < 0 || axis >= numberOfDims)
        THROW_CLDNN_EXCEPTION("Incorrect axis value! Actual axis is" + std::to_string(group));

    if (group < 1)
        THROW_CLDNN_EXCEPTION("Invalid group size value (should equal at least one). Actual block size is" +
                                       std::to_string(group));

    if (shuffleChannels->input().get()->getDims()[axis] % group != 0)
        THROW_CLDNN_EXCEPTION("Group parameter must evenly divide the channel dimension. Actual group size is " +
                                       std::to_string(axis));

    std::string shuffleChannelsName = layer_type_name_ID(layer);
    auto shuffleChannelsPrim = cldnn::shuffle_channels(
            shuffleChannelsName,
            inputPrimitives[0],
            group,
            axis);

    primitivesToIRLayersMap[shuffleChannelsName] = { layer->name };
    primitiveIDs[shuffleChannelsName] = shuffleChannelsName;
    topology.add(shuffleChannelsPrim);
    profilingIDs.push_back(shuffleChannelsName);
}

void Program::CreateStridedSlicePrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto stridedSliceLayer = as<InferenceEngine::GenericLayer*> (layer);

    auto tmp = stridedSliceLayer->GetParamAsUInts("end_mask");
    std::vector<uint8_t> end_mask(tmp.begin(), tmp.end());
    tmp = stridedSliceLayer->GetParamAsUInts("begin_mask");
    std::vector<uint8_t> begin_mask(tmp.begin(), tmp.end());
    tmp = stridedSliceLayer->GetParamAsUInts("new_axis_mask");
    std::vector<uint8_t> new_axis_mask(tmp.begin(), tmp.end());
    tmp = stridedSliceLayer->GetParamAsUInts("shrink_axis_mask");
    std::vector<uint8_t> shrink_axis_mask(tmp.begin(), tmp.end());

    std::string stridedSliceLayerName = layer_type_name_ID(layer);
    auto stridedSlicePrim = cldnn::strided_slice(
            stridedSliceLayerName,
            inputPrimitives[0], inputPrimitives[1], inputPrimitives[2], inputPrimitives[3],
            begin_mask, end_mask, new_axis_mask, shrink_axis_mask);

    primitivesToIRLayersMap[stridedSliceLayerName] = { layer->name };
    primitiveIDs[stridedSliceLayerName] = stridedSliceLayerName;
    topology.add(stridedSlicePrim);
    profilingIDs.push_back(stridedSliceLayerName);
}

void Program::CreateReverseSequencePrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 2);

    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto reverseSequence = as<InferenceEngine::GenericLayer*> (layer);

    const auto input = reverseSequence->insData[0].lock()->getDims();
    const auto sequence_lengths = reverseSequence->insData[1].lock()->getDims();

    int32_t batch_axis = reverseSequence->GetParamAsInt("batch_axis", 0);
    int32_t seq_axis = reverseSequence->GetParamAsInt("seq_axis", 1);

    if (batch_axis < 0)
        batch_axis += input.size();

    if (seq_axis < 0)
        seq_axis += input.size();

    if (batch_axis == seq_axis)
        THROW_CLDNN_EXCEPTION("Batch axis and sequence axis should not be equal\n");

    if (seq_axis < 0 || seq_axis >= input.size())
        THROW_CLDNN_EXCEPTION("Incorrect Sequence axis value! Actual axis is " + std::to_string(seq_axis));

    if (batch_axis < 0 || batch_axis >= input.size())
        THROW_CLDNN_EXCEPTION("Incorrect Sequence axis value! Actual axis is " + std::to_string(batch_axis));

    if (sequence_lengths[0] != input[batch_axis])
        THROW_CLDNN_EXCEPTION("Sequence lengths must be a vector of length " + std::to_string(input[batch_axis])
                              + "! Actual axis is " + std::to_string(sequence_lengths[0]));

    std::string reverseSequenceLayerName = layer_type_name_ID(layer);
    auto reverseSequencePrim = cldnn::reverse_sequence(
            reverseSequenceLayerName,
            inputPrimitives[0],
            inputPrimitives[1],
            seq_axis,
            batch_axis);

    primitivesToIRLayersMap[reverseSequenceLayerName] = { layer->name };
    primitiveIDs[reverseSequenceLayerName] = reverseSequenceLayerName;
    topology.add(reverseSequencePrim);
    profilingIDs.push_back(reverseSequence->name);
}

void Program::CreateBroadcastPrimitive(cldnn::topology &topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 2);

    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto broadcast = as<InferenceEngine::GenericLayer*>(layer);

    std::string broadcastLayerName = layer_type_name_ID(layer);
    auto broadcastPrim = cldnn::broadcast(
            broadcastLayerName,
            inputPrimitives[0],
            CldnnTensorFromIEDims(broadcast->outData[0]->getTensorDesc().getDims()));

    primitiveIDs[broadcastLayerName] = broadcastLayerName;
    topology.add(broadcastPrim);
    profilingIDs.push_back(broadcast->name);
}

void Program::CreateGemmPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    bool threeInputs = layer->insData.size() == 3;

    if (threeInputs) {
        ValidateLayer(layer, 3);
    } else {
        ValidateLayer(layer, 2);
    }

    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto gemmLayer = as<InferenceEngine::GemmLayer*>(layer);
    auto gemmLayerName = layer_type_name_ID(layer);

    auto outDims = layer->outData[0]->getTensorDesc().getDims();
    auto outDimsN = outDims.size();

    auto gemmSpecificTensor = [](const InferenceEngine::SizeVector& dims) {
        switch (dims.size()) {
        case 2: return cldnn::tensor(cldnn::spatial(dims[1], dims[0]));
        case 3: return cldnn::tensor(cldnn::batch(dims[0]), cldnn::spatial(dims[2], dims[1]));
        case 4: return cldnn::tensor(cldnn::batch(dims[0]), cldnn::feature(dims[1]), cldnn::spatial(dims[3], dims[2]));
        case 5: return cldnn::tensor(cldnn::batch(dims[0]), cldnn::feature(dims[1]), cldnn::spatial(dims[4], dims[3], dims[2]));
        case 6: return cldnn::tensor(cldnn::batch(dims[0]), cldnn::feature(dims[1]), cldnn::spatial(dims[5], dims[4], dims[3], dims[2]));
        default: THROW_CLDNN_EXCEPTION("Invalid dimensions size(" << dims.size() << ") for Gemm layer");
        }
    };

    // Preprocess inputs
    for (size_t i = 0; i < inputPrimitives.size(); ++i) {
        auto inputDims = layer->insData[i].lock()->getTensorDesc().getDims();
        auto inputDimsN = inputDims.size();

        // Add reorder if changing number of dimensions requires changing format
        auto targetFormat = defaultFormatForDims(outDimsN);

        if (targetFormat.value != defaultFormatForDims(inputDimsN).value) {
            auto reorderName = gemmLayerName + "_cldnn_in" + std::to_string(i) + "_reorder";
            auto targetDatatype = DataTypeFromPrecision(layer->precision);
            auto reorderPrim = cldnn::reorder(reorderName, inputPrimitives[i], targetFormat, targetDatatype);

            topology.add(reorderPrim);
            primitivesToIRLayersMap[reorderName] = { layer->name };
            profilingIDs.push_back(reorderName);
            primitiveIDs[reorderName] = reorderName;

            inputPrimitives[i] = reorderName;
        }

        // Reshape input if they differ or gemm specific shape matches default one
        if (inputDimsN != outDimsN || inputDimsN < 4) {
            auto reshapeName = gemmLayerName + "_cldnn_in" + std::to_string(i) + "_reshape";

            // Extend input dimensions by prepending ones
            inputDims.insert(inputDims.begin(), outDimsN - inputDimsN, 1ul);

            auto targetShape = gemmSpecificTensor(inputDims);

            auto reshapePrim = cldnn::reshape(reshapeName, inputPrimitives[i], targetShape);

            topology.add(reshapePrim);
            primitivesToIRLayersMap[reshapeName] = { layer->name };
            profilingIDs.push_back(reshapeName);
            primitiveIDs[reshapeName] = reshapeName;

            inputPrimitives[i] = reshapeName;
        }
    }

    // Add actual gemm
    auto alpha = gemmLayer->alpha;
    auto beta = gemmLayer->beta;
    auto transA = gemmLayer->transpose_a;
    auto transB = gemmLayer->transpose_b;

    auto gemmPrim = cldnn::gemm(
        gemmLayerName,
        inputPrimitives,
        transA,
        transB,
        alpha,
        beta);

    topology.add(gemmPrim);
    primitivesToIRLayersMap[gemmLayerName] = { layer->name };
    profilingIDs.push_back(gemmLayerName);

    auto lastLayerName = gemmLayerName;

    // Reshape output if gemm specific shape does not match default one
    if (outDimsN < 4) {
        auto outputShape = CldnnTensorFromIEDims(outDims);
        auto outReshapeName = gemmLayerName + "_cldnn_out_reshape";
        auto outReshapePrim = cldnn::reshape(outReshapeName, gemmLayerName, outputShape);

        topology.add(outReshapePrim);
        primitivesToIRLayersMap[outReshapeName] = { layer->name };
        profilingIDs.push_back(outReshapeName);
        primitiveIDs[outReshapeName] = outReshapeName;

        lastLayerName = outReshapeName;
    }

    primitiveIDs[gemmLayerName] = lastLayerName;
}


void Program::CreateReducePrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 2);

    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto reduce = as<InferenceEngine::ReduceLayer*>(layer);
    auto input = reduce->insData[0].lock();
    size_t reduceDimNumber = input->getTensorDesc().getDims().size();

    auto axesInput = layer->insData[1].lock();
    auto axesInputCreator = axesInput->getCreatorLayer().lock();

    std::vector<int32_t> rawAxes;
    if (axesInputCreator->blobs.size() == 1) {
        auto constantBlob = axesInputCreator->blobs.begin()->second;
        auto axesPrecision = constantBlob->getTensorDesc().getPrecision();
        if (axesPrecision == InferenceEngine::Precision::FP32) {
            auto data = constantBlob->buffer().as<float*>();
            for (size_t i = 0; i < constantBlob->size(); ++i)
                rawAxes.push_back(data[i]);
        } else if (axesPrecision == InferenceEngine::Precision::I32) {
            auto data = constantBlob->buffer().as<int32_t*>();
            for (size_t i = 0; i < constantBlob->size(); ++i)
                rawAxes.push_back(data[i]);
        } else {
            THROW_IE_EXCEPTION << layer->name << " Incorrect Reduce axes input Precision";
        }
    }

    std::vector<uint16_t> axes;
    for (size_t a = 0; a < rawAxes.size(); a++) {
        if (rawAxes[a] < 0)
            rawAxes[a] = rawAxes[a] + reduceDimNumber;
        if (rawAxes[a] < 0 || rawAxes[a] > reduceDimNumber - 1)
            THROW_IE_EXCEPTION << layer->name << " Incorrect Reduce axis value: " << rawAxes[a];
        if (reduceDimNumber == 6) {
            switch (rawAxes[a]) {
                case 0: axes.push_back(cldnn::reduce::along_b); break;
                case 1: axes.push_back(cldnn::reduce::along_f); break;
                case 2: axes.push_back(cldnn::reduce::along_w); break;
                case 3: axes.push_back(cldnn::reduce::along_z); break;
                case 4: axes.push_back(cldnn::reduce::along_y); break;
                case 5: axes.push_back(cldnn::reduce::along_x); break;
            }
        } else if (reduceDimNumber == 5) {
            switch (rawAxes[a]) {
                case 0: axes.push_back(cldnn::reduce::along_b); break;
                case 1: axes.push_back(cldnn::reduce::along_f); break;
                case 2: axes.push_back(cldnn::reduce::along_z); break;
                case 3: axes.push_back(cldnn::reduce::along_y); break;
                case 4: axes.push_back(cldnn::reduce::along_x); break;
            }
        } else {
            switch (rawAxes[a]) {
                case 0: axes.push_back(cldnn::reduce::along_b); break;
                case 1: axes.push_back(cldnn::reduce::along_f); break;
                case 2: axes.push_back(cldnn::reduce::along_y); break;
                case 3: axes.push_back(cldnn::reduce::along_x); break;
            }
        }
    }

    sort(axes.begin(), axes.end());
    axes.erase(unique(axes.begin(), axes.end()), axes.end());

    cldnn::reduce_mode mode;
    std::string reduceType = layer->type;
    if (reduceType == "ReduceMax") mode = cldnn::reduce_mode::max;
    else if (reduceType == "ReduceMin") mode = cldnn::reduce_mode::min;
    else if (reduceType == "ReduceMean") mode = cldnn::reduce_mode::mean;
    else if (reduceType == "ReduceProd") mode = cldnn::reduce_mode::prod;
    else if (reduceType == "ReduceSum") mode = cldnn::reduce_mode::sum;
    else if (reduceType == "ReduceAnd") mode = cldnn::reduce_mode::logical_and;
    else if (reduceType == "ReduceOr") mode = cldnn::reduce_mode::logical_or;
    else if (reduceType == "ReduceSumSquare") mode = cldnn::reduce_mode::sum_square;
    else if (reduceType == "ReduceL1") mode = cldnn::reduce_mode::l1;
    else if (reduceType == "ReduceL2") mode = cldnn::reduce_mode::l2;
    else if (reduceType == "ReduceLogSum") mode = cldnn::reduce_mode::log_sum;
    else if (reduceType == "ReduceLogSumExp") mode = cldnn::reduce_mode::log_sum_exp;
    else
        THROW_IE_EXCEPTION << layer->name << " Incorrect Reduce layer type!";

    std::string reduceLayerName = layer_type_name_ID(layer);
    auto reducePrim = cldnn::reduce(
            reduceLayerName,
            inputPrimitives[0],
            mode,
            axes,
            static_cast<int32_t>(reduce->keep_dims));

    primitiveIDs[reduceLayerName] = reduceLayerName;
    topology.add(reducePrim);
    profilingIDs.push_back(reduce->name);
}

void Program::CreateOneHotPrimitive(cldnn::topology& topology, InferenceEngine::CNNLayerPtr &layer) {
    ValidateLayer(layer, 1);

    auto inputPrimitives = GetPrevLayersPrimitives(layer);
    auto oneHot = as<InferenceEngine::GenericLayer*>(layer);

    int16_t axis = oneHot->GetParamAsInt("axis", -1);
    float on_value  = layer->GetParamAsFloat("on_value", 1.0f);
    float off_value = layer->GetParamAsFloat("off_value", 0.0f);
    auto dims = oneHot->input()->getDims();

    if (axis < -1 || axis > static_cast<int16_t>(dims.size()))
        THROW_IE_EXCEPTION << layer->name << " Incorrect OneHot axis value: " << axis << ". Should be between -1 and " << dims.size();

    if (axis == -1) {
        axis = dims.size();
        for (int i = dims.size() - 1; i >= 0; i--) {
            if (dims[i] == 1)
                axis--;
            else
                break;
        }
    }

    std::string oneHotLayerName = layer_type_name_ID(layer);
    auto oneHotPrim = cldnn::one_hot(
            oneHotLayerName,
            inputPrimitives[0],
            CldnnTensorFromIEDims(oneHot->outData[0]->getDims()),
            static_cast<uint16_t>(axis),
            on_value,
            off_value);

    primitiveIDs[oneHotLayerName] = oneHotLayerName;
    topology.add(oneHotPrim);
    profilingIDs.push_back(oneHot->name);
}

bool Program::IsValidSplitConvMerge(const InferenceEngine::SplitLayer *splitLayer) const {
    if (splitLayer->outData.size() != 2) return false;  // split into 2

    for (auto out : splitLayer->outData) {
        if (out->getInputTo().size() != 1) {
            return false;
        }
    }

    auto convLayer1 =
        tryAs<InferenceEngine::ConvolutionLayer *> (GetNextSingleLayer(splitLayer->outData[0]));
    auto convLayer2 =
        tryAs<InferenceEngine::ConvolutionLayer *> (GetNextSingleLayer(splitLayer->outData[1]));
    if (!convLayer1 || !convLayer2) {   // outputs aren't convolutions
        return false;
    }
    auto allPad1 = getPaddings(*convLayer1);
    auto allPad2 = getPaddings(*convLayer2);
    if (convLayer1->precision != convLayer2->precision                       // wrong precision
        || convLayer1->_fusedWith || convLayer2->_fusedWith                     // convolutions are fused
        || convLayer1->outData.size() != 1 || convLayer2->outData.size() != 1   // more than 1 output for convolutions
        || allPad1.begin[X_AXIS] != allPad2.begin[X_AXIS]                     // different padding
        || allPad1.begin[Y_AXIS] != allPad2.begin[Y_AXIS]                     // different padding
        || convLayer1->_stride[X_AXIS] != convLayer2->_stride[X_AXIS]                       // different strides
        || convLayer1->_stride[Y_AXIS] != convLayer2->_stride[Y_AXIS]                       // different strides
        || convLayer1->_dilation[X_AXIS] != convLayer2->_dilation[X_AXIS]                   // different dilation
        || convLayer1->_dilation[Y_AXIS] != convLayer2->_dilation[Y_AXIS]                   // different dilation
        || (GetNextSingleLayer(GetNextSingleLayer(splitLayer->outData[0]))      // no merge after convolutions
            != GetNextSingleLayer(GetNextSingleLayer(splitLayer->outData[1])))
        || (p_currentOutputs.find(convLayer1->name) != p_currentOutputs.end())
        || (p_currentOutputs.find(convLayer2->name) != p_currentOutputs.end())) {
        return false;
    }
    auto concatLayer =
        tryAs<InferenceEngine::ConcatLayer *> (
                GetNextSingleLayer(GetNextSingleLayer(splitLayer->outData[0])));
    if (!concatLayer ||                         // not a merge layer
        concatLayer->_axis != 1 ||              // merge on unsupported axis
        concatLayer->outData.size() != 1) {     // too many outputs
        return false;
    }
    if (m_config.customLayers.find(convLayer1->type) != m_config.customLayers.end() ||
        m_config.customLayers.find(concatLayer->type) != m_config.customLayers.end()) {
        return false;  // convolution or concat were overwritten by a custom layer
    }

    return true;
}

void Program::AddInputPrimitive(cldnn::topology& topology, InferenceEngine::InputInfo::Ptr inputInfo, Precision inputPrecision, const std::string inputName) {
    // first create and add the input layout
    const auto inputDesc = inputInfo->getTensorDesc();
    const auto inputDims = inputDesc.getDims();
    InferenceEngine::Layout l = inputDesc.getLayout();
    const std::string& layoutName = DebugOptions::IELayoutToString(l);
    auto consumers = inputInfo->getInputData()->getInputTo();

    cldnn::format inputFormat = m_defaultFormat;
    if (InferenceEngine::Layout::BLOCKED == l && 6 == inputDims.size())
        inputFormat = cldnn::format::bfwzyx;
    else
        inputFormat = FormatFromLayout(l);
    cldnn::tensor dataTensor;
    cldnn::tensor::value_type batch = (m_max_batch <= 1)
                                      ? (inputDims.size() > 3 ? TensorValue(inputDims[0]) : 1)
                                      : TensorValue(m_curBatch);
    switch (inputDims.size()) {
    case 6:
        dataTensor = cldnn::tensor(cldnn::batch(batch),
                                    cldnn::feature(inputDims[1]),
                                    cldnn::spatial(inputDims[5], inputDims[4], inputDims[3], inputDims[2]));
        break;
    case 5:
        if (InferenceEngine::Layout::NCDHW == l) {
            dataTensor = cldnn::tensor(cldnn::batch(batch),
                cldnn::feature(inputDims[1]),
                cldnn::spatial(inputDims[4], inputDims[3], inputDims[2]));
        } else {
            THROW_CLDNN_EXCEPTION("Unsupported layout (" << layoutName << ") in 5D input " + inputInfo->name());
        }
        break;
    case 4:
        if (InferenceEngine::Layout::NCHW == l || InferenceEngine::Layout::CHW == l) {
            dataTensor = cldnn::tensor(batch,
                TensorValue(inputDims[1]), TensorValue(inputDims[3]), TensorValue(inputDims[2]));
        } else if (InferenceEngine::Layout::NHWC == l) {
            dataTensor = cldnn::tensor(batch,
                TensorValue(inputDims[1]), TensorValue(inputDims[3]), TensorValue(inputDims[2]));
        } else {
            THROW_CLDNN_EXCEPTION("Unsupported layout (" << layoutName << ") in 4D input " + inputInfo->name());
        }
        break;
    case 3:
        if (InferenceEngine::Layout::CHW == l) {
            dataTensor = cldnn::tensor(TensorValue(inputDims[0]), TensorValue(inputDims[1]), 1, TensorValue(inputDims[2]));
        } else {
            THROW_CLDNN_EXCEPTION("Unsupported layout (" << layoutName << ") in 3D input " + inputInfo->name());
        }
        break;
    case 2:
        if (InferenceEngine::Layout::NCHW == l || InferenceEngine::NC == l) {
            dataTensor = cldnn::tensor(TensorValue(inputDims[0]), TensorValue(inputDims[1]), 1, 1);
        } else {
            THROW_CLDNN_EXCEPTION("Unsupported layout (" << layoutName << ") in 2D input " + inputInfo->name());
        }
        break;
    case 1:
        dataTensor = cldnn::tensor(TensorValue(inputDims[0]), 1, 1, 1);
        break;
    default: THROW_CLDNN_EXCEPTION("Invalid data dimensions");
    }
    cldnn::layout inputLayout(DataTypeFromPrecision(inputDesc.getPrecision()),
                              inputFormat,
                              dataTensor);

    // save the input dims
    inputLayouts.insert({ inputInfo->name(), inputLayout });

    topology.add(cldnn::input_layout(inputName, inputLayout));
    primitivesToIRLayersMap[inputName] = { inputInfo->name() };

    // create preprocess primitive for this input
    auto preProcess = inputInfo->getPreProcess();

    size_t meanChannels = preProcess.getNumberOfChannels();
    inputLayout.format = inputFormat;
    inputLayout.size = inputLayout.size.transform(inputFormat, 1);
    inputLayout.data_type = DataTypeFromPrecision(inputPrecision);
    auto preprocessPrimID = inputName + m_preProcessTag;

    if ((meanChannels > 0) &&
        (meanChannels != inputLayout.size.feature[0])) {
        THROW_CLDNN_EXCEPTION("Mismatched mean values channels in input " + inputName);
    }

    switch (preProcess.getMeanVariant()) {
        case NONE:
        case MEAN_VALUE: {
            std::vector<float> meanValues;
            if (meanChannels > 0) {
                for (size_t c = 0; c < meanChannels; c++) {
                    if (fabs(preProcess[c]->stdScale - 1.0f) > 1e-10)
                        THROW_CLDNN_EXCEPTION("not supporting stdScale yet in input " + inputName);
                    meanValues.push_back(preProcess[c]->meanValue);
                }
            }
            topology.add(cldnn::reorder(preprocessPrimID, inputName, inputLayout, meanValues));
            primitivesToIRLayersMap[preprocessPrimID] = { inputInfo->name() };
            profilingIDs.push_back(preprocessPrimID);
            InitProfileInfo(preprocessPrimID, "Reorder");
        }
            break;

        case MEAN_IMAGE: {
            IE_ASSERT(meanChannels);
            // first merge all mean values to a single blob
            // todo make sure mean blob precision is the same as the input precision
            auto meanDims = inputDims;
            // overwrite batches with 1
            switch (meanDims.size()) {
                case 4: meanDims[0] = 1;
                    break;
                default:
                    THROW_CLDNN_EXCEPTION("Missing batch dimensions in input image");
            }
            const TensorDesc desc(Precision(Precision::FP32), meanDims, TensorDesc::getLayoutByDims(meanDims));
            InferenceEngine::TBlob<float> meanBlob(desc);
            meanBlob.allocate();
            auto meanBlobData = meanBlob.data();
            for (size_t c = 0; c < meanChannels; c++) {
                if (fabs(preProcess[c]->stdScale - 1.0f) > 1e-10)
                    THROW_CLDNN_EXCEPTION("not supporting stdScale yet in input " + inputName);
                auto channelMeanBlob = std::dynamic_pointer_cast<TBlob<float>>(preProcess[c]->meanData);
                auto channelSize = channelMeanBlob->size();
                auto channelBlobData = channelMeanBlob->data();
                for (size_t i = 0; i < channelSize; i++) {
                    meanBlobData[(c * channelSize) + i] = channelBlobData[i];
                }
            }
            // then create a data primitive for the mean values
            auto meanBlobPtr = std::make_shared<InferenceEngine::TBlob<float>>(meanBlob);

            // mean values will use external format (sub in the input format before convert to new format)
            cldnn::tensor meanBlobTensor(inputLayout.size);
            meanBlobTensor.batch[0] = 1;  // mean values have no batches
            cldnn::layout meanBlobLayout(cldnn::data_types::f32, m_defaultFormat, meanBlobTensor);
            cldnn::primitive_id meanBlobID = inputName + m_meanValuesTag;
            meanBlobID = CreatePrimitiveFromBlob(topology,
                                                 meanBlobID,
                                                 meanBlobPtr,
                                                 meanBlobLayout);
            topology.add(cldnn::reorder(preprocessPrimID,
                                        inputName,
                                        inputLayout,
                                        meanBlobID));
            primitivesToIRLayersMap[preprocessPrimID] = { inputInfo->name() };
            profilingIDs.push_back(preprocessPrimID);
            InitProfileInfo(preprocessPrimID, "Reorder");
            break;
        }
        default: THROW_CLDNN_EXCEPTION("Invalid mean variant in input " + inputName);
            break;
    }
    primitiveIDs[inputName] = preprocessPrimID;
    primitiveIDs[preprocessPrimID] = preprocessPrimID;
}

std::vector<cldnn::primitive_id> Program::GetPrevLayersPrimitives(const InferenceEngine::CNNLayerPtr layer) const {
    if (layer == nullptr) {
        return {};
    }
    std::vector<cldnn::primitive_id> inputPrimitives;
    for (auto inputData : layer->insData) {
        auto prevData = inputData.lock();
        if (prevData == nullptr) {
            THROW_CLDNN_EXCEPTION("Nonexistent input for layer: " << layer->name);
        }
        auto prevCreator = prevData->getCreatorLayer().lock();
        std::string prevName;

        if (prevCreator) {
            prevName = layer_type_lower(prevCreator) + ":";
            if (prevCreator->outData.size() > 1)
                prevName += prevData->getName();
            else
                prevName += prevCreator->name;
        } else {
            prevName = prevData->getName();
        }
        inputPrimitives.push_back(primitiveIDs.at(prevName));
    }
    return inputPrimitives;
}

void Program::AddOutputPrimitive(cldnn::topology& topology, std::string outputName, const InferenceEngine::DataPtr outputData, Precision outputPrecision) {
    const auto outputDesc = outputData->getTensorDesc();
    const auto outputlayout = outputDesc.getLayout();

    // TODO: add precision check once there's an outputInfo object
    if (outputlayout != InferenceEngine::NCHW &&
        // TODO: change 6d case once new layout added in IE
        outputlayout != InferenceEngine::BLOCKED &&
        outputlayout != InferenceEngine::NCDHW &&
        outputlayout != InferenceEngine::NHWC &&
        outputlayout != InferenceEngine::CHW &&
        outputlayout != InferenceEngine::NC &&
        outputlayout != InferenceEngine::C) {
        THROW_CLDNN_EXCEPTION("Unsupported layout (" << DebugOptions::IELayoutToString(outputlayout) << ") in output: " << outputName);
    }

    auto outputCreator = outputData->getCreatorLayer().lock();
    std::string outLayerName = layer_type_lower(outputCreator) + ":";

    if (outputCreator->outData.size() > 1)
        outLayerName += outputName;
    else
        outLayerName += outputCreator->name;

    auto outputReorderID = outputName + m_postProcessTag;
    Precision precision = outputPrecision == Precision::UNSPECIFIED ? outputData->getPrecision() : outputPrecision;

    // Find correct output ID. Start with name stored in IR.
    std::string outputID = outLayerName;
    std::string finalID = primitiveIDs.at(outLayerName);

    while (outputID != finalID) {
        auto prim = primitiveIDs.find(finalID);

        if (prim == primitiveIDs.end()) {
            THROW_IE_EXCEPTION << "Unknown output primitive id " << outputID;
        }
        outputID = finalID;
        finalID = prim->second;
    }

    topology.add(cldnn::reorder(outputReorderID, outputID,
        FormatFromLayout(outputData->getLayout()),
        DataTypeFromPrecision(precision)));
    primitiveIDs[outputName] = outputReorderID;
    profilingIDs.push_back(outputReorderID);
    InitProfileInfo(outputReorderID, "Reorder");

    outputDims[outputName] = outputDesc.getDims();
    prevPrimitiveIDs[outputReorderID] = {outputName};
}

void Program::AddSingleValuePrimitive(cldnn::topology& topology, cldnn::primitive_id valPrimID, cldnn::data_types dataType, float value) {
    cldnn::layout primLayout(dataType, m_defaultFormat, { 1, 1, 1, 1 });
    auto primMem = cldnn::memory::allocate(*m_engine, primLayout);
    switch (dataType) {
    case cldnn::data_types::f32:
    {
        auto tmpPointer = primMem.pointer<float>();  // implicitly maps buffer - unmap in destructor
        tmpPointer[0] = value;
    }
        break;
    case cldnn::data_types::f16:
    {
        auto tmpPointer = primMem.pointer<uint16_t>();  // implicitly maps buffer - unmap in destructor
        cldnn_status status = CLDNN_SUCCESS;
        tmpPointer[0] = cldnn_float_to_half(value, &status);
        if (status != CLDNN_SUCCESS) {
            THROW_CLDNN_EXCEPTION("Error converting value to fp16.");
        }
    }
        break;
    default:
        THROW_CLDNN_EXCEPTION("Unhandled data type (precision)");
    }

    topology.add(cldnn::data(valPrimID, primMem));
}

cldnn::data_types Program::DataTypeFromPrecision(InferenceEngine::Precision p) {
    switch (p) {
    case Precision::I16:
    case Precision::FP32:
        return cldnn::data_types::f32;
    case Precision::FP16:
        return cldnn::data_types::f16;
    case Precision::U8:
        return cldnn::data_types::u8;
    case Precision::I8:
        return cldnn::data_types::i8;
    case Precision::I32:
        return cldnn::data_types::i32;
    case Precision::BIN:
        return cldnn::data_types::bin;
    default:
        THROW_IE_EXCEPTION << PARAMETER_MISMATCH_str << "The plugin does not support " << p.name() << " precision";
        break;
    }
}

cldnn::format Program::FormatFromLayout(InferenceEngine::Layout l) {
    switch (l) {
    // TODO: change 6d case once new layout added in IE
    case InferenceEngine::Layout::BLOCKED:
        return cldnn::format::bfwzyx;
    case InferenceEngine::Layout::NCDHW:
        return cldnn::format::bfzyx;
    case InferenceEngine::Layout::NCHW:
    case InferenceEngine::Layout::NC:
    case InferenceEngine::Layout::CHW:
    case InferenceEngine::Layout::C:
        return cldnn::format::bfyx;
    case InferenceEngine::Layout::NHWC:
        return cldnn::format::byxf;
    default:
        THROW_IE_EXCEPTION << PARAMETER_MISMATCH_str << "The plugin does not support " << l << " layout";
        break;
    }
}

cldnn::upsampling_sample_type Program::UpsamplingTypeFromString(const std::string& str) {
    static const caseless_map<std::string, cldnn::upsampling_sample_type> UpsamplingTypeNameToType = {
        { "Bilinear" , cldnn::upsampling_sample_type::bilinear },
        { "Nearest" , cldnn::upsampling_sample_type::nearest },
    };
    auto it = UpsamplingTypeNameToType.find(str);
    if (it != UpsamplingTypeNameToType.end())
        return it->second;
    else
        THROW_CLDNN_EXCEPTION("Unknown Upsampling type: " << str);
}

cldnn::softmax::dimension_t Program::SoftmaxDimensionFromIEAxis(const InferenceEngine::SoftMaxLayer* softmaxLayer) {
    auto sz = softmaxLayer->input()->getTensorDesc().getDims().size();
    switch (softmaxLayer->axis) {
    case 0: return cldnn::softmax::normalize_all;
    case 1: return cldnn::softmax::normalize_f;
    case 2:
        if (sz > 4)
            return cldnn::softmax::normalize_z;
        else
            return cldnn::softmax::normalize_y;
    case 3:
        if (sz > 4)
            return cldnn::softmax::normalize_y;
        else
            return cldnn::softmax::normalize_x;
    case 4:
        return cldnn::softmax::normalize_x;
    default: THROW_CLDNN_EXCEPTION("Invalid softmax axis " << softmaxLayer->axis);
    }
    return cldnn::softmax::normalize_fyx;
}

cldnn::prior_box_code_type Program::PriorBoxCodeFromString(const std::string& str) {
    static const std::map<std::string, cldnn::prior_box_code_type> CodeNameToType = {
        { "caffe.PriorBoxParameter.CORNER" , cldnn::prior_box_code_type::corner },
        { "caffe.PriorBoxParameter.CENTER_SIZE" , cldnn::prior_box_code_type::center_size },
        { "caffe.PriorBoxParameter.CORNER_SIZE" , cldnn::prior_box_code_type::corner_size },
    };
    auto it = CodeNameToType.find(str);
    if (it != CodeNameToType.end()) {
        return it->second;
    } else {
        THROW_CLDNN_EXCEPTION("Unknown Prior-Box code type: " + str);
        return cldnn::prior_box_code_type::corner;
    }
}

Program::GenericBlobMap Program::CreateGenericLayerBlobPrimitives(cldnn::topology& topology, const InferenceEngine::GenericLayer* layer) {
    IE_ASSERT(layer);
    GenericBlobMap res;
    for (auto& blob : layer->blobs) {
        const auto blobDims = blob.second->getTensorDesc().getDims();
        if (blobDims.size() != 1) {
            THROW_CLDNN_EXCEPTION("Unhandled blob dim in layer " + layer->name);
        }

        cldnn::layout genericLayout(DataTypeFromPrecision(blob.second->getTensorDesc().getPrecision()),
                                    m_defaultFormat,
                                    (cldnn::tensor) cldnn::spatial(TensorValue(blobDims.back())));

        cldnn::primitive_id initialWeightID = layer_type_name_ID(layer) + "_" + blob.first + m_weightsTag;
        cldnn::primitive_id weightID = CreatePrimitiveFromBlob(topology, initialWeightID, blob.second, genericLayout);
        res[initialWeightID] = weightID;
    }

    return res;
}

void Program::ValidateGenericLayerBlobs(const InferenceEngine::GenericLayer* layer, const std::vector<std::string>& blobNames) {
    IE_ASSERT(layer);
    for (auto& name : blobNames) {
        if (layer->blobs.find(name) == layer->blobs.end()) {
            THROW_CLDNN_EXCEPTION("Missing blob " + name + " in layer " + layer->name);
        }
    }
}

void Program::InitProfileInfo(const std::string& layerName,
                              const std::string& layerType,
                              bool isCPU,
                              InferenceEngine::InferenceEngineProfileInfo::LayerStatus status) {
    perfMap[layerType + ":" + layerName].first = layerName;
    auto& perfEntry = perfMap[layerType + ":" + layerName].second;
    perfEntry.layerType = layerType;
    perfEntry.status = status;
    perfEntry.cpu_uSec = perfEntry.realTime_uSec = 0;
    perfEntry.isCPU = isCPU;
    perfEntry.status = status;
}

}  // namespace CLDNNPlugin
