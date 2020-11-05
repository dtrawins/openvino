// Copyright (c) 2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
#include "convolution_kernel_base.h"
#include <string>
#include <vector>

namespace kernel_selector {

class ConvolutionKernel_fs_byx_fsv32_depthwise : public ConvolutionKernelBase {
public:
    ConvolutionKernel_fs_byx_fsv32_depthwise();
    virtual ~ConvolutionKernel_fs_byx_fsv32_depthwise() {}

    KernelsData GetKernelsData(const Params& params, const optional_params& options) const override;
    KernelsData GetKernelsDataForAutoTune(const Params& params, const optional_params& options) const override;
    ParamsKey GetSupportedKey() const override;
    KernelsData GetTunedKernelsDataByIndex(const Params& params,
                                           const optional_params& options,
                                           int autoTuneIndex = -1) const override;

protected:
    WeightsLayout GetPreferredWeightsLayout(const convolution_params &) const override {
        return WeightsLayout::gs_oiyx_gsv32;
    }

    std::vector<FusedOpType> GetSupportedFusedOps() const override {
        return { FusedOpType::ELTWISE,
                 FusedOpType::QUANTIZE,
                 FusedOpType::SCALE,
                 FusedOpType::ACTIVATION };
    }

    bool Validate(const Params& p, const optional_params& o) const override;
    JitConstants GetJitConstants(const convolution_params& params, const DispatchData& dispatchData) const override;
    DispatchData SetDefault(const convolution_params& arg, int autoTuneIndex = -1) const override;
    bool NeedPaddedInput() const override { return true; }

private:
    struct AutoTuneOption {
        size_t blockWidth;
        std::string exeMode;
    };

    std::vector<AutoTuneOption> autoTuneOptions;
    AutoTuneOption GetAutoTuneOptions(const Params& arg, int autoTuneIndex) const;
    size_t getInputWidth(const convolution_params &arg, size_t blockWidth) const;
    size_t getMinRegisterUsage(const convolution_params &arg, size_t blockWidth) const;
};

}  // namespace kernel_selector
