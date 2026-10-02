// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_AVIO_REGION_PACKING_H_
#define FLUTTER_IMPELLER_ENTITY_AVIO_REGION_PACKING_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace impeller {

// CPU bookkeeping for a fixed attachment page. No allocation, growth, texture
// sizing fiction, or GPU-completion inference occurs here. A coverage owner
// encodes/composites a batch before releasing its logical readers and Reset().
// A layer owner uses stack=true; out-of-order releases wait for the stack top.
class AvioRegionPacking {
 public:
  static constexpr size_t kMaximumAllocations = 512u;

  struct Rect {
    int64_t x = 0;
    int64_t y = 0;
    int64_t width = 0;
    int64_t height = 0;
    constexpr bool operator==(const Rect&) const = default;
  };
  struct Token {
    uint64_t generation = 0u;
    size_t index = 0u;
    uint64_t serial = 0u;
    constexpr bool operator==(const Token&) const = default;
  };
  struct Allocation {
    Token token;
    Rect area;     // Includes the cleared sampling gutter.
    Rect content;  // The requested logical extent, at its physical page offset.
  };
  enum class Status { kSuccess, kInvalid, kNeedsTiling, kNeedsFlush };
  struct Result {
    Status status = Status::kInvalid;
    std::optional<Allocation> allocation;
  };

  AvioRegionPacking(int64_t width, int64_t height, bool stack = false);
  Result Allocate(int64_t width, int64_t height);
  bool Release(Token token);
  bool IsLive(Token token) const;
  // Refuses to overwrite a region that still has a logical reader.
  bool Reset();
  // The frame owner closes all logical lifetimes after encoding that frame.
  // Texture lifetimes of GPU commands are independent of these tokens.
  void EndFrame();
  size_t LiveCount() const { return live_count_; }
  Rect MaximumContent() const;
  static constexpr int64_t kAlignment = 2;
  static constexpr int64_t kGutter = 2;

 private:
  struct Cursor {
    int64_t x = 0;
    int64_t y = 0;
    int64_t row_height = 0;
  };
  struct Entry {
    bool live = false;
    Cursor previous;
    uint64_t serial = 0u;
  };
  void Clear();

  int64_t width_;
  int64_t height_;
  bool stack_;
  uint64_t generation_ = 1u;
  uint64_t next_serial_ = 1u;
  Cursor cursor_;
  std::array<Entry, kMaximumAllocations> entries_ = {};
  size_t count_ = 0u;
  size_t live_count_ = 0u;
};

// Iterates a large physical path in finite, non-overlapping tiles. Rendering
// a tile may flush/reuse a fixed coverage page, but never grows that page.
class AvioRegionTiles {
 public:
  AvioRegionTiles(AvioRegionPacking::Rect bounds,
                  int64_t tile_width,
                  int64_t tile_height);
  std::optional<AvioRegionPacking::Rect> Next();

 private:
  AvioRegionPacking::Rect bounds_;
  int64_t tile_width_;
  int64_t tile_height_;
  int64_t x_ = 0;
  int64_t y_ = 0;
  bool valid_ = false;
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_AVIO_REGION_PACKING_H_
