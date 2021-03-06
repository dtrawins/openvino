// Copyright (C) 2018-2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <ie_icnn_network.hpp>
#include <ie_input_info.hpp>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "cpp_interfaces/exception2status.hpp"
#include "cpp_interfaces/plugin_itt.hpp"
#include "cpp_interfaces/interface/ie_iinfer_request_internal.hpp"
#include "cpp_interfaces/interface/ie_iplugin_internal.hpp"
#include "debug.h"
#include "ie_compound_blob.h"
#include "ie_memcpy.h"
#include "ie_preprocess_data.hpp"

namespace InferenceEngine {

class ExecutableNetworkInternal;

/**
 * @brief An optimal implementation of IInferRequestInternal interface to avoid duplication in all plugins
 * This base class is recommended to be used as a base class for plugin synchronous inference request implementation. 
 * @ingroup ie_dev_api_infer_request_api
 */
class InferRequestInternal : virtual public IInferRequestInternal {
public:
    /**
     * @brief A shared pointer to a InferRequestInternal implementation.
     */
    typedef std::shared_ptr<InferRequestInternal> Ptr;

    /**
     * @brief      Constructs a new instance.
     * @param[in]  networkInputs   The network inputs info
     * @param[in]  networkOutputs  The network outputs data
     */
    InferRequestInternal(const InputsDataMap& networkInputs, const OutputsDataMap& networkOutputs): m_curBatch(-1) {
        // // We should copy maps since they can be overriden in SetBlob with preprocess
        copyInputOutputInfo(networkInputs, networkOutputs, _networkInputs, _networkOutputs);
    }

    /**
     * @brief The minimal infer function to be implemented by plugins. It infers specified input(s) in synchronous mode
     * @note 
     *  * This method is used in InferRequestInternal::Infer, which calls the common code first and after uses this
     * plugin dependent implementation.
     *  * Blocks all method of IInferRequest while request is ongoing (running or waiting in queue)
     */
    virtual void InferImpl() = 0;

    /**
     * @brief Default common implementation for all plugins with checking input and output blobs before inference
     */
    void Infer() override {
        checkBlobs();
        InferImpl();
    }

    /**
     * @brief Given optional implementation of setting blob to avoid need for it to be implemented by plugin
     * @param name - a name of input or output blob.
     * @param data - a reference to input or output blob. The type of Blob must correspond to the network input
     * precision and size.
     */
    void SetBlob(const char* name, const Blob::Ptr& data) override {
        OV_ITT_SCOPED_TASK(itt::domains::Plugin, "SetBlob");
        if (name == nullptr) {
            THROW_IE_EXCEPTION << NOT_FOUND_str + "Failed to set blob with empty name";
        }
        if (!data) THROW_IE_EXCEPTION << NOT_ALLOCATED_str << "Failed to set empty blob with name: \'" << name << "\'";
        const bool compoundBlobPassed = data->is<CompoundBlob>();
        if (!compoundBlobPassed && data->buffer() == nullptr)
            THROW_IE_EXCEPTION << "Input data was not allocated. Input name: \'" << name << "\'";
        if (data->size() == 0) {
            THROW_IE_EXCEPTION << "Input data is empty. Input name: \'" << name << "\'";
        }

        InputInfo::Ptr foundInput;
        DataPtr foundOutput;
        size_t dataSize = data->size();
        if (findInputAndOutputBlobByName(name, foundInput, foundOutput)) {
            if (foundInput->getPrecision() != data->getTensorDesc().getPrecision()) {
                THROW_IE_EXCEPTION << PARAMETER_MISMATCH_str
                                   << "Failed to set Blob with precision not corresponding to user input precision";
            }

            const bool preProcRequired = preProcessingRequired(foundInput, data);
            if (compoundBlobPassed && !preProcRequired) {
                THROW_IE_EXCEPTION << NOT_IMPLEMENTED_str
                                   << "cannot set compound blob: supported only for input pre-processing";
            }

            if (preProcRequired) {
                if (_preProcData.find(name) == _preProcData.end()) {
                    _preProcData.emplace(name, CreatePreprocDataHelper());
                }
                _preProcData[name]->isApplicable(data, _inputs[name]);
                // Stores the given blob as ROI blob. It will be used to fill in network input
                // during pre-processing
                _preProcData[name]->setRoiBlob(data);
            } else {
                size_t inputSize = details::product(foundInput->getTensorDesc().getDims());
                if (dataSize != inputSize) {
                    THROW_IE_EXCEPTION << "Input blob size is not equal network input size (" << dataSize
                                       << "!=" << inputSize << ").";
                }
                _inputs[name] = data;
            }
        } else {
            if (compoundBlobPassed) {
                THROW_IE_EXCEPTION << NOT_IMPLEMENTED_str
                                   << "cannot set compound blob: supported only for input pre-processing";
            }
            size_t outputSize = details::product(foundOutput->getDims());
            if (dataSize != outputSize) {
                THROW_IE_EXCEPTION << "Output blob size is not equal network output size (" << dataSize
                                   << "!=" << outputSize << ").";
            }
            if (foundOutput->getPrecision() != data->getTensorDesc().getPrecision()) {
                THROW_IE_EXCEPTION << PARAMETER_MISMATCH_str
                                   << "Failed to set Blob with precision not corresponding to user output precision";
            }
            _outputs[name] = data;
        }
    }

    /**
     * @brief Given optional implementation of getting blob to avoid need for it to be implemented by plugin
     * @param name - a name of input or output blob.
     * @param data - a reference to input or output blob. The type of Blob must correspond to the network input
     * precision and size.
     * @note if ROI blob was previously set it is returned (without dimensions checks) instead of default blob.
     */
    void GetBlob(const char* name, Blob::Ptr& data) override {
        OV_ITT_SCOPED_TASK(itt::domains::Plugin, "GetBlob");
        InputInfo::Ptr foundInput;
        DataPtr foundOutput;
        const SizeVector oneVector = { 1 };
        if (findInputAndOutputBlobByName(name, foundInput, foundOutput)) {
            // ROI blob is returned only if it was set previously. Otherwise default blob is returned.
            auto it = _preProcData.find(name);
            if (it != _preProcData.end()) {
                data = it->second->getRoiBlob();
            } else {
                data = _inputs[name];
                checkBlob(data, name, true,
                    foundInput->getTensorDesc().getLayout() != SCALAR
                    ? foundInput->getTensorDesc().getDims()
                    : oneVector);
            }
        } else {
            data = _outputs[name];
            checkBlob(data, name, false,
                foundOutput->getTensorDesc().getLayout() != SCALAR
                ? foundOutput->getTensorDesc().getDims()
                : oneVector);
        }
    }

    /**
     * @brief Sets pre-process for input data
     * @param name Name of input blob.
     * @param data - a reference to input or output blob. The type of Blob must correspond to the network input precision and size.
     * @param info Preprocess info for blob.
     */
    void SetBlob(const char* name, const Blob::Ptr& data, const PreProcessInfo& info) override {
        InputInfo::Ptr foundInput;
        DataPtr foundOutput;
        if (findInputAndOutputBlobByName(name, foundInput, foundOutput)) {
            copyPreProcess(info, foundInput->getPreProcess());
        } else {
            THROW_IE_EXCEPTION << "Pre-process can't be set to output blob";
        }

        SetBlob(name, data);
    }

    /**
     * @brief Gets pre-process for input data
     * @param name Name of input blob.
     * @param info pointer to a pointer to PreProcessInfo structure
     */
    void GetPreProcess(const char* name, const PreProcessInfo** info) const override {
        InputInfo::Ptr foundInput;
        DataPtr foundOutput;
        if (findInputAndOutputBlobByName(name, foundInput, foundOutput)) {
            *info = &foundInput->getPreProcess();
        } else {
            THROW_IE_EXCEPTION << "Output blob can't have pre-processing";
        }
    }

    void SetBatch(int batch) override {
        (void)batch;
        THROW_IE_EXCEPTION << "Dynamic batch is not supported";
    };

    /**
     * @brief      Sets the pointer to executable network internal.
     * @note       Needed to correctly handle ownership between objects.
     * @param[in]  exeNetwork  The executable network
     */
    void setPointerToExecutableNetworkInternal(std::shared_ptr<ExecutableNetworkInternal> exeNetwork) {
        _exeNetwork = exeNetwork;
    }

    /**
     * @brief      Checks that both inputs and outputs blob are valid. Throws an exception if they are not.
     */
    virtual void checkBlobs() {
        for (auto const& input : _inputs) {
            checkBlob(input.second, input.first, true);
        }
        for (auto const& output : _outputs) {
            checkBlob(output.second, output.first, false);
        }
    }

protected:
    InferenceEngine::InputsDataMap _networkInputs;  //!< Holds information about network inputs info
    InferenceEngine::OutputsDataMap _networkOutputs;  //!< Holds information about network outputs data
    InferenceEngine::BlobMap _inputs;  //!< A map of network input blobs
    InferenceEngine::BlobMap _outputs;  //!< A map of network output blobs
    std::map<std::string, PreProcessDataPtr> _preProcData;  //!< A map of pre-process data per input
    int m_curBatch;  //!< Current batch value used in dynamic batching

    /**
     * @brief A shared pointer to ExecutableNetworkInternal interface
     * @note Needed to correctly handle ownership between objects.
     */
    std::shared_ptr<ExecutableNetworkInternal> _exeNetwork;

    /**
     * @brief Checks and executes input data pre-processing if needed.
     * @param inputs Inputs blobs to perform preprocessing on
     * @param serial Whether to use multiple threads to execute the step
     */
    void execDataPreprocessing(InferenceEngine::BlobMap& inputs, bool serial = false) {
        for (auto& input : inputs) {
            // If there is a pre-process entry for an input then it must be pre-processed
            // using preconfigured resize algorithm.
            auto it = _preProcData.find(input.first);
            if (it != _preProcData.end()) {
                _preProcData[input.first]->execute(input.second, _networkInputs[input.first]->getPreProcess(), serial,
                                                   m_curBatch);
            }
        }
    }

    /**
     * @brief Helper function to find input or output blob by name
     * @param name A name of input or output blob.
     * @param foundInput A pointer to input information if found.
     * @param foundOutput A pointer to output DataPtr if found.
     * @return `True` - if loaded network has input with provided name,
     *         `false` - if loaded network has output with provided name
     * @throws [parameter_mismatch] exception if input and output has the same name
     * @throws [not_found] exception if there is no input and output layers with given name
     */
    bool findInputAndOutputBlobByName(const char* name, InputInfo::Ptr& foundInput, DataPtr& foundOutput) const {
        foundInput = nullptr;
        foundOutput = nullptr;
        if (_networkInputs.empty() || _networkOutputs.empty()) {
            THROW_IE_EXCEPTION << "Internal error: network inputs and outputs is not set";
        }
        auto foundInputPair = std::find_if(std::begin(_networkInputs), std::end(_networkInputs),
                                           [&](const std::pair<std::string, InputInfo::Ptr>& pair) {
                                               return pair.first == name;
                                           });
        auto foundOutputPair = std::find_if(std::begin(_networkOutputs), std::end(_networkOutputs),
                                            [&](const std::pair<std::string, DataPtr>& pair) {
                                                return pair.first == name;
                                            });
        if (foundOutputPair == std::end(_networkOutputs) && (foundInputPair == std::end(_networkInputs))) {
            THROW_IE_EXCEPTION << NOT_FOUND_str << "Failed to find input or output with name: \'" << name << "\'";
        }
        if (foundInputPair != std::end(_networkInputs)) {
            foundInput = foundInputPair->second;
            return true;
        } else {
            foundOutput = foundOutputPair->second;
            return false;
        }
    }

    /**
     * @brief      Check that @p blob is valid. Throws an exception if it's not.
     *
     * @param[in]  blob     The blob to check
     * @param[in]  name     The name of input or output depending of if the @p blob is input or output 
     * @param[in]  isInput  Indicates if @p is input
     * @param[in]  refDims  The reference dims, empty if not specified
     */
    void checkBlob(const Blob::Ptr& blob, const std::string& name, bool isInput, const SizeVector& refDims = {}) const {
        std::string bType = isInput ? "Input" : "Output";
        std::string sType = isInput ? "input" : "output";
        std::string strNotAllocated(bType + " data was not allocated.");
        std::string strNotMatched("The " + sType + " blob size is not equal to the network " + sType + " size");

        if (!blob) {
            THROW_IE_EXCEPTION << strNotAllocated;
        }
        size_t refSize;
        if (refDims.empty()) {
            SizeVector dims;
            if (isInput) {
                auto foundInputPair = std::find_if(std::begin(_networkInputs), std::end(_networkInputs),
                                                   [&](const std::pair<std::string, InputInfo::Ptr>& pair) {
                                                       return pair.first == name;
                                                   });
                if (foundInputPair == std::end(_networkInputs)) {
                    THROW_IE_EXCEPTION << NOT_FOUND_str << "Failed to find input with name: \'" << name << "\'";
                }
                dims = foundInputPair->second->getTensorDesc().getDims();
                refSize = foundInputPair->second->getTensorDesc().getLayout() != SCALAR
                    ? details::product(dims)
                    : 1;
            } else {
                auto foundOutputPair = std::find_if(std::begin(_networkOutputs), std::end(_networkOutputs),
                                                    [&](const std::pair<std::string, DataPtr>& pair) {
                                                        return pair.first == name;
                                                    });
                if (foundOutputPair == std::end(_networkOutputs)) {
                    THROW_IE_EXCEPTION << NOT_FOUND_str << "Failed to find output with name: \'" << name << "\'";
                }
                dims = foundOutputPair->second->getTensorDesc().getDims();
                refSize = foundOutputPair->second->getTensorDesc().getLayout() != SCALAR
                    ? details::product(dims)
                    : 1;
            }
        } else {
            refSize = details::product(refDims);
        }

        if (refSize != blob->size()) {
            THROW_IE_EXCEPTION << strNotMatched + ": got " << blob->size() << " expecting " << refSize;
        }
        if (blob->buffer() == nullptr) THROW_IE_EXCEPTION << strNotAllocated;
    }

    /**
     * @brief Checks whether pre-processing step is required for a given input
     * @param info InputInfo corresponding to input blob
     * @param blob Input Blob object corresponding to input info
     * @return `True` if pre-processing is required, `false` otherwise
     */
    bool preProcessingRequired(const InputInfo::Ptr& info, const Blob::Ptr& blob) {
        // pre-processing is required if:
        // 1. resize algorithm is specified (resize required)
        // 2. color format specified:
        // 2.a. color format is not equal to network's expected (color conversion required)
        // 2.b. network's layout != blob's layout (reorder required)
        const auto& preProcessInfo = info->getPreProcess();
        const auto inputColorFormat = preProcessInfo.getColorFormat();
        // FIXME: support other network's input formats once the API is ready. Assuming input is in
        // the BGR format by default
        const auto networkColorFormat = ColorFormat::BGR;

        const bool colorFormatSpecified = inputColorFormat != ColorFormat::RAW;
        return preProcessInfo.getResizeAlgorithm() != ResizeAlgorithm::NO_RESIZE ||
               (colorFormatSpecified && inputColorFormat != networkColorFormat) ||
               (colorFormatSpecified && info->getLayout() != blob->getTensorDesc().getLayout());
    }
};

}  // namespace InferenceEngine
