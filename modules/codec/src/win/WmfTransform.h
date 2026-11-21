/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CORAL_CODEC_WMF_TRANSFORM_H
#define CORAL_CODEC_WMF_TRANSFORM_H

#include <mfapi.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace coral {

/**
 * A thin synchronous wrapper of the Media Foundation transform, shared by the Windows audio and
 * video codec implementations.
 */
class WmfTransformContext final {
public:
  /**
   * The availability of the transform output.
   */
  enum class OutputState : uint8_t {
    //!< An output sample is ready to be pulled.
    kSampleReady = 0,
    //!< The transform requires more input data.
    kNeedMoreInput = 1,
    //!< The transform does not report the output state, the output must be pulled directly.
    kUnknown = 2,
  };

  WmfTransformContext() = default;

  ~WmfTransformContext() = default;

  void attach(Microsoft::WRL::ComPtr<IMFTransform> transform);

  [[nodiscard]] bool isValid() const {
    return mTransform != nullptr;
  }

  [[nodiscard]] IMFTransform* get() const {
    return mTransform.Get();
  }

  void reset();

  HRESULT setInputType(IMFMediaType* mediaType);

  HRESULT setOutputType(IMFMediaType* mediaType);

  HRESULT getOutputType(IMFMediaType** mediaType) const;

  /**
   * Returns the output type which matches the required sub type.
   */
  Microsoft::WRL::ComPtr<IMFMediaType>
  findOutputType(const GUID& majorType, const GUID& subType) const;

  /**
   * Indicates whether the transform has an output sample ready.
   *
   * Several video decoders of the platform must not be polled with {@link processOutput} while the
   * output is not ready, so the state is queried first.
   */
  [[nodiscard]] OutputState queryOutputState() const;

  HRESULT processInput(IMFSample* sample);

  /**
   * Pulls one output sample of the transform.
   *
   * @return S_OK when a sample is produced, MF_E_TRANSFORM_NEED_MORE_INPUT when the transform
   *         requires more input data, MF_E_TRANSFORM_STREAM_CHANGE when the output type must be
   *         renegotiated, or another failure HRESULT.
   */
  HRESULT processOutput(Microsoft::WRL::ComPtr<IMFSample>& sample);

  void notifyStartOfStream();

  void flush();

  void drain();

  void release();

private:
  Microsoft::WRL::ComPtr<IMFTransform> mTransform;
  bool mDraining = false;
};

} // namespace coral

#endif //CORAL_CODEC_WMF_TRANSFORM_H
