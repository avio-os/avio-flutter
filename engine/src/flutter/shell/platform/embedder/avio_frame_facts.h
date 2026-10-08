// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_AVIO_FRAME_FACTS_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_AVIO_FRAME_FACTS_H_

#include "flutter/flow/avio_frame_facts.h"
#include "flutter/shell/platform/embedder/embedder.h"

namespace flutter {

inline const char* ValidateAvioFrameFactFeatures(
    FlutterAvioExtensionFeatures features) {
  constexpr auto kFacts = kFlutterAvioExtensionFeatureEmptyFrame |
                          kFlutterAvioExtensionFeatureItemEffects |
                          kFlutterAvioExtensionFeatureOutputGround |
                          kFlutterAvioExtensionFeatureReadyContent |
                          kFlutterAvioExtensionFeatureHitRegions;
  constexpr auto kRequired =
      kFlutterAvioExtensionFeatureRootRenderTarget |
      kFlutterAvioExtensionFeatureFrameOpportunityOutcomes;
  if ((features & kFacts) != 0 && (features & kRequired) != kRequired) {
    return "Root frame facts require exact root-target frame opportunities.";
  }
  // Only the empty-frame facts preroll collects hit regions, over the whole
  // frame and before a target is acquired.
  if ((features & kFlutterAvioExtensionFeatureHitRegions) != 0 &&
      (features & kFlutterAvioExtensionFeatureEmptyFrame) == 0) {
    return "Hit regions require empty frames.";
  }
  return nullptr;
}

// Allocation-free borrowed transport; these descriptors live only for the
// enclosing callback and cannot escape the exact revision which owns them.
class EmbedderAvioFrameFacts {
 public:
  explicit EmbedderAvioFrameFacts(const AvioFrameFacts& facts)
      : effect_{sizeof(FlutterAvioItemEffect), facts.item_opacity.value_or(1.0),
                facts.item_effect_declaration_id},
        ground_{sizeof(FlutterAvioOutputGround),
                facts.ground_color_argb.has_value(),
                facts.ground_color_argb.value_or(0u),
                facts.IsValid() ? facts.ground_regions_count : 0u, nullptr},
        ready_content_{
            sizeof(FlutterAvioReadyContent),
            facts.ready_content_revision.value_or(0u),
            static_cast<FlutterAvioReadyContentKind>(facts.ready_content_kind)},
        has_ready_content_(facts.IsValid() &&
                           facts.ready_content_revision.has_value()),
        has_effect_(facts.IsValid() && facts.item_opacity.has_value()),
        has_ground_(facts.IsValid() && facts.ground_authored) {
    static_assert(AvioFrameFacts::kMaxGroundRegions ==
                  FLUTTER_AVIO_MAX_OUTPUT_GROUND_REGIONS);
    for (size_t i = 0; i < ground_.regions_count; ++i) {
      const auto& r = facts.ground_regions[i];
      ground_regions_[i] = {sizeof(FlutterAvioOutputGroundRegion),
                            {r.left, r.top, r.right, r.bottom},
                            r.color_argb};
    }
    ground_.regions = ground_.regions_count ? ground_regions_.data() : nullptr;
  }
  EmbedderAvioFrameFacts(const EmbedderAvioFrameFacts&) = delete;
  EmbedderAvioFrameFacts& operator=(const EmbedderAvioFrameFacts&) = delete;

  const FlutterAvioItemEffect* effect() const {
    return has_effect_ ? &effect_ : nullptr;
  }
  const FlutterAvioOutputGround* ground() const {
    return has_ground_ ? &ground_ : nullptr;
  }

  const FlutterAvioReadyContent* ready_content() const {
    return has_ready_content_ ? &ready_content_ : nullptr;
  }

 private:
  std::array<FlutterAvioOutputGroundRegion,
             FLUTTER_AVIO_MAX_OUTPUT_GROUND_REGIONS>
      ground_regions_{};
  FlutterAvioItemEffect effect_;
  FlutterAvioOutputGround ground_;
  FlutterAvioReadyContent ready_content_;
  bool has_ready_content_;
  bool has_effect_;
  bool has_ground_;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_AVIO_FRAME_FACTS_H_
