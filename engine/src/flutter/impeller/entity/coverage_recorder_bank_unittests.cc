// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/coverage_recorder_bank.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "impeller/core/device_buffer.h"

namespace impeller {

struct CoverageRecorderBankTestPeer {
  using Sequence = CoverageTiledRenderPass::PacketSequence;
  static bool Record(CoverageRecorderStorage& storage,
                     Sequence& sequence,
                     uint32_t identity,
                     BufferView view = {},
                     size_t bindings = 0) {
    auto reservation =
        storage.ReserveDraw(sequence.size(), view ? 1 : 0, bindings);
    if (!reservation) {
      return false;
    }
    CoverageTiledRenderPass::DrawPacket packet;
    packet.stencil_reference = identity;
    std::array<BufferView, 1> vertices = {view};
    std::array<CoverageTiledRenderPass::Binding, 32> resources;
    for (size_t i = 0; i < bindings; i++) {
      CoverageTiledRenderPass::BufferBinding resource{};
      resource.view = view;
      resource.slot_name.emplace("exact_binding");
      resources[i] = std::move(resource);
    }
    reservation->Commit(sequence, std::move(packet),
                        {vertices.data(), view ? 1u : 0u},
                        {resources.data(), bindings});
    return true;
  }
};

namespace testing {
namespace {

// This payload only publishes the same final-owner fact as the real wrapper
// destructor. All admission, allocator, packet ranges and cleanup logic below
// are the production CoverageRecorderStorage implementation.
struct BankClaim {
  std::shared_ptr<CoverageRecorderStorage> storage;
  size_t control;
  BankClaim(std::shared_ptr<CoverageRecorderStorage> storage, size_t control)
      : storage(std::move(storage)), control(control) {}
  ~BankClaim() { storage->ReleaseRecorder(control); }
};

std::shared_ptr<BankClaim> Claim(
    const std::shared_ptr<CoverageRecorderStorage>& storage) {
  auto control = storage->ReserveControl();
  if (!control) {
    return nullptr;
  }
  return std::allocate_shared<BankClaim>(
      CoverageRecorderAllocator<BankClaim>(storage, *control), storage,
      *control);
}

class BankBuffer final : public DeviceBuffer {
 public:
  BankBuffer() : DeviceBuffer(DeviceBufferDescriptor{.size = 16}) {}
  bool SetLabel(std::string_view) override { return true; }
  bool SetLabel(std::string_view, Range) override { return true; }
  uint8_t* OnGetContents() const override { return nullptr; }

 private:
  bool OnCopyHostBuffer(const uint8_t*, Range, size_t) override {
    return false;
  }
};

// Resident pages of a fresh private mapping. Huge pages are disabled so one
// touched byte faults exactly one base page.
class FreshMapping {
 public:
  explicit FreshMapping(size_t bytes)
      : page_(static_cast<size_t>(sysconf(_SC_PAGESIZE))),
        bytes_((bytes + page_ - 1) / page_ * page_),
        base_(mmap(nullptr,
                   bytes_,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0)) {
    if (base_ != MAP_FAILED) {
      madvise(base_, bytes_, MADV_NOHUGEPAGE);
    }
  }
  ~FreshMapping() {
    if (base_ != MAP_FAILED) {
      munmap(base_, bytes_);
    }
  }
  bool IsValid() const { return base_ != MAP_FAILED; }
  void* get() const { return base_; }
  size_t pages() const { return bytes_ / page_; }
  size_t page_size() const { return page_; }
  size_t ResidentPages() const {
    std::vector<unsigned char> residency(pages());
    if (mincore(base_, bytes_, residency.data()) != 0) {
      return SIZE_MAX;
    }
    return std::count_if(residency.begin(), residency.end(),
                         [](unsigned char page) { return page & 1u; });
  }

 private:
  size_t page_;
  size_t bytes_;
  void* base_;
};

struct LifetimeProbe {
  static inline int live = 0;
  LifetimeProbe() { live++; }
  LifetimeProbe(const LifetimeProbe&) { live++; }
  ~LifetimeProbe() { live--; }
};

TEST(CoverageRecorderBankTest, FixedVectorFailsClosedAndDestroysExplicitly) {
  LifetimeProbe::live = 0;
  {
    CoverageFixedVector<LifetimeProbe, 4> values;
    EXPECT_EQ(LifetimeProbe::live, 0);
    for (int i = 0; i < 4; i++) {
      ASSERT_TRUE(values.emplace_back());
    }
    EXPECT_FALSE(values.emplace_back());
    EXPECT_FALSE(values.push_back(LifetimeProbe{}));
    EXPECT_FALSE(values.resize(5));
    EXPECT_EQ(values.size(), 4u);
    EXPECT_EQ(LifetimeProbe::live, 4);
    values.pop_back();
    EXPECT_EQ(LifetimeProbe::live, 3);
    ASSERT_TRUE(values.resize(1));
    EXPECT_EQ(LifetimeProbe::live, 1);
    ASSERT_TRUE(values.resize(2));
    EXPECT_EQ(LifetimeProbe::live, 2);
    values.clear();
    EXPECT_EQ(LifetimeProbe::live, 0);
    EXPECT_EQ(values.high_water(), 4u);
    ASSERT_TRUE(values.emplace_back());
  }
  EXPECT_EQ(LifetimeProbe::live, 0);
}

TEST(CoverageRecorderBankTest, CapsAreFixedAddressSpaceNotResidentMemory) {
  static_assert(decltype(CoverageRecorderStorage::controls)::capacity() == 64);
  static_assert(decltype(CoverageRecorderStorage::packets)::capacity() == 8192);
  static_assert(decltype(CoverageRecorderStorage::bindings)::capacity() ==
                32768);
  static_assert(decltype(CoverageRecorderStorage::vertices)::capacity() ==
                32768);
  static_assert(
      decltype(CoverageRecorderStorage::ordinal_indices)::capacity() == 256);
  FreshMapping mapping(sizeof(CoverageRecorderStorage));
  ASSERT_TRUE(mapping.IsValid());
  // The same value-initialization std::make_shared performs.
  auto* storage = ::new (mapping.get()) CoverageRecorderStorage();
  // One page per vector header plus the admission word.
  EXPECT_LE(mapping.ResidentPages(), 8u) << "of " << mapping.pages();
  auto control = storage->ReserveControl();
  ASSERT_TRUE(control);
  CoverageRecorderBankTestPeer::Sequence sequence;
  sequence.storage = storage;
  for (uint32_t i = 0; i < 64; i++) {
    ASSERT_TRUE(CoverageRecorderBankTestPeer::Record(*storage, sequence, i));
  }
  const size_t touched =
      storage->packets.high_water() * sizeof(CoverageRecorderStorage::Packet) +
      sizeof(CoverageRecorderStorage::Control) +
      sizeof(CoverageRecorderStorage::IndexChunk);
  EXPECT_LE(mapping.ResidentPages(), 8u + touched / mapping.page_size() + 3u)
      << "of " << mapping.pages();
  storage->~CoverageRecorderStorage();
}

TEST(CoverageRecorderBankTest, ReclaimIdleDestroysGenerationKeepsHighWater) {
  using Binding = decltype(CoverageRecorderStorage::bindings)::value_type;
  auto storage = CoverageTiledRenderPass::CreateStorage();
  CoverageFixedStorageUsage fresh;
  storage->AccumulateStorageUsage(fresh);
  EXPECT_EQ(fresh.high_water_elements, 0u);
  EXPECT_EQ(fresh.high_water_bytes, 0u);
  EXPECT_EQ(fresh.capacity_bytes,
            64 * sizeof(CoverageRecorderStorage::Control) +
                8192 * sizeof(CoverageRecorderStorage::Packet) +
                32768 * sizeof(Binding) + 32768 * sizeof(BufferView) +
                256 * sizeof(CoverageRecorderStorage::IndexChunk));
  auto owner = Claim(storage);
  CoverageRecorderBankTestPeer::Sequence sequence;
  sequence.storage = storage.get();
  auto buffer = std::make_shared<BankBuffer>();
  std::weak_ptr<BankBuffer> witness = buffer;
  for (uint32_t i = 0; i < 3; i++) {
    ASSERT_TRUE(CoverageRecorderBankTestPeer::Record(
        *storage, sequence, i, BufferView(buffer, Range{0, 16}), 2));
  }
  buffer.reset();
  EXPECT_FALSE(witness.expired());
  owner.reset();
  // The vertex views and buffer bindings were destroyed, not overwritten.
  EXPECT_TRUE(witness.expired());
  EXPECT_EQ(storage->packets.size(), 0u);
  EXPECT_EQ(storage->bindings.size(), 0u);
  EXPECT_EQ(storage->vertices.size(), 0u);
  EXPECT_EQ(storage->ordinal_indices.size(), 0u);
  CoverageFixedStorageUsage used;
  storage->AccumulateStorageUsage(used);
  EXPECT_EQ(used.capacity_bytes, fresh.capacity_bytes);
  EXPECT_EQ(used.high_water_elements, 1u + 3u + 6u + 3u + 1u);
  EXPECT_EQ(used.high_water_bytes,
            sizeof(CoverageRecorderStorage::Control) +
                3 * sizeof(CoverageRecorderStorage::Packet) +
                6 * sizeof(Binding) + 3 * sizeof(BufferView) +
                sizeof(CoverageRecorderStorage::IndexChunk));
  // The next generation reuses the lowest indices: no new high water.
  auto next = Claim(storage);
  CoverageRecorderBankTestPeer::Sequence replacement;
  replacement.storage = storage.get();
  ASSERT_TRUE(CoverageRecorderBankTestPeer::Record(*storage, replacement, 7));
  CoverageFixedStorageUsage reused;
  storage->AccumulateStorageUsage(reused);
  EXPECT_EQ(reused.high_water_elements, used.high_water_elements);
}

TEST(CoverageRecorderBankTest, WeakControlExhaustionNeverRevivesAnOldClaim) {
  auto storage = CoverageTiledRenderPass::CreateStorage();
  std::array<std::weak_ptr<BankClaim>, 64> weak;
  for (auto& old : weak) {
    auto claim = Claim(storage);
    ASSERT_TRUE(claim);
    old = claim;
    claim.reset();
    EXPECT_TRUE(old.expired());
  }
  EXPECT_FALSE(Claim(storage));
  weak[17].reset();
  auto replacement = Claim(storage);
  ASSERT_TRUE(replacement);
  for (const auto& old : weak) {
    EXPECT_FALSE(old.lock());
  }
}

TEST(CoverageRecorderBankTest, PacketCapacityAndCleanupUseTheExactOwnerCut) {
  auto storage = CoverageTiledRenderPass::CreateStorage();
  auto first = Claim(storage);
  auto reader = first;
  CoverageRecorderBankTestPeer::Sequence sequence;
  sequence.storage = storage.get();
  auto buffer = std::make_shared<BankBuffer>();
  std::weak_ptr<BankBuffer> witness = buffer;
  for (uint32_t i = 0; i < 8192; ++i) {
    ASSERT_TRUE(CoverageRecorderBankTestPeer::Record(
        *storage, sequence, i, BufferView(buffer, Range{0, 16})));
  }
  EXPECT_FALSE(CoverageRecorderBankTestPeer::Record(*storage, sequence, 9999));
  EXPECT_EQ(sequence.size(), 8192u);
  for (size_t i = 0; i < sequence.size(); i += 63) {
    EXPECT_EQ(sequence[i].stencil_reference, i);
  }
  buffer.reset();
  first.reset();
  EXPECT_FALSE(witness.expired());
  auto second = Claim(storage);
  ASSERT_TRUE(second);
  EXPECT_EQ(storage->packets.size(), 8192u);
  second.reset();
  reader.reset();
  EXPECT_TRUE(witness.expired());
  EXPECT_EQ(storage->packets.size(), 0u);
  auto fresh = Claim(storage);
  ASSERT_TRUE(fresh);
  CoverageRecorderBankTestPeer::Sequence replacement;
  replacement.storage = storage.get();
  EXPECT_TRUE(CoverageRecorderBankTestPeer::Record(*storage, replacement, 23));
  EXPECT_EQ(replacement[0].stencil_reference, 23u);
}

TEST(CoverageRecorderBankTest, InterleavedPassIndicesKeepImmutableRanges) {
  auto storage = CoverageTiledRenderPass::CreateStorage();
  std::array<std::shared_ptr<BankClaim>, 64> claims;
  std::array<CoverageRecorderBankTestPeer::Sequence, 64> sequences;
  for (size_t i = 0; i < claims.size(); i++) {
    claims[i] = Claim(storage);
    ASSERT_TRUE(claims[i]);
    sequences[i].storage = storage.get();
  }
  for (uint32_t ordinal = 0; ordinal < 128; ordinal++) {
    for (uint32_t i = 0; i < claims.size(); i++) {
      ASSERT_TRUE(CoverageRecorderBankTestPeer::Record(*storage, sequences[i],
                                                       ordinal * 100 + i));
    }
  }
  for (size_t i = 0; i < sequences.size(); i++) {
    for (size_t ordinal = 0; ordinal < sequences[i].size(); ordinal++) {
      EXPECT_EQ(sequences[i][ordinal].stencil_reference, ordinal * 100 + i);
    }
  }
  EXPECT_EQ(storage->packets.size(), 8192u);
  EXPECT_FALSE(
      CoverageRecorderBankTestPeer::Record(*storage, sequences[0], 9999));
}

TEST(CoverageRecorderBankTest, WholeBindingReservationRefusesBeforeAppend) {
  auto storage = CoverageTiledRenderPass::CreateStorage();
  auto claim = Claim(storage);
  CoverageRecorderBankTestPeer::Sequence sequence;
  sequence.storage = storage.get();
  auto buffer = std::make_shared<BankBuffer>();
  for (uint32_t i = 0; i < 1024; ++i) {
    ASSERT_TRUE(CoverageRecorderBankTestPeer::Record(
        *storage, sequence, i, BufferView(buffer, Range{0, 16}), 32));
  }
  EXPECT_EQ(storage->bindings.size(), 32768u);
  EXPECT_FALSE(CoverageRecorderBankTestPeer::Record(
      *storage, sequence, 1024, BufferView(buffer, Range{0, 16}), 1));
  EXPECT_EQ(storage->packets.size(), 1024u);
  EXPECT_EQ(storage->bindings.size(), 32768u);
  EXPECT_EQ(sequence.back().bindings.size(), 32u);
}

TEST(CoverageRecorderBankTest, BusyAdmissionRefusesWithoutWaitingOrMutation) {
  auto storage = CoverageTiledRenderPass::CreateStorage();
  auto claim = Claim(storage);
  ASSERT_TRUE(storage->TryLock());
  EXPECT_FALSE(Claim(storage));
  EXPECT_FALSE(storage->ReserveDraw(0, 1, 1));
  EXPECT_EQ(storage->packets.size(), 0u);
  storage->Unlock();
  EXPECT_TRUE(Claim(storage));
}

TEST(CoverageRecorderBankTest, FinalReleaseCompletesCleanupBeforeNextClaim) {
  auto storage = CoverageTiledRenderPass::CreateStorage();
  for (int turn = 0; turn < 128; turn++) {
    auto old = Claim(storage);
    ASSERT_TRUE(old);
    CoverageRecorderBankTestPeer::Sequence sequence;
    sequence.storage = storage.get();
    auto buffer = std::make_shared<BankBuffer>();
    std::weak_ptr<BankBuffer> witness = buffer;
    ASSERT_TRUE(CoverageRecorderBankTestPeer::Record(
        *storage, sequence, 1, BufferView(buffer, Range{0, 16})));
    buffer.reset();
    std::thread release([owner = std::move(old)]() mutable { owner.reset(); });
    release.join();
    auto fresh = Claim(storage);
    ASSERT_TRUE(fresh);
    EXPECT_EQ(storage->packets.size(), 0u);
    EXPECT_TRUE(witness.expired());
  }
}

TEST(CoverageRecorderBankTest, ReleaseDuringAdmissionDefersCleanupUntilUnlock) {
  auto storage = CoverageTiledRenderPass::CreateStorage();
  auto old = Claim(storage);
  CoverageRecorderBankTestPeer::Sequence sequence;
  sequence.storage = storage.get();
  auto buffer = std::make_shared<BankBuffer>();
  std::weak_ptr<BankBuffer> witness = buffer;
  ASSERT_TRUE(CoverageRecorderBankTestPeer::Record(
      *storage, sequence, 1, BufferView(buffer, Range{0, 16})));
  buffer.reset();
  ASSERT_TRUE(storage->TryLock());
  std::thread release([owner = std::move(old)]() mutable { owner.reset(); });
  release.join();
  EXPECT_EQ(storage->packets.size(), 1u);
  EXPECT_FALSE(witness.expired());
  EXPECT_FALSE(Claim(storage));
  storage->Unlock();
  EXPECT_EQ(storage->packets.size(), 0u);
  EXPECT_TRUE(witness.expired());
  EXPECT_TRUE(Claim(storage));
}

TEST(CoverageRecorderBankTest, WeakControlKeepsOnlyCpuStorageAfterLastOwner) {
  auto storage = CoverageTiledRenderPass::CreateStorage();
  std::weak_ptr<CoverageRecorderStorage> bank = storage;
  auto owner = Claim(storage);
  std::weak_ptr<BankClaim> old = owner;
  CoverageRecorderBankTestPeer::Sequence sequence;
  sequence.storage = storage.get();
  auto buffer = std::make_shared<BankBuffer>();
  std::weak_ptr<BankBuffer> witness = buffer;
  ASSERT_TRUE(CoverageRecorderBankTestPeer::Record(
      *storage, sequence, 1, BufferView(buffer, Range{0, 16})));
  buffer.reset();
  owner.reset();
  storage.reset();
  EXPECT_TRUE(old.expired());
  EXPECT_TRUE(witness.expired());
  EXPECT_FALSE(bank.expired());
  old.reset();
  EXPECT_TRUE(bank.expired());
}

TEST(CoverageRecorderBankTest,
     Sequential256SegmentsReuseOnlyCpuStandingStorage) {
  auto storage = CoverageTiledRenderPass::CreateStorage();
  for (uint32_t segment = 0; segment < 256; ++segment) {
    auto owner = Claim(storage);
    ASSERT_TRUE(owner);
    CoverageRecorderBankTestPeer::Sequence sequence;
    sequence.storage = storage.get();
    ASSERT_TRUE(
        CoverageRecorderBankTestPeer::Record(*storage, sequence, segment));
    EXPECT_EQ(sequence[0].stencil_reference, segment);
    std::weak_ptr<BankClaim> old = owner;
    owner.reset();
    EXPECT_TRUE(old.expired());
    EXPECT_EQ(storage->packets.size(), 0u);
  }
}

}  // namespace
}  // namespace testing
}  // namespace impeller
