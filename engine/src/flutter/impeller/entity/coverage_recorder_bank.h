// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_COVERAGE_RECORDER_BANK_H_
#define FLUTTER_IMPELLER_ENTITY_COVERAGE_RECORDER_BANK_H_

#include <atomic>
#include <cstddef>
#include <new>

#include "fml/logging.h"
#include "impeller/entity/coverage_tiled_render_pass.h"

namespace impeller {

// Aggregate cold CPU storage. A shared control slot is reused only after its
// last weak reference is destroyed; the contained pass is a fresh object each
// time. Packet ranges are immutable while any strong recorder owns this bank.
// Final releases publish cleanup facts through the admission word. Its owner
// drains those facts before unlocking, rather than exposing zero readers while
// cleanup is still running. Claims/records never wait for an admission owner.
//
// The caps are fixed-address uninitialized capacity inside the one standing
// allocation, never a preallocation: an element is constructed only while the
// admission word is owned, so resident memory is the touched high-water mark.
// ReclaimIdle destroys every generation element and later records reuse the
// lowest indices first. Readers index elements handed to them, never sizes.
class CoverageRecorderStorage {
 public:
  using Pass = CoverageTiledRenderPass;
  static constexpr size_t kControls = 64;
  static constexpr size_t kPackets = 8192;
  static constexpr size_t kBindings = 32768;
  static constexpr size_t kVertices = 32768;
  // At most 128 full pages plus 63 partial pages for 64 simultaneous passes.
  static constexpr size_t kIndexChunks = 256;
  static constexpr uint32_t kOwned = 1;
  static constexpr uint32_t kCleanup = 2;
  static constexpr size_t kControlBytes = sizeof(Pass) + 128;
  struct Control {
    // User-provided: constructing a control writes only its flags; the pass
    // constructor writes the bytes it actually uses.
    Control() {}
    alignas(std::max_align_t) std::array<std::byte, kControlBytes> bytes;
    std::atomic_bool occupied = false;
    std::atomic_bool live = false;
  };
  struct Packet {
    Pass::DrawPacket draw;
    uint32_t next = Pass::PacketSequence::kEnd;
  };
  using IndexChunk = std::array<uint32_t, 64>;

  // User-provided, so std::make_shared does not zero-initialize the bank.
  CoverageRecorderStorage() {}

  std::atomic<uint32_t> admission = 0;
  // Controls are constructed on first demand and never destroyed or moved
  // while the bank lives; allocators address them by index.
  CoverageFixedVector<Control, kControls> controls;
  CoverageFixedVector<Packet, kPackets> packets;
  CoverageFixedVector<Pass::Binding, kBindings> bindings;
  CoverageFixedVector<BufferView, kVertices> vertices;
  CoverageFixedVector<IndexChunk, kIndexChunks> ordinal_indices;

  void AccumulateStorageUsage(CoverageFixedStorageUsage& usage) const {
    controls.AccumulateUsage(usage);
    packets.AccumulateUsage(usage);
    bindings.AccumulateUsage(usage);
    vertices.AccumulateUsage(usage);
    ordinal_indices.AccumulateUsage(usage);
  }

  bool TryLock() {
    auto state = admission.load(std::memory_order_acquire);
    return !(state & kOwned) &&
           admission.compare_exchange_strong(state, state | kOwned,
                                             std::memory_order_acquire);
  }
  void ReclaimIdle() {
    for (auto& control : controls) {
      if (control.live.load(std::memory_order_acquire)) {
        return;
      }
    }
    // Native command buffers independently own every encoded pipeline,
    // texture and buffer. Weak pass readers cannot keep a packet generation.
    packets.clear();
    bindings.clear();
    vertices.clear();
    ordinal_indices.clear();
  }
  void Unlock() {
    for (;;) {
      if (admission.fetch_and(~kCleanup, std::memory_order_acq_rel) &
          kCleanup) {
        ReclaimIdle();
      }
      uint32_t state = kOwned;
      if (admission.compare_exchange_strong(state, 0,
                                            std::memory_order_release)) {
        return;
      }
      // This processes a newly published final-release fact, not a lock wait.
      // No claims enter while owned; at most the 64 live controls can release.
      FML_DCHECK(state == (kOwned | kCleanup));
    }
  }
  std::optional<size_t> ReserveControl() {
    if (!TryLock()) {
      return std::nullopt;
    }
    ReclaimIdle();
    std::optional<size_t> result;
    for (size_t i = 0; i < controls.size(); i++) {
      bool free = false;
      if (controls[i].occupied.compare_exchange_strong(
              free, true, std::memory_order_acquire)) {
        result = i;
        break;
      }
    }
    // Lowest free slot first; a new slot is constructed only when all
    // constructed ones are still weakly referenced.
    if (!result && controls.emplace_back()) {
      result = controls.size() - 1;
      controls[*result].occupied.store(true, std::memory_order_relaxed);
    }
    if (result) {
      controls[*result].live.store(true, std::memory_order_release);
    }
    Unlock();
    return result;
  }
  void ReleaseRecorder(size_t index) {
    controls[index].live.store(false, std::memory_order_release);
    admission.fetch_or(kCleanup, std::memory_order_acq_rel);
    if (TryLock()) {
      Unlock();
    }
  }
  bool HasRoom(size_t vertex_views, size_t resource_bindings) const {
    return packets.size() < packets.capacity() &&
           vertex_views <= vertices.capacity() - vertices.size() &&
           resource_bindings <= bindings.capacity() - bindings.size();
  }
  struct Reservation {
    CoverageRecorderStorage& storage;
    uint32_t index;
    size_t vertex_start;
    size_t binding_start;
    std::optional<uint16_t> index_chunk;
    void Commit(Pass::PacketSequence& sequence,
                Pass::DrawPacket&& draw,
                std::span<BufferView> vertex_views,
                std::span<Pass::Binding> resource_bindings) {
      for (size_t i = 0; i < vertex_views.size(); i++) {
        storage.vertices[vertex_start + i] = std::move(vertex_views[i]);
      }
      for (size_t i = 0; i < resource_bindings.size(); i++) {
        storage.bindings[binding_start + i] = std::move(resource_bindings[i]);
      }
      draw.vertices = {storage.vertices.data() + vertex_start,
                       vertex_views.size()};
      draw.bindings = {storage.bindings.data() + binding_start,
                       resource_bindings.size()};
      storage.packets[index].draw = std::move(draw);
      if (sequence.empty()) {
        sequence.head = index;
      } else {
        storage.packets[sequence.tail].next = index;
      }
      sequence.tail = index;
      const auto ordinal = sequence.count;
      if (index_chunk) {
        sequence.index_chunks[ordinal / 64] = *index_chunk;
      }
      storage
          .ordinal_indices[sequence.index_chunks[ordinal / 64]][ordinal % 64] =
          index;
      sequence.count++;
    }
  };
  std::optional<Reservation> ReserveDraw(size_t ordinal,
                                         size_t vertex_views,
                                         size_t resource_bindings) {
    if (!TryLock()) {
      return std::nullopt;
    }
    if (!HasRoom(vertex_views, resource_bindings) || ordinal >= kPackets ||
        (ordinal % 64 == 0 &&
         ordinal_indices.size() == ordinal_indices.capacity())) {
      Unlock();
      return std::nullopt;
    }
    // Construct the whole reservation while owned; Commit then only assigns
    // already-live elements, and the next link starts at kEnd.
    Reservation result{*this, static_cast<uint32_t>(packets.size()),
                       vertices.size(), bindings.size(), std::nullopt};
    packets.emplace_back();
    vertices.resize(vertices.size() + vertex_views);
    bindings.resize(bindings.size() + resource_bindings);
    if (ordinal % 64 == 0) {
      result.index_chunk = static_cast<uint16_t>(ordinal_indices.size());
      ordinal_indices.emplace_back();
    }
    Unlock();
    return result;
  }
};

template <class T>
struct CoverageRecorderAllocator {
  using value_type = T;
  std::shared_ptr<CoverageRecorderStorage> storage;
  size_t index;
  CoverageRecorderAllocator(std::shared_ptr<CoverageRecorderStorage> storage,
                            size_t index)
      : storage(std::move(storage)), index(index) {}
  template <class U>
  CoverageRecorderAllocator(const CoverageRecorderAllocator<U>& other)
      : storage(other.storage), index(other.index) {}
  T* allocate(size_t count) noexcept {
    static_assert(sizeof(T) <= CoverageRecorderStorage::kControlBytes);
    static_assert(alignof(T) <= alignof(std::max_align_t));
    FML_CHECK(count == 1 && storage->controls[index].occupied.load());
    return reinterpret_cast<T*>(storage->controls[index].bytes.data());
  }
  void deallocate(T*, size_t) noexcept {
    storage->controls[index].occupied.store(false, std::memory_order_release);
  }
  template <class U, class... Args>
  void construct(U* pointer, Args&&... args) {
    ::new (static_cast<void*>(pointer)) U(std::forward<Args>(args)...);
  }
  template <class U>
  bool operator==(const CoverageRecorderAllocator<U>& other) const {
    return storage == other.storage && index == other.index;
  }
};

}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_COVERAGE_RECORDER_BANK_H_
