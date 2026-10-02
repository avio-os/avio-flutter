// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/avio_region_packing.h"

#include <algorithm>
#include <limits>

namespace impeller {

AvioRegionPacking::AvioRegionPacking(int64_t width, int64_t height, bool stack)
    : width_(width), height_(height), stack_(stack) {}

AvioRegionPacking::Rect AvioRegionPacking::MaximumContent() const {
  return {
      0, 0,
      width_ > 2 * kGutter ? width_ - width_ % kAlignment - 2 * kGutter : 0,
      height_ > 2 * kGutter ? height_ - height_ % kAlignment - 2 * kGutter : 0};
}

AvioRegionPacking::Result AvioRegionPacking::Allocate(int64_t width,
                                                      int64_t height) {
  if (width <= 0 || height <= 0 || generation_ == 0u || next_serial_ == 0u) {
    return {};
  }
  const auto maximum = MaximumContent();
  if (width > maximum.width || height > maximum.height) {
    return {Status::kNeedsTiling, std::nullopt};
  }
  if (count_ == entries_.size()) {
    return {Status::kNeedsFlush, std::nullopt};
  }
  // The extent checks above bound these additions by the fixed page extent.
  const auto align = [](int64_t value) {
    return value +
           (value % kAlignment == 0 ? 0 : kAlignment - value % kAlignment);
  };
  const int64_t padded_width = align(width + 2 * kGutter);
  const int64_t padded_height = align(height + 2 * kGutter);
  Cursor next = cursor_;
  if (padded_width > width_ - next.x) {
    next.x = 0;
    next.y += next.row_height;
    next.row_height = 0;
  }
  if (padded_height > height_ - next.y) {
    return {Status::kNeedsFlush, std::nullopt};
  }
  const size_t index = count_++;
  const uint64_t serial = next_serial_;
  next_serial_ =
      serial == std::numeric_limits<uint64_t>::max() ? 0u : serial + 1u;
  entries_[index] = {true, cursor_, serial};
  const Rect area = {next.x, next.y, padded_width, padded_height};
  next.x += padded_width;
  next.row_height = std::max(next.row_height, padded_height);
  cursor_ = next;
  live_count_++;
  return {Status::kSuccess,
          Allocation{Token{generation_, index, serial},
                     area,
                     {area.x + kGutter, area.y + kGutter, width, height}}};
}

bool AvioRegionPacking::IsLive(Token token) const {
  return token.generation == generation_ && token.index < count_ &&
         entries_[token.index].live &&
         entries_[token.index].serial == token.serial;
}

bool AvioRegionPacking::Release(Token token) {
  if (!IsLive(token)) {
    return false;
  }
  entries_[token.index].live = false;
  live_count_--;
  if (stack_) {
    while (count_ > 0u && !entries_[count_ - 1u].live) {
      cursor_ = entries_[count_ - 1u].previous;
      count_--;
    }
  }
  return true;
}

void AvioRegionPacking::Clear() {
  cursor_ = {};
  count_ = 0u;
  live_count_ = 0u;
  // Exhaustion fails future allocations instead of making stale tokens live.
  generation_ = generation_ == std::numeric_limits<uint64_t>::max()
                    ? 0u
                    : generation_ + 1u;
}

bool AvioRegionPacking::Reset() {
  if (live_count_ != 0u) {
    return false;
  }
  Clear();
  return true;
}

void AvioRegionPacking::EndFrame() {
  Clear();
}

AvioRegionTiles::AvioRegionTiles(AvioRegionPacking::Rect bounds,
                                 int64_t tile_width,
                                 int64_t tile_height)
    : bounds_(bounds), tile_width_(tile_width), tile_height_(tile_height) {
  // Offsets are checked before adding to a physical origin in Next().
  valid_ = bounds.width > 0 && bounds.height > 0 && tile_width > 0 &&
           tile_height > 0 &&
           bounds.x <= std::numeric_limits<int64_t>::max() - bounds.width &&
           bounds.y <= std::numeric_limits<int64_t>::max() - bounds.height;
}

std::optional<AvioRegionPacking::Rect> AvioRegionTiles::Next() {
  if (!valid_ || y_ >= bounds_.height) {
    return std::nullopt;
  }
  const int64_t width = std::min(tile_width_, bounds_.width - x_);
  const int64_t height = std::min(tile_height_, bounds_.height - y_);
  const AvioRegionPacking::Rect tile = {bounds_.x + x_, bounds_.y + y_, width,
                                        height};
  x_ += width;
  if (x_ == bounds_.width) {
    x_ = 0;
    y_ += height;
  }
  return tile;
}

}  // namespace impeller
