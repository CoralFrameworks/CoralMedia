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

#ifndef CORAL_CODEC_ERROR_H
#define CORAL_CODEC_ERROR_H

#include <sstream>
#include <string>

namespace coral {

enum ErrCode : int32_t {
  kUnknown = (-2147483647 - 1), // INT32_MIN value
  kNoErr = 0,
  kNotInit = 1,
  kInvalidArgument = 2,
};

/**
 * Information about an error condition including a domain, a domain-specific
 * error code, and application-specific information.
 */
class Error {
public:
  //!< The error code
  ErrCode code;
  //!< A string containing the error domain.
  std::string domain;
  //!< A string containing the explanation of the reason for the error.
  std::string failureReason;
  //!< A string containing the description of the error.
  std::string description;

  Error() : code(ErrCode::kNoErr) {}

  explicit Error(std::string failureReason) noexcept
      : Error(ErrCode::kUnknown, "", std::move(failureReason), "") {}

  explicit Error(const ErrCode code, std::string failureReason) noexcept
      : Error(code, "", std::move(failureReason), "") {}

  explicit Error(const ErrCode code, std::string failureReason, std::string description) noexcept
      : Error(code, "", std::move(failureReason), std::move(description)) {}

  explicit Error(const ErrCode code,
      std::string domain,
      std::string failureReason,
      std::string description) noexcept
      : code(code),
        domain(std::move(domain)),
        failureReason(std::move(failureReason)),
        description(std::move(description)) {}

  Error(const Error& other) = default;
  Error& operator=(const Error& other) = default;

  void clear() {
    code = ErrCode::kNoErr;
    failureReason.clear();
    description.clear();
  }

  [[nodiscard]] bool empty() const {
    return domain.empty() && failureReason.empty() && description.empty();
  }

  [[nodiscard]] std::string toString() const {
    std::stringstream out;
    out << "{ code=" << static_cast<int32_t>(code)
        << ", domain=" << domain
        << ", failureReason=" << failureReason
        << ", description=" << description
        << "}";
    return out.str();
  }
};

} // namespace coral

#endif //CORAL_CODEC_ERROR_H
