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

#ifndef CORAL_CODEC_WMF_EXTRACTOR_BYTE_STREAM_H
#define CORAL_CODEC_WMF_EXTRACTOR_BYTE_STREAM_H

#include <extractor/MediaExtractorInput.h>

#include <mfobjects.h>
#include <wrl/implements.h>

#include <cstdint>

namespace coral {

/**
 * The media of an {@link AVMediaExtractorInput}, i.e. of the reader of the extractor module, read by
 * the Media Foundation as a byte stream.
 *
 * <p>The extractor of the platform does not open the media of a url itself: it reads the media the
 * reader owns, through this stream. That is what makes a platform extractor interchangeable with a
 * built-in one, because the reader then measures the bytes it consumed by the position of its input,
 * which is what tells it that the extractor is not stuck. See {@link AVMediaExtractorInput}.
 *
 * <p>The stream only reads, it never writes, and it holds a reference to the input rather than owning
 * it: the extractor is the one that keeps the input alive while it drives the reader, and it releases
 * the reader before the input.
 */
class WmfExtractorByteStream final : public Microsoft::WRL::RuntimeClass<
      Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFByteStream> {
public:
  explicit WmfExtractorByteStream(AVMediaExtractorInput& input) noexcept;

  ~WmfExtractorByteStream() override = default;

  WmfExtractorByteStream(const WmfExtractorByteStream& other) = delete;

  WmfExtractorByteStream& operator=(const WmfExtractorByteStream& other) = delete;

  // ================================================================================================
  // IMFByteStream
  // ================================================================================================

  HRESULT STDMETHODCALLTYPE GetCapabilities(DWORD* capabilities) override;

  HRESULT STDMETHODCALLTYPE GetLength(QWORD* length) override;

  HRESULT STDMETHODCALLTYPE SetLength(QWORD length) override;

  HRESULT STDMETHODCALLTYPE GetCurrentPosition(QWORD* position) override;

  HRESULT STDMETHODCALLTYPE SetCurrentPosition(QWORD position) override;

  HRESULT STDMETHODCALLTYPE IsEndOfStream(BOOL* endOfStream) override;

  HRESULT STDMETHODCALLTYPE Read(BYTE* buffer, ULONG size, ULONG* readCount) override;

  HRESULT STDMETHODCALLTYPE
  BeginRead(BYTE* buffer, ULONG size, IMFAsyncCallback* callback, IUnknown* state) override;

  HRESULT STDMETHODCALLTYPE EndRead(IMFAsyncResult* result, ULONG* readCount) override;

  HRESULT STDMETHODCALLTYPE Write(const BYTE* buffer, ULONG size, ULONG* writtenCount) override;

  HRESULT STDMETHODCALLTYPE
  BeginWrite(const BYTE* buffer, ULONG size, IMFAsyncCallback* callback, IUnknown* state) override;

  HRESULT STDMETHODCALLTYPE EndWrite(IMFAsyncResult* result, ULONG* writtenCount) override;

  HRESULT STDMETHODCALLTYPE
  Seek(MFBYTESTREAM_SEEK_ORIGIN origin, LONGLONG offset, DWORD seekFlags,
      QWORD* currentPosition) override;

  HRESULT STDMETHODCALLTYPE Flush() override;

  HRESULT STDMETHODCALLTYPE Close() override;

private:
  AVMediaExtractorInput& input_;
};

} // namespace coral

#endif //CORAL_CODEC_WMF_EXTRACTOR_BYTE_STREAM_H
