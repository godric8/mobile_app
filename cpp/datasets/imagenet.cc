/* Copyright 2019 The MLPerf Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "cpp/datasets/imagenet.h"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <streambuf>
#include <string>
#include <unordered_set>

#include "cpp/utils.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/tools/evaluation/proto/evaluation_stages.pb.h"
#include "tensorflow/lite/tools/evaluation/stages/image_preprocessing_stage.h"
#include "tensorflow/lite/tools/evaluation/utils.h"

namespace mlperf {
namespace mobile {
namespace {
// TODO(b/145480762) Remove this code when preprocessing code is refactored.
inline TfLiteType DataType2TfType(DataType::Type type) {
  switch (type) {
    case DataType::Float32:
      return kTfLiteFloat32;
    case DataType::Uint8:
      return kTfLiteUInt8;
    case DataType::Int8:
      return kTfLiteInt8;
    case DataType::Float16:
      return kTfLiteFloat16;
  }
}
}  // namespace

Imagenet::Imagenet(const DataFormat& input_format,
                   const DataFormat& output_format,
                   const std::string& image_dir,
                   const std::string& groundtruth_file, int offset,
                   int image_width, int image_height)
    : Dataset(input_format, output_format),
      groundtruth_file_(groundtruth_file),
      offset_(offset) {
  if (input_format_.size() != 1 || output_format_.size() != 1) {
    LOG(FATAL) << "Imagenet only supports 1 input and 1 output";
    return;
  }
  // Finds all images under image_dir.
  std::unordered_set<std::string> exts{".rgb8", ".jpg", ".jpeg"};
  TfLiteStatus ret = tflite::evaluation::GetSortedFileNames(
      tflite::evaluation::StripTrailingSlashes(image_dir), &image_list_, exts);
  if (ret == kTfLiteError || image_list_.empty()) {
    LOG(FATAL) << "Failed to list all the images file in provided path";
    return;
  }
  samples_ =
      std::vector<std::vector<std::vector<uint8_t>*>>(image_list_.size());
  // Prepares the preprocessing stage.
  std::string image_ext =
      image_list_.at(0).substr(image_list_.at(0).find_last_of("."));
  bool is_raw_image = (image_ext == ".rgb8");
  tflite::evaluation::EvaluationStageConfig preprocessing_config;
  preprocessing_config.set_name("image_preprocessing");
  auto* preprocess_params = preprocessing_config.mutable_specification()
                                ->mutable_image_preprocessing_params();
  preprocess_params->set_image_height(image_height);
  preprocess_params->set_image_width(image_width);
  preprocess_params->set_aspect_preserving(true);
  preprocess_params->set_output_type(
      static_cast<int>(DataType2TfType(input_format_.at(0).type)));
  preprocess_params->set_load_raw_images(is_raw_image);
  preprocessing_stage_.reset(
      new tflite::evaluation::ImagePreprocessingStage(preprocessing_config));
  if (preprocessing_stage_->Init() != kTfLiteOk) {
    LOG(FATAL) << "Failed to init preprocessing stage";
  }
}

void Imagenet::LoadSamplesToRam(const std::vector<QuerySampleIndex>& samples) {
  for (QuerySampleIndex sample_idx : samples) {
    // Preprocessing.
    if (sample_idx >= image_list_.size()) {
      LOG(FATAL) << "Sample index out of bound";
    }
    std::string filename = image_list_.at(sample_idx);
    preprocessing_stage_->SetImagePath(&filename);
    if (preprocessing_stage_->Run() != kTfLiteOk) {
      LOG(FATAL) << "Failed to run preprocessing stage";
    }

    // Move data out of preprocessing_stage_ so it can be reused.
    int total_byte = input_format_[0].size * input_format_[0].GetByte();
    void* data_void = preprocessing_stage_->GetPreprocessedImageData();
    std::vector<uint8_t>* data_uint8 = new std::vector<uint8_t>(total_byte);
    std::copy(static_cast<uint8_t*>(data_void),
              static_cast<uint8_t*>(data_void) + total_byte,
              data_uint8->begin());
    samples_.at(sample_idx).push_back(data_uint8);
  }
}

void Imagenet::UnloadSamplesFromRam(
    const std::vector<QuerySampleIndex>& samples) {
  for (QuerySampleIndex sample_idx : samples) {
    for (std::vector<uint8_t>* v : samples_.at(sample_idx)) {
      delete v;
    }
    samples_.at(sample_idx).clear();
  }
}

std::vector<uint8_t> Imagenet::ProcessOutput(
    const int sample_idx, const std::vector<void*>& outputs) {
  std::vector<int32_t> topk;
  void* output = outputs.at(0);
  int data_size = output_format_[0].size;
  switch (output_format_[0].type) {
    case DataType::Float32:
      topk = GetTopK(reinterpret_cast<float*>(output), data_size, 1, offset_);
      break;
    case DataType::Uint8:
      topk = GetTopK(reinterpret_cast<uint8_t*>(output), data_size, 1, offset_);
      break;
    case DataType::Int8:
      topk = GetTopK(reinterpret_cast<int8_t*>(output), data_size, 1, offset_);
      break;
    case DataType::Float16:
      topk =
          GetTopK(reinterpret_cast<uint16_t*>(output), data_size, 1, offset_);
      break;
  }
  // Mlperf interpret data as uint8_t* and log it as a HEX string.
  predictions_[sample_idx] = topk.at(0);
  std::vector<uint8_t> result(topk.size() * 4);
  uint8_t* temp_data = reinterpret_cast<uint8_t*>(topk.data());
  std::copy(temp_data, temp_data + result.size(), result.begin());
  return result;
}
float Imagenet::ComputeAccuracy() {
  std::ifstream gt_file(groundtruth_file_);
  if (!gt_file.good()) {
    LOG(FATAL) << "Could not read the ground truth file";
    return 0.0f;
  }
  int32_t label_idx;
  std::vector<int32_t> groundtruth;
  while (gt_file >> label_idx) {
    groundtruth.push_back(label_idx);
  }

  // Read the result in mlpef log file and calculate the accuracy.
  int good = 0;
  for (auto [sample_idx, class_idx] : predictions_) {
    if (groundtruth[sample_idx] == class_idx) good++;
  }
  return static_cast<float>(good) / predictions_.size();
}

std::string Imagenet::ComputeAccuracyString() {
  float result = ComputeAccuracy();
  if (result == 0.0f) {
    return std::string("N/A");
  }
  std::stringstream stream;
  stream << std::fixed << std::setprecision(2) << result * 100 << "%";
  return stream.str();
}

}  // namespace mobile
}  // namespace mlperf