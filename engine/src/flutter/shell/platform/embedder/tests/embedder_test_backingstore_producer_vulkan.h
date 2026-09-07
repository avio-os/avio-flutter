// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_TESTS_EMBEDDER_TEST_BACKINGSTORE_PRODUCER_VULKAN_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_TESTS_EMBEDDER_TEST_BACKINGSTORE_PRODUCER_VULKAN_H_

#include "flutter/shell/platform/embedder/tests/embedder_test_backingstore_producer.h"

#include "flutter/testing/test_vulkan_context.h"

namespace flutter::testing {

class EmbedderTestBackingStoreProducerVulkan
    : public EmbedderTestBackingStoreProducer {
 public:
  EmbedderTestBackingStoreProducerVulkan(
      fml::RefPtr<TestVulkanContext> test_vulkan_context,
      RenderTargetType type);

  virtual ~EmbedderTestBackingStoreProducerVulkan();

  // Transfer the test image between Skia and Impeller in a known GENERAL
  // layout on their shared graphics queue. This is a real same-family handoff,
  // not a claim that the image belongs to a foreign process. Metadata may be
  // withheld to exercise the legacy full-repaint path; producer/consumer
  // synchronization and actual image-layout tracking remain mandatory.
  static bool PrepareForExternalRendering(
      const FlutterBackingStore* backing_store,
      bool advertise_external_ownership = true);
  static bool CompleteExternalRendering(
      const FlutterBackingStore* backing_store,
      int render_complete_sync_fd);

  bool Create(const FlutterBackingStoreConfig* config,
              FlutterBackingStore* backing_store_out) override;

  sk_sp<SkSurface> GetSurface(
      const FlutterBackingStore* backing_store) const override;

  sk_sp<SkImage> MakeImageSnapshot(
      const FlutterBackingStore* backing_store) const override;

 private:
  fml::RefPtr<TestVulkanContext> test_vulkan_context_;

  FML_DISALLOW_COPY_AND_ASSIGN(EmbedderTestBackingStoreProducerVulkan);
};

}  // namespace flutter::testing

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_TESTS_EMBEDDER_TEST_BACKINGSTORE_PRODUCER_VULKAN_H_
