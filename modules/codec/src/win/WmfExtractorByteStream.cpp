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

#include "WmfExtractorByteStream.h"

#include <mfapi.h>

namespace coral {

namespace {

/**
 * The number of bytes a read of the stream returned, which the caller that completes the read asks for.
 *
 * <p>A byte stream reports that number through the object of the asynchronous result of the read, see
 * {@link WmfExtractorByteStream::EndRead}: the result itself carries the state of the caller, which the
 * caller reads to know which of its reads was completed.
 */
class WmfReadCount final : public Microsoft::WRL::RuntimeClass<
      Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IUnknown> {
public:
  explicit WmfReadCount(const ULONG count) noexcept : count_(count) {}

  ~WmfReadCount() override = default;

  [[nodiscard]] ULONG count() const { return count_; }

private:
  ULONG count_;
};

} // namespace anonymous

WmfExtractorByteStream::WmfExtractorByteStream(AVMediaExtractorInput& input) noexcept
  : input_(input) {
}

HRESULT STDMETHODCALLTYPE WmfExtractorByteStream::GetCapabilities(DWORD* capabilities) {
  if (capabilities == nullptr) {
    return E_POINTER;
  }
  // The media of the reader is read and seeked, and it is never written: the reader owns it.
  *capabilities = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_SEEKABLE;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE WmfExtractorByteStream::GetLength(QWORD* length) {
  if (length == nullptr) {
    return E_POINTER;
  }
  const long inputLength = input_.getLength();
  if (inputLength < 0) {
    // The reader does not report the length of the media, e.g. the response of a server that does not
    // report one, so the stream cannot report it either: a parser that requires it reports that itself.
    return E_NOTIMPL;
  }
  *length = static_cast<QWORD>(inputLength);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE WmfExtractorByteStream::SetLength(QWORD length) {
  static_cast<void>(length);
  return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE WmfExtractorByteStream::GetCurrentPosition(QWORD* position) {
  if (position == nullptr) {
    return E_POINTER;
  }
  *position = static_cast<QWORD>(input_.getPosition());
  return S_OK;
}

HRESULT STDMETHODCALLTYPE WmfExtractorByteStream::SetCurrentPosition(QWORD position) {
  const bool moved = input_.setPosition(static_cast<long>(position), true);
  return moved ? S_OK : E_FAIL;
}

HRESULT STDMETHODCALLTYPE WmfExtractorByteStream::IsEndOfStream(BOOL* endOfStream) {
  if (endOfStream == nullptr) {
    return E_POINTER;
  }
  *endOfStream = input_.isAtEnd() ? TRUE : FALSE;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE WmfExtractorByteStream::Read(BYTE* buffer, ULONG size, ULONG* readCount) {
  if (readCount == nullptr) {
    return E_POINTER;
  }
  *readCount = 0;
  if (size == 0) {
    return S_OK;
  }
  if (buffer == nullptr) {
    return E_POINTER;
  }
  // A byte stream has to fill the request, because a read that returns fewer bytes than it was asked for
  // is how it reports the end of the media: the input of the reader serves a read from its read ahead
  // buffer, so it returns what it holds and the rest is read here.
  ULONG total = 0;
  while (total < size) {
    const long read = input_.read(buffer + total, 0, size - total);
    if (read == AVMediaExtractorInput::kEndOfInput || read <= 0) {
      break;
    }
    total += static_cast<ULONG>(read);
  }
  *readCount = total;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE WmfExtractorByteStream::BeginRead(BYTE* buffer, ULONG size,
    IMFAsyncCallback* callback, IUnknown* state) {
  if (callback == nullptr) {
    return E_POINTER;
  }
  // The media of the reader is read here, but the callback of the read is invoked by the work queue of
  // the Media Foundation rather than from this call: a byte stream handler that is completed from
  // inside BeginRead is re-entered before it finished the call that started the read, which it does not
  // expect (the mkv handler of the platform crashes on it).
  ULONG readCount = 0;
  const HRESULT result = Read(buffer, size, &readCount);
  if (FAILED(result)) {
    return result;
  }
  auto readCountObject = Microsoft::WRL::Make<WmfReadCount>(readCount);
  if (readCountObject == nullptr) {
    return E_OUTOFMEMORY;
  }
  Microsoft::WRL::ComPtr<IMFAsyncResult> asyncResult;
  const HRESULT createResult =
      ::MFCreateAsyncResult(readCountObject.Get(), callback, state, &asyncResult);
  if (FAILED(createResult)) {
    return createResult;
  }
  return ::MFPutWorkItemEx(MFASYNC_CALLBACK_QUEUE_STANDARD, asyncResult.Get());
}

HRESULT STDMETHODCALLTYPE WmfExtractorByteStream::EndRead(IMFAsyncResult* result, ULONG* readCount) {
  if (readCount == nullptr) {
    return E_POINTER;
  }
  *readCount = 0;
  if (result == nullptr) {
    return E_POINTER;
  }
  Microsoft::WRL::ComPtr<IUnknown> readCountObject;
  if (FAILED(result->GetObject(&readCountObject)) || readCountObject == nullptr) {
    return E_FAIL;
  }
  *readCount = static_cast<WmfReadCount*>(readCountObject.Get())->count();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE WmfExtractorByteStream::Write(const BYTE* buffer, ULONG size,
    ULONG* writtenCount) {
  static_cast<void>(buffer);
  static_cast<void>(size);
  static_cast<void>(writtenCount);
  return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE WmfExtractorByteStream::BeginWrite(const BYTE* buffer, ULONG size,
    IMFAsyncCallback* callback, IUnknown* state) {
  static_cast<void>(buffer);
  static_cast<void>(size);
  static_cast<void>(callback);
  static_cast<void>(state);
  return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE WmfExtractorByteStream::EndWrite(IMFAsyncResult* result,
    ULONG* writtenCount) {
  static_cast<void>(result);
  static_cast<void>(writtenCount);
  return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE WmfExtractorByteStream::Seek(MFBYTESTREAM_SEEK_ORIGIN origin,
    LONGLONG offset, DWORD seekFlags, QWORD* currentPosition) {
  static_cast<void>(seekFlags);
  LONGLONG target = offset;
  if (origin == msoCurrent) {
    target += input_.getPosition();
  }
  if (target < 0) {
    return E_INVALIDARG;
  }
  if (!input_.setPosition(static_cast<long>(target), true)) {
    return E_FAIL;
  }
  if (currentPosition != nullptr) {
    *currentPosition = static_cast<QWORD>(input_.getPosition());
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE WmfExtractorByteStream::Flush() {
  // Every read reaches the media of the reader, so there is nothing to flush.
  return S_OK;
}

HRESULT STDMETHODCALLTYPE WmfExtractorByteStream::Close() {
  // The stream does not own the input, so closing it only stops reading it: the reader releases the
  // media when it releases the extractor.
  return S_OK;
}

} // namespace coral
