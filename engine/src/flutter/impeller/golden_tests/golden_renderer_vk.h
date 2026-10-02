// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_GOLDEN_TESTS_GOLDEN_RENDERER_VK_H_
#define FLUTTER_IMPELLER_GOLDEN_TESTS_GOLDEN_RENDERER_VK_H_

#include <memory>
#include <vector>

#include "flutter/display_list/display_list.h"
#include "impeller/display_list/aiks_context.h"
#include "impeller/testing/screenshot.h"

namespace impeller::testing {

// Test-only policies. Aliased1 also forces nested offscreens to one sample;
// it does not negotiate an unsupported one-sample Coverage configuration.
enum class AvioGoldenPolicyVK { kMsaa4, kCoverage, kAliased1 };

std::shared_ptr<Context> MakeHeadlessGoldenContextVK(AvioGoldenPolicyVK policy,
                                                     bool use_sdfs = false);
std::shared_ptr<RenderTargetAllocator> MakeGoldenAllocatorVK(
    const std::shared_ptr<Context>& context,
    AvioGoldenPolicyVK policy);
std::shared_ptr<Texture> RenderGoldenDisplayListVK(
    AiksContext& renderer,
    const sk_sp<flutter::DisplayList>& list,
    ISize size,
    AvioGoldenPolicyVK policy);
std::unique_ptr<Screenshot> ReadGoldenTextureVK(
    const std::shared_ptr<Context>& context,
    const std::shared_ptr<Texture>& texture);
std::unique_ptr<Screenshot> MakeGoldenScreenshotVK(ISize size,
                                                   std::vector<uint8_t> rgba);

}  // namespace impeller::testing
#endif  // FLUTTER_IMPELLER_GOLDEN_TESTS_GOLDEN_RENDERER_VK_H_
