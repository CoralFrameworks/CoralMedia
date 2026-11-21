/*
 * Copyright (C) 2022 The Coral Project
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
#ifndef CORAL_MEDIA_CRYPTO_INFO_H
#define CORAL_MEDIA_CRYPTO_INFO_H

#include <cstdint>
#include <vector>

namespace coral {

/**
 * Crypto modes for a codec. One of {@link CRYPTO_MODE_UNENCRYPTED}, {@link CRYPTO_MODE_AES_CTR}
 * or {@link CRYPTO_MODE_AES_CBC}.
 */
enum class CryptoMode : uint8_t {
  CRYPTO_MODE_UNENCRYPTED = 0,
  CRYPTO_MODE_AES_CTR = 1,
  CRYPTO_MODE_AES_CBC = 2,
};

/**
 * Metadata describing the structure of an encrypted input sample.
 */
class CryptoInfo {
private:
  /**
   * The 16 byte initialization vector. If the initialization vector of the content is shorter than
   * 16 bytes, 0 byte padding is appended to extend the vector to the required 16 byte length.
   */
  std::vector<uint8_t> ivBuffer;
  /**
   * The 16 byte key id.
   */
  std::vector<uint8_t> keyBuffer;

  /**
   * The type of encryption that has been applied. Must be one of the {@link CryptoMode} values.
   */
  CryptoMode cryptoMode = CryptoMode::CRYPTO_MODE_UNENCRYPTED;

  /**
   * The number of leading unencrypted bytes in each subsample. If null, all bytes are treated as
   * encrypted and {@link #numBytesOfEncryptedData} must be specified.
   */
  std::vector<int> numBytesOfClearData;

  /**
   * The number of trailing encrypted bytes in each subsample. If null, all bytes are treated as
   * clear and {@link numBytesOfClearData} must be specified.
   */
  std::vector<int> numBytesOfEncryptedData;

public:
  /**
   * Returns the 16 byte initialization vector.
   */
  [[nodiscard]] const uint8_t* iv() const {
    return ivBuffer.data();
  }

  /**
   * Returns the 16 byte key id.
   */
  [[nodiscard]] const uint8_t* key() const {
    return keyBuffer.data();
  }

  /**
   * Returns the type of encryption that has been applied
   */
  [[nodiscard]] CryptoMode mode() const {
    return cryptoMode;
  }
};

} // namespace coral

#endif //CORAL_MEDIA_CRYPTO_INFO_H
