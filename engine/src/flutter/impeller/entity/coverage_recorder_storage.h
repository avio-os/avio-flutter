// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_COVERAGE_RECORDER_STORAGE_H_
#define FLUTTER_IMPELLER_ENTITY_COVERAGE_RECORDER_STORAGE_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
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

// Element-type independent census of one fixed vector, in elements and bytes.
// Capacity bytes are address space reserved inside the owner's allocation; an
// element's bytes are first written when it is constructed, so high-water bytes
// bound what the owner can have made resident (before page rounding).
struct CoverageFixedStorageUsage {
  uint64_t high_water_elements = 0;
  uint64_t capacity_bytes = 0;
  uint64_t high_water_bytes = 0;
};

// Fixed-capacity vector over uninitialized storage, with the semantics of
// C++26 std::inplace_vector except that a full vector refuses (returns false)
// instead of throwing. Elements are placement-constructed on append and
// destroyed explicitly by pop_back/resize/clear; element addresses never move.
// A cap is therefore address space, not a preallocation: unused capacity is
// never written, and the owner's demand-zero pages stay uncommitted.
//
// operator[] and data() never read the size. A reader may access an element
// constructed before it was handed to that reader while the single owner of
// mutation appends further elements (the recorder bank relies on this).
template <class T, size_t Capacity>
class CoverageFixedVector {
 public:
  using value_type = T;
  // User-provided, so a value-initializing owner (std::make_shared) does not
  // zero the storage.
  CoverageFixedVector() noexcept {}
  ~CoverageFixedVector() { clear(); }
  CoverageFixedVector(const CoverageFixedVector&) = delete;
  CoverageFixedVector& operator=(const CoverageFixedVector&) = delete;

  template <class... Args>
  bool emplace_back(Args&&... args) {
    if (size_ == Capacity) {
      return false;
    }
    ::new (static_cast<void*>(data() + size_)) T(std::forward<Args>(args)...);
    size_++;
    if (size_ > high_water_.load(std::memory_order_relaxed)) {
      high_water_.store(size_, std::memory_order_relaxed);
    }
    return true;
  }
  bool push_back(const T& value) { return emplace_back(value); }
  bool push_back(T&& value) { return emplace_back(std::move(value)); }
  void pop_back() { std::destroy_at(data() + --size_); }
  void clear() {
    while (size_) {
      pop_back();
    }
  }
  // Shrinking destroys the tail; growing value-initializes new elements.
  bool resize(size_t count) {
    if (count > Capacity) {
      return false;
    }
    while (size_ > count) {
      pop_back();
    }
    while (size_ < count) {
      emplace_back();
    }
    return true;
  }
  bool empty() const { return size_ == 0; }
  size_t size() const { return size_; }
  static constexpr size_t capacity() { return Capacity; }
  // Monotonic count of constructed slots, readable from any thread.
  size_t high_water() const {
    return high_water_.load(std::memory_order_relaxed);
  }
  void AccumulateUsage(CoverageFixedStorageUsage& usage) const {
    const uint64_t high_water_elements = high_water();
    usage.high_water_elements += high_water_elements;
    usage.capacity_bytes += Capacity * sizeof(T);
    usage.high_water_bytes += high_water_elements * sizeof(T);
  }

  T* data() { return reinterpret_cast<T*>(storage_); }
  const T* data() const { return reinterpret_cast<const T*>(storage_); }
  T& operator[](size_t i) { return data()[i]; }
  const T& operator[](size_t i) const { return data()[i]; }
  T& front() { return data()[0]; }
  const T& front() const { return data()[0]; }
  T& back() { return data()[size_ - 1]; }
  const T& back() const { return data()[size_ - 1]; }
  T* begin() { return data(); }
  T* end() { return data() + size_; }
  const T* begin() const { return data(); }
  const T* end() const { return data() + size_; }
  auto rbegin() { return std::make_reverse_iterator(end()); }
  auto rend() { return std::make_reverse_iterator(begin()); }
  auto rbegin() const { return std::make_reverse_iterator(end()); }
  auto rend() const { return std::make_reverse_iterator(begin()); }
  std::span<T> span() { return {data(), size_}; }
  std::span<const T> span() const { return {data(), size_}; }

 private:
  // The counters precede the storage, so constructing an empty vector writes
  // only its first cache line.
  size_t size_ = 0;
  std::atomic<size_t> high_water_ = 0;
  alignas(T) std::byte storage_[Capacity * sizeof(T)];
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_COVERAGE_RECORDER_STORAGE_H_
