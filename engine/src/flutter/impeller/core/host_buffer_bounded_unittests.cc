// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/core/host_buffer.h"

#include <cstring>
#include <limits>

#include "gtest/gtest.h"
#include "impeller/core/device_buffer.h"

namespace impeller {
namespace {
constexpr size_t kBlockSize = 1024000;
class MappedBuffer final : public DeviceBuffer {
 public:
  explicit MappedBuffer(DeviceBufferDescriptor desc)
      : DeviceBuffer(desc), bytes_(desc.size) {}
  uint8_t* OnGetContents() const override { return bytes_.data(); }
  bool SetLabel(std::string_view) override { return true; }
  bool SetLabel(std::string_view, Range) override { return true; }
  bool OnCopyHostBuffer(const uint8_t* source,
                        Range range,
                        size_t offset) override {
    std::memcpy(bytes_.data() + offset, source + range.offset, range.length);
    return true;
  }

 private:
  mutable std::vector<uint8_t> bytes_;
};
class CountingAllocator final : public Allocator {
 public:
  size_t calls = 0;
  size_t fail_on = 0;
  ISize GetMaxTextureSizeSupported() const override { return {4096, 4096}; }
  std::shared_ptr<DeviceBuffer> OnCreateBuffer(
      const DeviceBufferDescriptor& desc) override {
    ++calls;
    return fail_on == calls ? nullptr : std::make_shared<MappedBuffer>(desc);
  }
  std::shared_ptr<Texture> OnCreateTexture(const TextureDescriptor&,
                                           bool) override {
    return nullptr;
  }
};
TEST(HostBufferBoundedTest, ColdCapacityAndWarmReuseNeverAllocate) {
  auto allocator = std::make_shared<CountingAllocator>();
  auto arena = HostBuffer::CreateBounded(allocator, nullptr, 256, 2);
  ASSERT_TRUE(arena);
  EXPECT_EQ(allocator->calls, 8u);
  for (size_t frame = 0; frame < 32; ++frame) {
    {
      auto first = arena->Emplace(kBlockSize, 16,
                                  [](uint8_t* bytes) { bytes[0] = 0x5a; });
      auto second = arena->Emplace(kBlockSize, 16,
                                   [](uint8_t* bytes) { bytes[0] = 0xa5; });
      ASSERT_TRUE(first);
      ASSERT_TRUE(second);
      EXPECT_EQ(first.GetBuffer()->OnGetContents()[0], 0x5a);
      EXPECT_EQ(second.GetBuffer()->OnGetContents()[0], 0xa5);
    }
    arena->Reset();
    EXPECT_EQ(arena->GetStateForTest().total_buffer_count, 2u);
  }
  EXPECT_EQ(allocator->calls, 8u);
}
TEST(HostBufferBoundedTest, PendingAndSubmittedOwnersRejectWrappedArena) {
  auto allocator = std::make_shared<CountingAllocator>();
  auto arena = HostBuffer::CreateBounded(allocator, nullptr, 16, 1);
  const uint32_t value = 0x12345678;
  auto pending = arena->Emplace(value);
  ASSERT_TRUE(pending);
  const auto* identity = pending.GetBuffer();
  for (size_t i = 0; i < 4; ++i) {
    arena->Reset();
  }
  EXPECT_FALSE(arena->Emplace(value));
  auto submitted = pending.TakeBuffer();
  ASSERT_TRUE(submitted);
  EXPECT_EQ(submitted.get(), identity);
  for (size_t i = 0; i < 4; ++i) {
    arena->Reset();
  }
  EXPECT_FALSE(arena->Emplace(4, 16, [](uint8_t*) { FAIL(); }));
  submitted.reset();
  for (size_t i = 0; i < 4; ++i) {
    arena->Reset();
  }
  auto reused = arena->Emplace(value);
  ASSERT_TRUE(reused);
  EXPECT_EQ(reused.GetBuffer(), identity);
  EXPECT_EQ(allocator->calls, 4u);
}
TEST(HostBufferBoundedTest, CapacityFailureStaysTerminalUntilReset) {
  auto allocator = std::make_shared<CountingAllocator>();
  auto arena = HostBuffer::CreateBounded(allocator, nullptr, 16, 1);
  auto whole = arena->Emplace(kBlockSize, 16, [](uint8_t*) {});
  ASSERT_TRUE(whole);
  EXPECT_FALSE(arena->Emplace(uint32_t(1)));
  EXPECT_FALSE(arena->Emplace(uint32_t(2)));
  EXPECT_EQ(arena->GetStateForTest().current_buffer, 0u);
  EXPECT_EQ(allocator->calls, 4u);
  arena->Reset();
  EXPECT_TRUE(arena->Emplace(uint32_t(3)));
}
TEST(HostBufferBoundedTest, OversizedAndAlignmentRequestsNeverCreateOneOffs) {
  auto allocator = std::make_shared<CountingAllocator>();
  auto arena = HostBuffer::CreateBounded(allocator, nullptr, 16, 1);
  EXPECT_FALSE(arena->Emplace(kBlockSize + 1, 16, [](uint8_t*) { FAIL(); }));
  EXPECT_FALSE(arena->Emplace(nullptr, kBlockSize + 1, 16));
  arena->Reset();
  auto first = arena->Emplace(uint8_t(7));
  auto aligned = arena->Emplace(uint32_t(9), 64);
  ASSERT_TRUE(aligned);
  EXPECT_EQ(aligned.GetRange().offset, 64u);
  auto huge_align =
      arena->Emplace(uint32_t(10), std::numeric_limits<size_t>::max());
  EXPECT_FALSE(huge_align);
  EXPECT_EQ(allocator->calls, 4u);
}
TEST(HostBufferBoundedTest, ColdFailureReturnsNullAndValidatesCapacity) {
  auto allocator = std::make_shared<CountingAllocator>();
  allocator->fail_on = 3;
  EXPECT_FALSE(HostBuffer::CreateBounded(allocator, nullptr, 16, 2));
  EXPECT_EQ(allocator->calls, 3u);
  EXPECT_FALSE(HostBuffer::CreateBounded(allocator, nullptr, 16, 0));
  EXPECT_FALSE(HostBuffer::CreateBounded(allocator, nullptr, 16, 65));
  EXPECT_FALSE(HostBuffer::CreateBounded(nullptr, nullptr, 16, 1));
}
}  // namespace
}  // namespace impeller
