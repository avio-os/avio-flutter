// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BOUNDED_RESOURCE_OWNERS_H_
#define FLUTTER_IMPELLER_RENDERER_BOUNDED_RESOURCE_OWNERS_H_

#include <array>
#include <memory>
#include <utility>

namespace impeller {

// Inline custody for the added generic/pipeline owners. Existing native
// texture/buffer tracking remains its own authority. Overflow poisons this
// batch before submission; no replacement vector, wait or heap fallback.
class BoundedResourceOwners {
 public:
  static constexpr size_t kCapacity = 512;
  BoundedResourceOwners() = default;
  BoundedResourceOwners(const BoundedResourceOwners&) = delete;
  BoundedResourceOwners& operator=(const BoundedResourceOwners&) = delete;
  BoundedResourceOwners(BoundedResourceOwners&& other) noexcept {
    *this = std::move(other);
  }
  BoundedResourceOwners& operator=(BoundedResourceOwners&& other) noexcept {
    if (this != &other) {
      Clear();
      valid_ = other.valid_;
      count_ = std::exchange(other.count_, 0);
      for (size_t i = 0; i < count_; i++) {
        owners_[i] = std::move(other.owners_[i]);
      }
      other.valid_ = true;
    }
    return *this;
  }
  bool Retain(std::shared_ptr<void> owner) {
    if (!valid_) {
      return false;
    }
    if (!owner) {
      return true;
    }
    for (size_t i = 0; i < count_; i++) {
      if (owners_[i] == owner) {
        return true;
      }
    }
    if (count_ == owners_.size()) {
      valid_ = false;
      return false;
    }
    owners_[count_++] = std::move(owner);
    return true;
  }
  bool IsValid() const { return valid_; }
  size_t size() const { return count_; }
  void Clear() {
    while (count_) {
      owners_[--count_].reset();
    }
  }

 private:
  std::array<std::shared_ptr<void>, kCapacity> owners_;
  size_t count_ = 0;
  bool valid_ = true;
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_RENDERER_BOUNDED_RESOURCE_OWNERS_H_
