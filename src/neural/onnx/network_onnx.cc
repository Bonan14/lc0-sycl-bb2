/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2021 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#if __has_include("dml_provider_factory.h")
#include "dml_provider_factory.h"
#define USE_DML
#endif

#include "cpu_provider_factory.h"
#include "neural/factory.h"
#include "neural/loader.h"
#include "neural/network.h"
#include "neural/onnx/converter.h"
#include "onnxruntime_cxx_api.h"
#include "utils/bititer.h"
#include "utils/exception.h"
#include "utils/logging.h"

namespace lczero {
namespace {

enum class OnnxProvider { CPU, CUDA, DML };

class OnnxNetwork;

class OnnxComputation : public NetworkComputation {
 public:
  OnnxComputation(OnnxNetwork* network) : network_(network) {}
  void AddInput(InputPlanes&& input) override;
  int GetBatchSize() const override { return raw_input_.size(); }
  void ComputeBlocking() override;
  float GetQVal(int sample) const override;
  float GetDVal(int sample) const override;
  float GetPVal(int sample, int move_id) const override;
  float GetMVal(int sample) const override;

 private:
  Ort::Value PrepareInput();

  OnnxNetwork* network_;
  std::vector<InputPlanes> raw_input_;
  std::vector<float> input_tensor_data_;
  std::vector<Ort::Value> output_tensors_;
};

class OnnxNetwork : public Network {
 public:
  OnnxNetwork(const WeightsFile& file, const OptionsDict& options,
              OnnxProvider provider, int batch_size);
  std::unique_ptr<NetworkComputation> NewComputation() override {
    return std::make_unique<OnnxComputation>(this);
  }
  const NetworkCapabilities& GetCapabilities() const override {
    return capabilities_;
  }

  Ort::Env onnx_env_;
  Ort::SessionOptions session_options_;
  Ort::Session session_;
  std::vector<std::string> inputs_;
  // Points to strings in inputs_.
  std::vector<const char*> inputs_cstr_;
  std::vector<std::string> outputs_;
  // Points to strings in outputs_.
  std::vector<const char*> outputs_cstr_;
  // Indices in output_cstr_ vector.
  int policy_head_ = -1;
  int wdl_head_ = -1;
  int value_head_ = -1;
  int mlh_head_ = -1;
  NetworkCapabilities capabilities_;
  // The batch size to use, or -1 for variable.
  int batch_size_;
};

void OnnxComputation::AddInput(InputPlanes&& input) {
  raw_input_.emplace_back(input);
  size_t batch_size = network_->batch_size_;
  if (raw_input_.size() > batch_size) {
    throw Exception("NN input exceeds batch size of " +
                    std::to_string(batch_size) + ".");
  }
}

float OnnxComputation::GetQVal(int sample) const {
  if (network_->wdl_head_ != -1) {
    const auto& data =
        output_tensors_[network_->wdl_head_].GetTensorData<float>();
    return data[sample * 3 + 0] - data[sample * 3 + 2];
  } else {
    const auto& data =
        output_tensors_[network_->value_head_].GetTensorData<float>();
    return data[sample];
  }
}
float OnnxComputation::GetDVal(int sample) const {
  if (network_->wdl_head_ == -1) return 0.0f;
  const auto& data =
      output_tensors_[network_->wdl_head_].GetTensorData<float>();
  return data[sample * 3 + 1];
}
float OnnxComputation::GetPVal(int sample, int move_id) const {
  const auto& data =
      output_tensors_[network_->policy_head_].GetTensorData<float>();
  return data[sample * 1858 + move_id];
}
float OnnxComputation::GetMVal(int sample) const {
  if (network_->mlh_head_ == -1) return 0.0f;
  const auto& data =
      output_tensors_[network_->mlh_head_].GetTensorData<float>();
  return data[sample];
}

Ort::Value OnnxComputation::PrepareInput() {
  input_tensor_data_.clear();
  int batch_size = network_->batch_size_;
  if (batch_size < 0) batch_size = raw_input_.size();
  input_tensor_data_.resize(batch_size * kInputPlanes * 8 * 8);
  auto iter = input_tensor_data_.data();
  for (const auto& sample : raw_input_) {
    assert(sample.size() == kInputPlanes);
    for (const auto& plane : sample) {
      for (auto bit : IterateBits(plane.mask)) {
        *(iter + bit) = plane.value;
      }
      iter += 64;
    }
  }
  for (int i = raw_input_.size() * kInputPlanes * 64;
       i < batch_size * kInputPlanes * 64; i++) {
    *iter++ = 0;
  }

  int64_t dims[] = {batch_size, kInputPlanes, 8, 8};
  auto memory_info =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  // Hopefully having dims in a temporary variable is fine.
  return Ort::Value::CreateTensor<float>(memory_info, input_tensor_data_.data(),
                                         input_tensor_data_.size(), dims, 4);
}

void OnnxComputation::ComputeBlocking() {
  auto input_tensor = PrepareInput();
  output_tensors_ = network_->session_.Run(
      {}, network_->inputs_cstr_.data(), &input_tensor, 1,
      network_->outputs_cstr_.data(), network_->outputs_cstr_.size());
}

Ort::SessionOptions GetOptions(OnnxProvider provider, const OptionsDict& dict) {
  Ort::SessionOptions options;
  OrtCUDAProviderOptions cuda_options;
  // options.SetIntraOpNumThreads(1);
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  switch (provider) {
    case OnnxProvider::DML:
#ifdef USE_DML
      Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(
          options, dict.GetOrDefault<int>("gpu", 0)));
#else
      throw Exception("ONNX backend internal error.");
#endif
      break;
    case OnnxProvider::CUDA:
      cuda_options.device_id = dict.GetOrDefault<int>("gpu", 0);
      options.AppendExecutionProvider_CUDA(cuda_options);
      break;
    case OnnxProvider::CPU:
      // Doesn't really work. :-( There are two execution providers (CUDA and
      // CPU) already added, don't know how to force it to use CPU.
      auto status = OrtSessionOptionsAppendExecutionProvider_CPU(options, 0);
      if (status) {
        std::string error_message = Ort::GetApi().GetErrorMessage(status);
        OrtErrorCode error_code = Ort::GetApi().GetErrorCode(status);
        Ort::GetApi().ReleaseStatus(status);
        throw Exception("ONNX CPU error " + std::to_string(error_code) + ": " +
                        error_message);
      }
      break;
  }
  return options;
}

OnnxNetwork::OnnxNetwork(const WeightsFile& file, const OptionsDict& dict,
                         OnnxProvider provider, int batch_size)
    : onnx_env_(ORT_LOGGING_LEVEL_WARNING, "lc0"),
      session_(onnx_env_, file.onnx_model().model().data(),
               file.onnx_model().model().size(), GetOptions(provider, dict)),
      capabilities_{file.format().network_format().input(),
                    file.format().network_format().moves_left()},
      batch_size_(batch_size) {
  const auto& md = file.onnx_model();
  if (!md.has_input_planes()) {
    throw Exception("NN doesn't have input planes defined.");
  }
  inputs_.emplace_back(md.input_planes());
  if (!md.has_output_policy()) {
    throw Exception("NN doesn't have policy head defined.");
  }
  policy_head_ = outputs_.size();
  outputs_.emplace_back(md.output_policy());
  if (md.has_output_wdl()) {
    wdl_head_ = outputs_.size();
    outputs_.emplace_back(md.output_wdl());
  } else if (md.has_output_value()) {
    value_head_ = outputs_.size();
    outputs_.emplace_back(md.output_value());
  } else {
    throw Exception("NN doesn't have value head.");
  }
  if (md.has_output_mlh()) {
    mlh_head_ = outputs_.size();
    outputs_.emplace_back(md.output_mlh());
  }
  std::transform(inputs_.begin(), inputs_.end(),
                 std::back_inserter(inputs_cstr_),
                 [](const auto& x) { return x.c_str(); });
  std::transform(outputs_.begin(), outputs_.end(),
                 std::back_inserter(outputs_cstr_),
                 [](const auto& x) { return x.c_str(); });
}

template <OnnxProvider kProvider>
std::unique_ptr<Network> MakeOnnxNetwork(const std::optional<WeightsFile>& w,
                                         const OptionsDict& opts) {
  if (!w) throw Exception("The ONNX backend requires a network file.");

  int batch_size = opts.GetOrDefault<int>(
      "batch", kProvider == OnnxProvider::DML ? 256 : -1);
  if (batch_size <= 0) batch_size = -1;  // Variable batch size.

  if (w->has_onnx_model()) {
    return std::make_unique<OnnxNetwork>(*w, opts, kProvider, batch_size);
  } else {
    if (w->format().network_format().network() !=
            pblczero::NetworkFormat::NETWORK_CLASSICAL_WITH_HEADFORMAT &&
        w->format().network_format().network() !=
            pblczero::NetworkFormat::NETWORK_SE_WITH_HEADFORMAT) {
      throw Exception("Network format " +
                      pblczero::NetworkFormat::NetworkStructure_Name(
                          w->format().network_format().network()) +
                      " is not supported by the ONNX backend.");
    }
    if (w->format().network_format().policy() !=
            pblczero::NetworkFormat::POLICY_CLASSICAL &&
        w->format().network_format().policy() !=
            pblczero::NetworkFormat::POLICY_CONVOLUTION) {
      throw Exception("Policy format " +
                      pblczero::NetworkFormat::PolicyFormat_Name(
                          w->format().network_format().policy()) +
                      " is not supported by the ONNX backend.");
    }
    if (w->format().network_format().value() !=
            pblczero::NetworkFormat::VALUE_CLASSICAL &&
        w->format().network_format().value() !=
            pblczero::NetworkFormat::VALUE_WDL) {
      throw Exception("Value format " +
                      pblczero::NetworkFormat::ValueFormat_Name(
                          w->format().network_format().value()) +
                      " is not supported by the ONNX backend.");
    }
    if (w->format().network_format().default_activation() !=
        pblczero::NetworkFormat::DEFAULT_ACTIVATION_RELU) {
      throw Exception("Default activation " +
                      pblczero::NetworkFormat::DefaultActivation_Name(
                          w->format().network_format().default_activation()) +
                      " is not supported by the ONNX backend.");
    }
    WeightsToOnnxConverterOptions converter_options;
    converter_options.batch_size = batch_size;
    auto converted = ConvertWeightsToOnnx(*w, converter_options);
    return std::make_unique<OnnxNetwork>(converted, opts, kProvider,
                                         batch_size);
  }
}

#ifdef USE_DML
REGISTER_NETWORK("onnx-dml", MakeOnnxNetwork<OnnxProvider::DML>, 60)
#endif
REGISTER_NETWORK("onnx-cuda", MakeOnnxNetwork<OnnxProvider::CUDA>, 61)
REGISTER_NETWORK("onnx-cpu", MakeOnnxNetwork<OnnxProvider::CPU>, 62)

}  // namespace
}  // namespace lczero
