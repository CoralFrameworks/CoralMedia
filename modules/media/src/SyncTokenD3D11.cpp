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

#if defined(_WIN32)

#include <media/SyncTokenD3D11.h>

#include <d3d11_3.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <chrono>
#include <thread>

namespace coral {

namespace {

class D3D11QuerySyncToken final : public SyncToken {
public:
  D3D11QuerySyncToken(Microsoft::WRL::ComPtr<ID3D11DeviceContext> context,
      Microsoft::WRL::ComPtr<ID3D11Query> query)
    : mContext(std::move(context)),
      mQuery(std::move(query)) {
  }

  bool wait(const uint64_t timeoutNanoseconds) override {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::nanoseconds(timeoutNanoseconds);
    for (;;) {
      if (isSignaled()) {
        return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }

  [[nodiscard]] bool isSignaled() const override {
    return mContext != nullptr && mQuery != nullptr &&
           mContext->GetData(mQuery.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
  }

private:
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> mContext;
  Microsoft::WRL::ComPtr<ID3D11Query> mQuery;
};

class D3D11FenceSyncToken final : public SyncToken {
public:
  D3D11FenceSyncToken(Microsoft::WRL::ComPtr<ID3D11Fence> fence, const uint64_t value,
      HANDLE sharedHandle = nullptr)
    : mFence(std::move(fence)),
      mValue(value),
      mSharedHandle(sharedHandle) {
  }

  ~D3D11FenceSyncToken() override {
    if (mSharedHandle != nullptr) {
      CloseHandle(mSharedHandle);
    }
  }

  bool wait(const uint64_t timeoutNanoseconds) override {
    if (mFence == nullptr) {
      return false;
    }
    const auto event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (event == nullptr) {
      return false;
    }
    const bool signalled = SUCCEEDED(mFence->SetEventOnCompletion(mValue, event));
    const auto milliseconds = timeoutNanoseconds == UINT64_MAX
                                ? INFINITE
                                : static_cast<DWORD>(std::min<uint64_t>(
                                    timeoutNanoseconds / 1000000ull, INFINITE - 1ull));
    const bool result = signalled && WaitForSingleObject(event, milliseconds) == WAIT_OBJECT_0;
    CloseHandle(event);
    return result;
  }

  [[nodiscard]] bool isSignaled() const override {
    return mFence != nullptr && mFence->GetCompletedValue() >= mValue;
  }

  bool nativeHandle(SyncTokenNativeHandle* out) const override {
    if (out == nullptr || mSharedHandle == nullptr) {
      return false;
    }
    out->kind = SyncTokenKind::D3D11Fence;
    out->handle = mSharedHandle;
    out->value = mValue;
    return true;
  }

private:
  Microsoft::WRL::ComPtr<ID3D11Fence> mFence;
  uint64_t mValue = 0;
  HANDLE mSharedHandle = nullptr;
};

} // namespace anonymous

std::shared_ptr<SyncToken> createD3D11QuerySyncTokenFromQuery(ID3D11DeviceContext* context,
    ID3D11Query* query) {
  if (context == nullptr || query == nullptr) {
    return nullptr;
  }
  return std::make_shared<D3D11QuerySyncToken>(context, query);
}

std::shared_ptr<SyncToken> createD3D11QuerySyncToken(ID3D11Device* device,
    ID3D11DeviceContext* context) {
  if (device == nullptr || context == nullptr) {
    return nullptr;
  }
  D3D11_QUERY_DESC desc{};
  desc.Query = D3D11_QUERY_EVENT;
  Microsoft::WRL::ComPtr<ID3D11Query> query;
  if (FAILED(device->CreateQuery(&desc, &query)) || query == nullptr) {
    return nullptr;
  }
  context->End(query.Get());
  // A query is only advanced by submitted work; flush the (possibly empty) command stream so the
  // event can complete even when the caller created the token outside a frame submission.
  context->Flush();
  return std::make_shared<D3D11QuerySyncToken>(context, std::move(query));
}

std::shared_ptr<SyncToken> createD3D11FenceSyncToken(ID3D11Device* device,
    ID3D11DeviceContext* context, const uint64_t value) {
  if (device == nullptr || context == nullptr) {
    return nullptr;
  }
  Microsoft::WRL::ComPtr<ID3D11Device5> device5;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))) || device5 == nullptr ||
      FAILED(context->QueryInterface(IID_PPV_ARGS(&context4))) || context4 == nullptr) {
    return createD3D11QuerySyncToken(device, context);
  }

  Microsoft::WRL::ComPtr<ID3D11Fence> fence;
  if (FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) ||
      fence == nullptr || FAILED(context4->Signal(fence.Get(), value))) {
    return createD3D11QuerySyncToken(device, context);
  }
  HANDLE sharedHandle = nullptr;
  static_cast<void>(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedHandle));
  return std::make_shared<D3D11FenceSyncToken>(std::move(fence), value, sharedHandle);
}

} // namespace coral

#endif // defined(_WIN32)
