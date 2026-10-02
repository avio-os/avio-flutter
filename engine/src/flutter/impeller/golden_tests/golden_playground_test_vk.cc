// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/golden_tests/golden_playground_test.h"

#include <algorithm>

#include "impeller/display_list/dl_image_impeller.h"
#include "impeller/golden_tests/golden_digest.h"
#include "impeller/golden_tests/golden_renderer_vk.h"
#include "impeller/renderer/command_buffer.h"
#include "impeller/renderer/command_queue.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/typographer/backends/skia/typographer_context_skia.h"

namespace impeller {
namespace {
std::string TestNameVK() {
  const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string name =
      std::string("impeller_") + test->test_suite_name() + "_" + test->name();
  std::replace(name.begin(), name.end(), '/', '_');
  return name;
}
}  // namespace

struct GoldenPlaygroundTest::GoldenPlaygroundTestImpl {
  std::shared_ptr<Context> context;
  ISize window_size = {1024, 768};
};

GoldenPlaygroundTest::GoldenPlaygroundTest()
    : typographer_context_(TypographerContextSkia::Make()),
      pimpl_(std::make_shared<GoldenPlaygroundTestImpl>()) {}
GoldenPlaygroundTest::~GoldenPlaygroundTest() = default;
void GoldenPlaygroundTest::SetTypographerContext(
    std::shared_ptr<TypographerContext> context) {
  typographer_context_ = std::move(context);
}
void GoldenPlaygroundTest::SetUp() {
  ASSERT_EQ(GetParam(), PlaygroundBackend::kVulkan)
      << "The headless Linux runner supports Vulkan only; select /Vulkan "
         "tests. Unsupported backend selection is a failure, not a skip.";
  pimpl_->context =
      testing::MakeHeadlessGoldenContextVK(testing::AvioGoldenPolicyVK::kMsaa4);
  ASSERT_TRUE(pimpl_->context && pimpl_->context->IsValid())
      << "A real Vulkan ICD, validation layers and generated shader libraries "
         "are required. VK_ICD_FILENAMES selects SwiftShader or the device.";
  testing::GoldenDigest::Instance()->AddDimension(
      "gpu_string", pimpl_->context->DescribeGpuModel());
}
void GoldenPlaygroundTest::TearDown() {
  if (pimpl_->context) {
    pimpl_->context->Shutdown();
    pimpl_->context.reset();
  }
}
PlaygroundBackend GoldenPlaygroundTest::GetBackend() const {
  return GetParam();
}
std::shared_ptr<Context> GoldenPlaygroundTest::GetContext() const {
  return pimpl_->context;
}
std::shared_ptr<Context> GoldenPlaygroundTest::MakeContext() const {
  if (pimpl_->context)
    pimpl_->context->Shutdown();
  pimpl_->context =
      testing::MakeHeadlessGoldenContextVK(testing::AvioGoldenPolicyVK::kMsaa4);
  return pimpl_->context;
}
bool GoldenPlaygroundTest::SaveScreenshot(
    std::unique_ptr<testing::Screenshot> screenshot,
    const std::string& postfix) {
  if (!screenshot || !screenshot->GetBytes())
    return false;
  const std::string test = TestNameVK();
  const std::string file = test + postfix + ".png";
  if (!screenshot->WriteToPNG(
          testing::WorkingDirectory::Instance()->GetFilenamePath(file))) {
    return false;
  }
  testing::GoldenDigest::Instance()->AddImage(
      test, file, screenshot->GetWidth(), screenshot->GetHeight());
  return true;
}
std::unique_ptr<testing::Screenshot> GoldenPlaygroundTest::MakeScreenshot(
    const sk_sp<flutter::DisplayList>& list) {
  AiksContext renderer(GetContext(), typographer_context_);
  auto texture = testing::RenderGoldenDisplayListVK(
      renderer, list, GetWindowSize(), testing::AvioGoldenPolicyVK::kMsaa4);
  return testing::ReadGoldenTextureVK(GetContext(), texture);
}
bool GoldenPlaygroundTest::OpenPlaygroundHere(
    const AiksDlPlaygroundCallback& callback) {
  AiksContext renderer(GetContext(), typographer_context_);
  std::unique_ptr<testing::Screenshot> screenshot;
  for (int i = 0; i < 2; ++i) {
    auto texture = testing::RenderGoldenDisplayListVK(
        renderer, callback(), GetWindowSize(),
        testing::AvioGoldenPolicyVK::kMsaa4);
    screenshot = testing::ReadGoldenTextureVK(GetContext(), texture);
    if (!screenshot)
      return false;
  }
  return SaveScreenshot(std::move(screenshot));
}
bool GoldenPlaygroundTest::OpenPlaygroundHere(
    const sk_sp<flutter::DisplayList>& list) {
  return OpenPlaygroundHere([&list] { return list; });
}
bool GoldenPlaygroundTest::OpenPlaygroundHere(
    const Playground::SinglePassCallback& callback) {
  const auto context = GetContext();
  RenderTargetAllocator allocator(context->GetResourceAllocator());
  auto target = allocator.CreateOffscreen(
      *context, GetWindowSize(), 1, "Golden Single Pass",
      RenderTarget::kDefaultColorAttachmentConfig, std::nullopt);
  auto commands = context->CreateCommandBuffer();
  if (!target.IsValid() || !commands)
    return false;
  auto pass = commands->CreateRenderPass(target);
  if (!pass || !callback(*pass) || !pass->EncodeCommands() ||
      !context->GetCommandQueue()->Submit({commands}).ok())
    return false;
  return SaveScreenshot(
      testing::ReadGoldenTextureVK(context, target.GetRenderTargetTexture()));
}
std::shared_ptr<Texture> GoldenPlaygroundTest::CreateTextureForFixture(
    const char* name,
    bool mipmapping) const {
  std::shared_ptr<fml::Mapping> mapping =
      flutter::testing::OpenFixtureAsMapping(name);
  return Playground::CreateTextureForMapping(GetContext(), mapping, mipmapping);
}
sk_sp<flutter::DlImage> GoldenPlaygroundTest::CreateDlImageForFixture(
    const char* name,
    bool mipmapping) const {
  return DlImageImpeller::Make(CreateTextureForFixture(name, mipmapping));
}
absl::StatusOr<RuntimeStage::Map> GoldenPlaygroundTest::OpenAssetAsRuntimeStage(
    const char* name) const {
  std::shared_ptr<fml::Mapping> mapping =
      flutter::testing::OpenFixtureAsMapping(name);
  if (!mapping || !mapping->GetSize())
    return absl::NotFoundError(name);
  return RuntimeStage::DecodeRuntimeStages(mapping);
}
Point GoldenPlaygroundTest::GetContentScale() const {
  return {1, 1};
}
Scalar GoldenPlaygroundTest::GetSecondsElapsed() const {
  return 0;
}
ISize GoldenPlaygroundTest::GetWindowSize() const {
  return pimpl_->window_size;
}
IRect GoldenPlaygroundTest::GetWindowBounds() const {
  return IRect::MakeSize(pimpl_->window_size);
}
void GoldenPlaygroundTest::SetWindowSize(ISize size) {
  pimpl_->window_size = size;
}
fml::Status GoldenPlaygroundTest::SetCapabilities(
    const std::shared_ptr<Capabilities>&) {
  return {fml::StatusCode::kUnimplemented,
          "Headless Vulkan uses actual device capabilities"};
}
RuntimeStageBackend GoldenPlaygroundTest::GetRuntimeStageBackend() const {
  return RuntimeStageBackend::kVulkan;
}
void GoldenPlaygroundTest::SetEnableWriteGolden(bool enabled) {
  ASSERT_TRUE(enabled) << "Every selected headless golden must execute";
}
bool GoldenPlaygroundTest::InitializePipelineDescriptorForRendering(
    PipelineDescriptor& descriptor) const {
  descriptor.SetSampleCount(SampleCount::kCount1);
  descriptor.ClearStencilAttachments();
  descriptor.ClearDepthAttachment();
  return true;
}

}  // namespace impeller
