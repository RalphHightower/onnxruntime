// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/shared_library/provider_api.h"

#include "core/providers/cuda/gpu_data_transfer.h"
#include "cuda_common.h"

namespace onnxruntime {
bool GPUDataTransfer::CanCopy(const OrtDevice& src_device, const OrtDevice& dst_device) const {
  return src_device.Type() == OrtDevice::GPU || src_device.MemType() == OrtDevice::MemType::HOST_ACCESSIBLE ||
         dst_device.Type() == OrtDevice::GPU || dst_device.MemType() == OrtDevice::MemType::HOST_ACCESSIBLE;
}

common::Status GPUDataTransfer::CopyTensor(const Tensor& src, Tensor& dst) const {
  size_t bytes = src.SizeInBytes();
  const void* src_data = src.DataRaw();
  void* dst_data = dst.MutableDataRaw();

  auto& src_device = src.Location().device;
  auto& dst_device = dst.Location().device;

  // for the sync version of memcpy, launch to cuda default stream
  if (dst_device.Type() == OrtDevice::GPU) {
    if (src_device.Type() == OrtDevice::GPU) {
      // Copy only if the two addresses are different.
      if (dst_data != src_data) {
        CUDA_RETURN_IF_ERROR(cudaMemcpy(dst_data, src_data, bytes, cudaMemcpyDeviceToDevice));
        // For device memory to device memory copy, no host-side synchronization is performed by cudaMemcpy.
        // see https://docs.nvidia.com/cuda/cuda-runtime-api/api-sync-behavior.html
        CUDA_RETURN_IF_ERROR(cudaStreamSynchronize(nullptr));
      }
    } else {
      // copy from other CPU memory to GPU, this is blocking
      CUDA_RETURN_IF_ERROR(cudaMemcpy(dst_data, src_data, bytes, cudaMemcpyHostToDevice));
      if (src_device.MemType() != OrtDevice::MemType::HOST_ACCESSIBLE) {
        // For cudaMemcpy from pageable host memory to device memory, DMA to final destination may not have completed.
        // see https://docs.nvidia.com/cuda/cuda-runtime-api/api-sync-behavior.html
        CUDA_RETURN_IF_ERROR(cudaStreamSynchronize(nullptr));
      }
    }
  } else if (src_device.Type() == OrtDevice::GPU) {
    // copying from GPU to CPU memory, this is blocking
    CUDA_RETURN_IF_ERROR(cudaMemcpy(dst_data, src_data, bytes, cudaMemcpyDeviceToHost));
  } else {
    // copying between cpu memory
    ORT_ENFORCE(dst_data != src_data);
    memcpy(dst_data, src_data, bytes);
  }

  return Status::OK();
}

common::Status GPUDataTransfer::CopyTensorAsync(const Tensor& src, Tensor& dst, Stream& stream) const {
  size_t bytes = src.SizeInBytes();
  const void* src_data = src.DataRaw();
  void* dst_data = dst.MutableDataRaw();

  auto& src_device = src.Location().device;
  auto& dst_device = dst.Location().device;

  if (dst_device.Type() == OrtDevice::GPU) {
    if (src_device.Type() == OrtDevice::CPU) {
      // copy from pinned or non-pinned CPU memory to GPU
      CUDA_RETURN_IF_ERROR(cudaMemcpyAsync(dst_data, src_data, bytes, cudaMemcpyHostToDevice,
                                           static_cast<cudaStream_t>(stream.GetHandle())));
    } else if (src_device.Type() == OrtDevice::GPU) {
      // copying between GPU, this is non-blocking
      if (dst_data != src_data) {
        CUDA_RETURN_IF_ERROR(cudaMemcpyAsync(dst_data, src_data, bytes, cudaMemcpyDeviceToDevice,
                                             static_cast<cudaStream_t>(stream.GetHandle())));
      }
    }
  } else if (src_device.Type() == OrtDevice::GPU) {
    if (dst_device.Type() == OrtDevice::CPU) {
      // copy from GPU to pinned or non-pinned CPU memory.
      CUDA_RETURN_IF_ERROR(cudaMemcpyAsync(dst_data, src_data, bytes, cudaMemcpyDeviceToHost,
                                           static_cast<cudaStream_t>(stream.GetHandle())));
    }
  } else {
    if (src_device.MemType() == OrtDevice::MemType::HOST_ACCESSIBLE) {
      // sync the stream first to make sure the data arrived
      CUDA_RETURN_IF_ERROR(cudaStreamSynchronize(static_cast<cudaStream_t>(stream.GetHandle())));
    }

    ORT_ENFORCE(dst_data != src_data);
    memcpy(dst_data, src_data, bytes);
  }

  return Status::OK();
}

}  // namespace onnxruntime
