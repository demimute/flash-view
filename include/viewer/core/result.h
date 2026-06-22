#pragma once

#include <string>
#include <utility>
#include <variant>

namespace viewer::core {

enum class ErrorCode {
  cancelled,
  invalid_format,
  unsupported_format,
  io_error,
  decode_error,
  resource_limit,
  platform_error,
};

struct Error {
  ErrorCode code;
  std::wstring message;
};

template <typename T>
class Result {
 public:
  static Result success(T value) {
    return Result(std::in_place_index<0>, std::move(value));
  }

  static Result failure(Error error) {
    return Result(std::in_place_index<1>, std::move(error));
  }

  [[nodiscard]] bool has_value() const noexcept {
    return storage_.index() == 0;
  }

  T& value() & { return std::get<0>(storage_); }
  const T& value() const& { return std::get<0>(storage_); }
  T&& value() && { return std::get<0>(std::move(storage_)); }

  Error& error() & { return std::get<1>(storage_); }
  const Error& error() const& { return std::get<1>(storage_); }
  Error&& error() && { return std::get<1>(std::move(storage_)); }

 private:
  template <typename... Args>
  explicit Result(std::in_place_index_t<0> index, Args&&... args)
      : storage_(index, std::forward<Args>(args)...) {}

  template <typename... Args>
  explicit Result(std::in_place_index_t<1> index, Args&&... args)
      : storage_(index, std::forward<Args>(args)...) {}

  std::variant<T, Error> storage_;
};

}  // namespace viewer::core
