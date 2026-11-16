// Minimal exception-free result type for core code.
//
// Core libraries avoid exceptions so the same logic stays usable in embedded
// builds later. C++20 has no std::expected, so this is a small stand-in.
#pragma once

#include <string>
#include <utility>
#include <variant>

namespace talos {

struct Error {
  std::string code;     // stable machine-readable identifier, e.g. "invalid_envelope"
  std::string message;  // human-readable detail
};

template <typename T>
class [[nodiscard]] Result {
 public:
  Result(T value) : v_(std::move(value)) {}      // NOLINT(google-explicit-constructor)
  Result(Error error) : v_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  bool ok() const { return std::holds_alternative<T>(v_); }
  explicit operator bool() const { return ok(); }

  const T& value() const& { return std::get<T>(v_); }
  T& value() & { return std::get<T>(v_); }
  T&& value() && { return std::get<T>(std::move(v_)); }
  const Error& error() const { return std::get<Error>(v_); }

 private:
  std::variant<T, Error> v_;
};

// Result for operations that produce no value.
class [[nodiscard]] Status {
 public:
  Status() = default;
  Status(Error error) : error_(std::move(error)), ok_(false) {}  // NOLINT

  static Status Ok() { return {}; }
  bool ok() const { return ok_; }
  explicit operator bool() const { return ok_; }
  const Error& error() const { return error_; }

 private:
  Error error_;
  bool ok_ = true;
};

}  // namespace talos
