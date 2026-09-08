// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_EXTERNAL_COVERAGE_CONTENTS_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_EXTERNAL_COVERAGE_CONTENTS_H_

#include "impeller/entity/contents/contents.h"

namespace impeller {

// Resolves geometric coverage before applying the external-backdrop transfer.
// The source must not have applied that transfer already. Unlike a color
// shader, this stage sees MSAA coverage and the final filtered source alpha.
class ExternalCoverageContents final : public Contents {
 public:
  explicit ExternalCoverageContents(std::shared_ptr<Contents> source);
  bool Render(const ContentContext& renderer,
              const Entity& entity,
              RenderPass& pass) const override;
  std::optional<Rect> GetCoverage(const Entity& entity) const override;
  void SetInheritedOpacity(Scalar opacity) override;

 private:
  std::shared_ptr<Contents> source_;
  Scalar opacity_ = 1.0f;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_EXTERNAL_COVERAGE_CONTENTS_H_
