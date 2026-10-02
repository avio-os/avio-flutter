// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_COVERAGE_RECORDER_STORAGE_H_
#define FLUTTER_IMPELLER_ENTITY_COVERAGE_RECORDER_STORAGE_H_

#include <array>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

namespace impeller {

// Diagnostic labels and slot names are copied into standing packet storage.
// Refuse an overlong binding name rather than changing a shader lookup key.
template <size_t Capacity>
class CoverageRecorderString {
 public:
  CoverageRecorderString() = default;
  explicit CoverageRecorderString(std::string_view value) { Assign(value); }
  bool Assign(std::string_view value) {
    if (value.size() >= bytes_.size()) {
      return false;
    }
    if (!value.empty()) {
      std::memcpy(bytes_.data(), value.data(), value.size());
    }
    size_ = value.size();
    bytes_[size_] = '\0';
    return true;
  }
  void AssignLabel(std::string_view value) {
    Assign(value.substr(0, Capacity - 1));
  }
  bool empty() const { return size_ == 0; }
  const char* c_str() const { return bytes_.data(); }
  operator const char*() const { return c_str(); }
  operator std::string_view() const { return {bytes_.data(), size_}; }
  bool operator==(const char* value) const {
    return std::string_view(*this) == value;
  }
  bool operator==(std::string_view value) const {
    return std::string_view(*this) == value;
  }

 private:
  std::array<char, Capacity> bytes_ = {};
  size_t size_ = 0;
};

template <class T, size_t Capacity>
class CoverageRecorderVector {
 public:
  bool push_back(T value) {
    if (size_ == Capacity) {
      return false;
    }
    values_[size_++] = std::move(value);
    return true;
  }
  template <class... Args>
  bool emplace_back(Args&&... args) {
    return push_back(T(std::forward<Args>(args)...));
  }
  void clear() {
    while (size_) {
      values_[--size_] = T{};
    }
  }
  size_t size() const { return size_; }
  static constexpr size_t capacity() { return Capacity; }
  T* data() { return values_.data(); }
  std::span<T> span() { return {values_.data(), size_}; }
  std::span<const T> span() const { return {values_.data(), size_}; }

 private:
  std::array<T, Capacity> values_ = {};
  size_t size_ = 0;
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_COVERAGE_RECORDER_STORAGE_H_
