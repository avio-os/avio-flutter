// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_RESOURCE_CUSTODY_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_RESOURCE_CUSTODY_H_

#include <utility>

#include "flutter/fml/closure.h"

namespace flutter {

// Inline custody in the actual native texture source, not a second shared
// owner/control block. Arm only after render-target acquisition succeeds;
// before then the acquisition's ScopedCleanupClosure owns collection.
class EmbedderExternalResourceCustody {
 public:
  EmbedderExternalResourceCustody() = default;
  EmbedderExternalResourceCustody(const EmbedderExternalResourceCustody&) =
      delete;
  EmbedderExternalResourceCustody& operator=(
      const EmbedderExternalResourceCustody&) = delete;

  ~EmbedderExternalResourceCustody() {
    if (framebuffer_destruction_) {
      framebuffer_destruction_();
    }
    if (collect_) {
      collect_();
    }
  }

  bool Adopt(fml::closure framebuffer_destruction, fml::closure collect) {
    if (adopted_) {
      return false;
    }
    adopted_ = true;
    framebuffer_destruction_ = std::move(framebuffer_destruction);
    collect_ = std::move(collect);
    return true;
  }

 private:
  bool adopted_ = false;
  fml::closure framebuffer_destruction_;
  fml::closure collect_;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_RESOURCE_CUSTODY_H_
