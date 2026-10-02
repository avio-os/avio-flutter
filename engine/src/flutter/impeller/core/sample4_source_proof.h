// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_CORE_SAMPLE4_SOURCE_PROOF_H_
#define FLUTTER_IMPELLER_CORE_SAMPLE4_SOURCE_PROOF_H_

#include <cstdint>

namespace impeller {

// Source-owned evidence for one recorded draw. This augments the immutable
// clip declaration's geometry proof; it never overrides its independent
// painter-order proof that earlier/later work does not require these lanes.
// In particular, texture opacity alone is not evidence of uniform shading or
// of full geometry. Captured sources must establish all three independently.
struct AvioSample4SourceProof {
  uint64_t segment_token = 0;
  bool uniform_samples = false;
  bool full_clip_geometry = false;
  bool source_is_opaque = false;
  // Source-side proof of an already encoded attachment value (or an exact
  // endpoint), independent of alpha. Raw bilinear/F16 shader colour is not it.
  bool source_is_encoded = false;
  bool captured_backdrop = false;

  bool Matches(uint64_t token) const {
    return token != 0 && segment_token == token && uniform_samples &&
           full_clip_geometry;
  }
};

// RGBA8 painter rounding commutes with a transparent fringe group only when
// its first source overwrites the covered lanes. An opaque first source
// establishes that closure; later uniform SrcOver sources preserve it. A
// translucent first source keeps the original destination in every lane and
// must use the native4 fringe instead.
class AvioSample4OpaqueGroupProof {
 public:
  void Add(bool src_over, bool source_is_opaque) {
    if (empty_) {
      valid_ = src_over && source_is_opaque;
      empty_ = false;
    } else {
      valid_ = valid_ && src_over;
    }
  }

  bool IsValid() const { return !empty_ && valid_; }

 private:
  bool empty_ = true;
  bool valid_ = false;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_CORE_SAMPLE4_SOURCE_PROOF_H_
