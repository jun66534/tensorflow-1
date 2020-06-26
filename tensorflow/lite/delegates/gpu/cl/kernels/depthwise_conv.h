/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_DEPTHWISE_CONV_H_
#define TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_DEPTHWISE_CONV_H_

#include <vector>

#include "tensorflow/lite/delegates/gpu/cl/buffer.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/gpu_operation.h"
#include "tensorflow/lite/delegates/gpu/cl/linear_storage.h"
#include "tensorflow/lite/delegates/gpu/cl/tensor.h"
#include "tensorflow/lite/delegates/gpu/cl/texture2d.h"
#include "tensorflow/lite/delegates/gpu/cl/util.h"
#include "tensorflow/lite/delegates/gpu/common/data_type.h"
#include "tensorflow/lite/delegates/gpu/common/operations.h"
#include "tensorflow/lite/delegates/gpu/common/shape.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"
#include "tensorflow/lite/delegates/gpu/common/tensor.h"
#include "tensorflow/lite/delegates/gpu/common/types.h"

namespace tflite {
namespace gpu {
namespace cl {

class DepthwiseConvolution : public GPUOperation {
 public:
  DepthwiseConvolution() = default;
  absl::Status AddToQueue(CLCommandQueue* queue) override;
  absl::Status Tune(const TuningParameters& params) override;

  absl::Status Compile(const CreationContext& creation_context) override;

  // Move only
  DepthwiseConvolution(DepthwiseConvolution&& operation);
  DepthwiseConvolution& operator=(DepthwiseConvolution&& operation);
  DepthwiseConvolution(const DepthwiseConvolution&) = delete;
  DepthwiseConvolution& operator=(const DepthwiseConvolution&) = delete;

 private:
  friend absl::Status CreateDepthwiseConvolution(
      const CreationContext& creation_context, const OperationDef& definition,
      const DepthwiseConvolution2DAttributes& attr,
      DepthwiseConvolution* result);
  friend absl::Status CreateDepthwiseConvolution(
      const CreationContext& creation_context, const OperationDef& definition,
      const DepthwiseConvolution3DAttributes& attr,
      DepthwiseConvolution* result);
  DepthwiseConvolution(const OperationDef& definition,
                       const DepthwiseConvolution2DAttributes& attr,
                       bool weights_are_buffer);
  DepthwiseConvolution(const OperationDef& definition,
                       const DepthwiseConvolution3DAttributes& attr,
                       bool weights_are_buffer);

  template <DataType T>
  absl::Status UploadWeights(const tflite::gpu::Tensor<OHWI, T>& weights,
                             CLContext* context);

  template <DataType S, typename T>
  void RearrangeWeightsData(const tflite::gpu::Tensor<OHWI, S>& weights,
                            absl::Span<T> dst);

  template <DataType T>
  absl::Status UploadWeights(const tflite::gpu::Tensor<OHWDI, T>& weights,
                             CLContext* context);

  template <DataType S, typename T>
  void RearrangeWeightsData(const tflite::gpu::Tensor<OHWDI, S>& weights,
                            absl::Span<T> dst);

  absl::Status BindArguments();
  int3 GetGridSize() const;

  bool weights_are_buffer_;

  int4 kernel_size_;
  int4 stride_;
  int4 padding_;
  int4 dilation_;
  int channel_multiplier_;

  CLKernel kernel_;
  int3 work_group_size_;
};

template <DataType T>
absl::Status DepthwiseConvolution::UploadWeights(
    const tflite::gpu::Tensor<OHWI, T>& weights, CLContext* context) {
  const int dst_channels = weights.shape.i * weights.shape.o;
  const int dst_slices = DivideRoundUp(dst_channels, 4);
  const int kernel_x = weights.shape.w;
  const int kernel_y = weights.shape.h;

  const int elements_count = kernel_x * kernel_y * dst_slices;

  const bool fp32_weights = definition_.precision == CalculationsPrecision::F32;
  const int float4_size = fp32_weights ? 16 : 8;

  Texture2D weights_tex2d;
  Buffer weights_buf;
  if (fp32_weights) {
    std::vector<float4> gpu_data(elements_count);
    RearrangeWeightsData(weights, absl::MakeSpan(gpu_data));
    if (weights_are_buffer_) {
      RETURN_IF_ERROR(CreateReadOnlyBuffer(float4_size * elements_count,
                                           gpu_data.data(), context,
                                           &weights_buf));
    } else {
      RETURN_IF_ERROR(CreateTexture2DRGBA(
          definition_.GetDataType(), kernel_x * kernel_y, dst_slices,
          gpu_data.data(), context, &weights_tex2d));
    }
  } else {
    std::vector<half4> gpu_data(elements_count);
    RearrangeWeightsData(weights, absl::MakeSpan(gpu_data));
    if (weights_are_buffer_) {
      RETURN_IF_ERROR(CreateReadOnlyBuffer(float4_size * elements_count,
                                           gpu_data.data(), context,
                                           &weights_buf));
    } else {
      RETURN_IF_ERROR(CreateTexture2DRGBA(
          definition_.GetDataType(), kernel_x * kernel_y, dst_slices,
          gpu_data.data(), context, &weights_tex2d));
    }
  }

  if (weights_are_buffer_) {
    BufferDescriptor desc;
    desc.element_type = fp32_weights ? DataType::FLOAT32 : DataType::FLOAT16;
    desc.element_size = 4;
    args_.AddObject("weights", AccessType::READ,
                    absl::make_unique<Buffer>(std::move(weights_buf)),
                    absl::make_unique<BufferDescriptor>(desc));
  } else {
    Texture2DDescriptor desc;
    desc.element_type = fp32_weights ? DataType::FLOAT32 : DataType::FLOAT16;
    args_.AddObject("weights", AccessType::READ,
                    absl::make_unique<Texture2D>(std::move(weights_tex2d)),
                    absl::make_unique<Texture2DDescriptor>(desc));
  }

  return absl::OkStatus();
}

template <DataType S, typename T>
void DepthwiseConvolution::RearrangeWeightsData(
    const tflite::gpu::Tensor<OHWI, S>& weights, absl::Span<T> dst) {
  const int dst_channels = weights.shape.i * weights.shape.o;
  const int dst_depth = DivideRoundUp(dst_channels, 4);
  const int kernel_x = weights.shape.w;
  const int kernel_y = weights.shape.h;

  int counter = 0;
  for (int d = 0; d < dst_depth; ++d) {
    for (int y = 0; y < kernel_y; ++y) {
      for (int x = 0; x < kernel_x; ++x) {
        T filter_val;
        for (int i = 0; i < 4; ++i) {
          const int d_ch = d * 4 + i;
          if (d_ch < dst_channels) {
            const int f_index = weights.shape.LinearIndex(
                {d_ch % weights.shape.o, y, x, d_ch / weights.shape.o});
            filter_val[i] = weights.data[f_index];
          } else {
            filter_val[i] = 0.0f;
          }
        }
        dst[counter++] = filter_val;
      }
    }
  }
}

template <DataType T>
absl::Status DepthwiseConvolution::UploadWeights(
    const tflite::gpu::Tensor<OHWDI, T>& weights, CLContext* context) {
  const int dst_channels = weights.shape.i * weights.shape.o;
  const int dst_slices = DivideRoundUp(dst_channels, 4);
  const int kernel_x = weights.shape.w;
  const int kernel_y = weights.shape.h;
  const int kernel_z = weights.shape.d;

  const int elements_count = kernel_x * kernel_y * kernel_z * dst_slices;

  const bool fp32_weights = definition_.precision == CalculationsPrecision::F32;
  const int float4_size = fp32_weights ? 16 : 8;

  Texture2D weights_tex2d;
  Buffer weights_buf;
  if (fp32_weights) {
    std::vector<float4> gpu_data(elements_count);
    RearrangeWeightsData(weights, absl::MakeSpan(gpu_data));
    if (weights_are_buffer_) {
      RETURN_IF_ERROR(CreateReadOnlyBuffer(float4_size * elements_count,
                                           gpu_data.data(), context,
                                           &weights_buf));
    } else {
      RETURN_IF_ERROR(CreateTexture2DRGBA(
          definition_.GetDataType(), kernel_x * kernel_y * kernel_z, dst_slices,
          gpu_data.data(), context, &weights_tex2d));
    }
  } else {
    std::vector<half4> gpu_data(elements_count);
    RearrangeWeightsData(weights, absl::MakeSpan(gpu_data));
    if (weights_are_buffer_) {
      RETURN_IF_ERROR(CreateReadOnlyBuffer(float4_size * elements_count,
                                           gpu_data.data(), context,
                                           &weights_buf));
    } else {
      RETURN_IF_ERROR(CreateTexture2DRGBA(
          definition_.GetDataType(), kernel_x * kernel_y * kernel_z, dst_slices,
          gpu_data.data(), context, &weights_tex2d));
    }
  }

  if (weights_are_buffer_) {
    BufferDescriptor desc;
    desc.element_type = fp32_weights ? DataType::FLOAT32 : DataType::FLOAT16;
    desc.element_size = 4;
    args_.AddObject("weights", AccessType::READ,
                    absl::make_unique<Buffer>(std::move(weights_buf)),
                    absl::make_unique<BufferDescriptor>(desc));
  } else {
    Texture2DDescriptor desc;
    desc.element_type = fp32_weights ? DataType::FLOAT32 : DataType::FLOAT16;
    args_.AddObject("weights", AccessType::READ,
                    absl::make_unique<Texture2D>(std::move(weights_tex2d)),
                    absl::make_unique<Texture2DDescriptor>(desc));
  }

  return absl::OkStatus();
}

template <DataType S, typename T>
void DepthwiseConvolution::RearrangeWeightsData(
    const tflite::gpu::Tensor<OHWDI, S>& weights, absl::Span<T> dst) {
  const int dst_channels = weights.shape.i * weights.shape.o;
  const int dst_slices = DivideRoundUp(dst_channels, 4);
  const int kernel_x = weights.shape.w;
  const int kernel_y = weights.shape.h;
  const int kernel_z = weights.shape.d;

  int counter = 0;
  for (int d = 0; d < dst_slices; ++d) {
    for (int z = 0; z < kernel_z; ++z) {
      for (int y = 0; y < kernel_y; ++y) {
        for (int x = 0; x < kernel_x; ++x) {
          T filter_val;
          for (int i = 0; i < 4; ++i) {
            const int d_ch = d * 4 + i;
            if (d_ch < dst_channels) {
              const int f_index = weights.shape.LinearIndex(
                  {d_ch % weights.shape.o, y, x, z, d_ch / weights.shape.o});
              filter_val[i] = weights.data[f_index];
            } else {
              filter_val[i] = 0.0f;
            }
          }
          dst[counter++] = filter_val;
        }
      }
    }
  }
}

absl::Status CreateDepthwiseConvolution(
    const CreationContext& creation_context, const OperationDef& definition,
    const DepthwiseConvolution2DAttributes& attr, DepthwiseConvolution* result);

}  // namespace cl
}  // namespace gpu
}  // namespace tflite

#endif  // TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_DEPTHWISE_CONV_H_
