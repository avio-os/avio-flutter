// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_DISPLAY_LIST_BACKDROP_FILTER_SUPPORT_H_
#define FLUTTER_IMPELLER_DISPLAY_LIST_BACKDROP_FILTER_SUPPORT_H_

#include "flutter/display_list/effects/dl_image_filters.h"

namespace impeller {

// Device bounds alone do not prove finite sampling support. Repeating blur
// depends on the original texture period; arbitrary runtime code can sample
// outside a geometric bound. Preserve the complete logical prefix in these
// cases, including when nested inside a composed/local-matrix filter.
inline bool BackdropFilterRequiresFullInput(
    const flutter::DlImageFilter* filter) {
  if (!filter) {
    return false;
  }
  switch (filter->type()) {
    case flutter::DlImageFilterType::kBlur: {
      const auto mode = filter->asBlur()->tile_mode();
      return mode == flutter::DlTileMode::kRepeat ||
             mode == flutter::DlTileMode::kMirror;
    }
    case flutter::DlImageFilterType::kRuntimeEffect:
      return true;
    case flutter::DlImageFilterType::kCompose:
      return BackdropFilterRequiresFullInput(
                 filter->asCompose()->inner().get()) ||
             BackdropFilterRequiresFullInput(
                 filter->asCompose()->outer().get());
    case flutter::DlImageFilterType::kLocalMatrix:
      return BackdropFilterRequiresFullInput(
          filter->asLocalMatrix()->image_filter().get());
    case flutter::DlImageFilterType::kMatrix: {
      const auto sampling = filter->asMatrix()->sampling();
      return sampling == flutter::DlImageSampling::kMipmapLinear ||
             sampling == flutter::DlImageSampling::kCubic;
    }
    case flutter::DlImageFilterType::kDilate:
    case flutter::DlImageFilterType::kErode:
    case flutter::DlImageFilterType::kColorFilter:
      return false;
  }
  return true;
}

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_DISPLAY_LIST_BACKDROP_FILTER_SUPPORT_H_
