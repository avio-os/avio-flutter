// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <memory>
#include "gtest/gtest.h"
#include "impeller/entity/contents/continuous_clip.h"
#include "impeller/entity/contents/uber_sdf_parameters.h"
namespace impeller::testing {
TEST(AvioContinuousClipTest, RetainsExactInversePhysicalGrid) {
  const auto clip = AvioContinuousClip::RectClip(Rect::MakeXYWH(2, 3, 10, 20));
  const Matrix transform = Matrix::MakeTranslation(Point(37.25f, 11.5f)) *
                           Matrix::MakeRotationZ(Radians(.73f)) *
                           Matrix::MakeScale(Vector2(2, .7f));
  const auto primitive = clip.Transform(transform);
  ASSERT_TRUE(primitive);
  const Point local(6, 9), device = transform * local;
  const auto& x = primitive->vectors[1];
  const auto& y = primitive->vectors[2];
  EXPECT_NEAR(x[0] * device.x + x[1] * device.y + x[2], local.x, 1e-4);
  EXPECT_NEAR(y[0] * device.x + y[1] * device.y + y[2], local.y, 1e-4);
  EXPECT_FALSE(clip.Transform(Matrix::MakeScale(Vector2(0, 1))));
}
TEST(AvioContinuousClipTest, EllipticalCornersRetainBothAxes) {
  auto clip = AvioContinuousClip::RoundRectClip(
      RoundRect::MakeRectXY(Rect::MakeXYWH(0, 0, 100, 80), 12, 7));
  EXPECT_EQ(clip.primitive.vectors[4][0], 12);
  EXPECT_EQ(clip.primitive.vectors[5][0], 7);
}
TEST(AvioContinuousClipTest,
     SavedStateIsImmutableAndCorrelatedIntersectionIsOnce) {
  auto old = std::make_shared<AvioContinuousClipExpression>();
  auto a = AvioContinuousClip::RectClip(Rect::MakeXYWH(.25f, .75f, 10, 20))
               .primitive;
  ASSERT_TRUE(old->Append(a, false));
  std::shared_ptr<const AvioContinuousClipExpression> saved = old;
  auto next = std::make_shared<AvioContinuousClipExpression>(*saved);
  auto b = AvioContinuousClip::OvalClip(Rect::MakeXYWH(3, 4, 5, 6)).primitive;
  ASSERT_TRUE(next->Append(b, true));
  EXPECT_EQ(saved->count, 1u);
  EXPECT_EQ(next->count, 2u);
  EXPECT_EQ(saved->primitives[0].vectors[0][1], 0.f);
  EXPECT_FLOAT_EQ(
      AvioContinuousCoverage(CombineAvioContinuousDistance(0, 0, false)), .5f);
  EXPECT_NE(AvioContinuousCoverage(CombineAvioContinuousDistance(0, 0, false)),
            .25f);
}
TEST(AvioContinuousClipTest, ExactContradictionIsEmptyInsteadOfFaintRim) {
  auto a = AvioContinuousClip::RectClip(Rect::MakeXYWH(.25f, .75f, 10, 20))
               .primitive;
  AvioContinuousClipExpression expression;
  ASSERT_TRUE(expression.Append(a, false));
  ASSERT_TRUE(expression.Append(a, true));
  EXPECT_EQ(expression.count, 1u);
  EXPECT_EQ(expression.primitives[0].vectors[0][0],
            static_cast<float>(AvioContinuousPrimitiveType::kEmpty));
  ASSERT_TRUE(expression.Append(a, false));
  EXPECT_EQ(expression.count, 1u);
}
TEST(AvioContinuousClipTest, EmptyGeometryHasNoAnalyticFringe) {
  const auto empty = AvioContinuousClip::RectClip(Rect::MakeXYWH(7, 9, 0, 12));
  EXPECT_EQ(empty.primitive.vectors[0][0],
            static_cast<float>(AvioContinuousPrimitiveType::kEmpty));
  const auto oval = AvioContinuousClip::OvalClip(Rect::MakeXYWH(7, 9, 12, 0));
  EXPECT_EQ(oval.primitive.vectors[0][0],
            static_cast<float>(AvioContinuousPrimitiveType::kEmpty));
  AvioContinuousClipExpression expression;
  const auto parent =
      AvioContinuousClip::RectClip(Rect::MakeXYWH(1, 2, 30, 40)).primitive;
  ASSERT_TRUE(expression.Append(parent, false));
  ASSERT_TRUE(expression.Append(empty.primitive, true));
  EXPECT_EQ(expression.count, 1u);
  EXPECT_EQ(expression.primitives[0], parent);
  ASSERT_TRUE(expression.Append(empty.primitive, false));
  EXPECT_EQ(expression.count, 1u);
  EXPECT_EQ(expression.primitives[0].vectors[0][0],
            static_cast<float>(AvioContinuousPrimitiveType::kEmpty));
  ASSERT_TRUE(expression.Append(parent, false));
  EXPECT_EQ(expression.count, 1u);
  EXPECT_EQ(expression.primitives[0].vectors[0][0],
            static_cast<float>(AvioContinuousPrimitiveType::kEmpty));
}
TEST(AvioContinuousClipTest,
     CapacityRefusalPreservesParentsAndDuplicateDoesNotGrow) {
  AvioContinuousClipExpression expression;
  for (size_t i = 0; i < kAvioContinuousMaxClips; i++) {
    ASSERT_TRUE(expression.Append(
        AvioContinuousClip::RectClip(Rect::MakeXYWH(i, 0, 10, 20)).primitive,
        false));
  }
  const auto before = expression.primitives;
  EXPECT_FALSE(expression.Append(
      AvioContinuousClip::RectClip(Rect::MakeXYWH(100, 0, 10, 20)).primitive,
      false));
  EXPECT_EQ(expression.count, kAvioContinuousMaxClips);
  EXPECT_EQ(expression.primitives, before);
  EXPECT_TRUE(expression.Append(before[0], false));
  EXPECT_EQ(expression.count, kAvioContinuousMaxClips);
}
TEST(AvioContinuousClipTest, GeometryFactoriesRetainBorderAndNormalizedArc) {
  auto outer = RoundRect::MakeRectXY(Rect::MakeXYWH(0, 0, 100, 80), 12, 7);
  auto inner = RoundRect::MakeRectXY(Rect::MakeXYWH(5, 7, 60, 50), 4, 2);
  const auto border =
      UberSDFParameters::MakeBorderedRoundedRect(Color::Red(), outer, inner);
  EXPECT_EQ(border.type, UberSDFParameters::Type::kBorderedRoundedRect);
  EXPECT_EQ(border.inner_center, inner.GetBounds().GetCenter());
  EXPECT_EQ(border.bordered_radii_y.x, 7);
  EXPECT_EQ(border.inner_radii_y.x, 2);
  const Arc arc(Rect::MakeXYWH(0, 0, 90, 60), Degrees(30), Degrees(-80), false);
  const auto parameters = UberSDFParameters::MakeArc(
      Color::Red(), arc, StrokeParameters{.width = 3, .cap = Cap::kSquare});
  EXPECT_NEAR(parameters.arc.x, static_cast<Radians>(Degrees(-50)).radians,
              1e-5);
  EXPECT_NEAR(parameters.arc.y, static_cast<Radians>(Degrees(80)).radians,
              1e-5);
  EXPECT_EQ(parameters.arc.w, 2);
}
TEST(AvioContinuousClipTest,
     DeferredOwnGeometryKeepsBothContoursAndStrokeJoin) {
  const auto outer =
      RoundRect::MakeRectXY(Rect::MakeXYWH(2, 3, 100, 80), 12, 7);
  const auto inner = RoundRect::MakeRectXY(Rect::MakeXYWH(9, 11, 60, 50), 4, 2);
  auto border =
      UberSDFParameters::MakeBorderedRoundedRect(Color::Red(), outer, inner);
  const auto primitive = AvioContinuousClip::Geometry(border);
  ASSERT_TRUE(primitive);
  EXPECT_EQ(primitive->shape_class, AvioContinuousClass::kBorderedRoundedRect);
  EXPECT_EQ(primitive->primitive.vectors[8][0],
            inner.GetBounds().GetCenter().x);
  EXPECT_EQ(primitive->primitive.vectors[9][0], 4);
  EXPECT_EQ(primitive->primitive.vectors[10][0], 2);
  auto arc = UberSDFParameters::MakeArc(
      Color::Red(),
      Arc(Rect::MakeXYWH(0, 0, 90, 60), Degrees(30), Degrees(80), true),
      StrokeParameters{.width = 3,
                       .cap = Cap::kSquare,
                       .join = Join::kBevel,
                       .miter_limit = 2});
  const auto arc_primitive = AvioContinuousClip::Geometry(arc);
  ASSERT_TRUE(arc_primitive);
  EXPECT_EQ(arc_primitive->primitive.vectors[12][0], 3);
  EXPECT_EQ(arc_primitive->primitive.vectors[12][1], 1);
  EXPECT_EQ(arc_primitive->primitive.vectors[12][2], 1);
  EXPECT_EQ(arc_primitive->primitive.vectors[12][3], 2);
}
TEST(AvioContinuousClipTest, CoverageUsesTheSameSingleSmoothRampAsShader) {
  EXPECT_FLOAT_EQ(AvioContinuousCoverage(-.5f), 1.f);
  EXPECT_FLOAT_EQ(AvioContinuousCoverage(-.25f), .84375f);
  EXPECT_FLOAT_EQ(AvioContinuousCoverage(0), .5f);
  EXPECT_FLOAT_EQ(AvioContinuousCoverage(.25f), .15625f);
  EXPECT_FLOAT_EQ(AvioContinuousCoverage(.5f), 0.f);
}
TEST(AvioContinuousClipTest, RasterQuadContainsPhysicalFringeUnderShear) {
  const auto params = UberSDFParameters::MakeArc(
      Color::Red(),
      Arc(Rect::MakeXYWH(0, 0, 90, 60), Degrees(30), Degrees(80), false),
      std::nullopt);
  const Matrix transform = Matrix::MakeTranslation(Point(37.25f, 11.5f)) *
                           Matrix::MakeRotationZ(Radians(.73f)) *
                           Matrix::MakeSkew(.7f, .2f) *
                           Matrix::MakeScale(Vector2(2, .7f));
  const auto padding = params.GetRasterPadding(transform);
  const auto inverse = transform.Invert();
  const auto origin = inverse * Point(0, 0);
  for (const auto x : {-1.f, 1.f}) {
    for (const auto y : {-1.f, 1.f}) {
      const auto neighbour = inverse * Point(x, y) - origin;
      EXPECT_LE(std::abs(neighbour.x), padding.width + 1e-5f);
      EXPECT_LE(std::abs(neighbour.y), padding.height + 1e-5f);
    }
  }
}
TEST(AvioContinuousClipTest, RasterQuadRetainsSectorMiterAndSquareCap) {
  const Arc sector(Rect::MakeXYWH(0, 0, 90, 60), Degrees(30), Degrees(5), true);
  const auto miter = UberSDFParameters::MakeArc(
      Color::Red(), sector,
      StrokeParameters{.width = 4, .join = Join::kMiter, .miter_limit = 8});
  EXPECT_GE(miter.GetRasterPadding(Matrix{}).width, 17);
  auto bevel = miter;
  bevel.stroke->join = Join::kBevel;
  EXPECT_LT(bevel.GetRasterPadding(Matrix{}).width,
            miter.GetRasterPadding(Matrix{}).width);
  const auto square = UberSDFParameters::MakeArc(
      Color::Red(),
      Arc(sector.GetOvalBounds(), sector.GetStart(), sector.GetSweep(), false),
      StrokeParameters{.width = 4, .cap = Cap::kSquare});
  EXPECT_GE(square.GetRasterPadding(Matrix{}).width, 1 + 2 * kSqrt2);
}
TEST(AvioContinuousClipTest, DefaultAndNativePolicyNeverOptIn) {
  AvioAntialiasingConfig config;
  EXPECT_EQ(config.continuous_requested_classes, 0u);
  config.continuous_requested_classes =
      static_cast<uint64_t>(AvioContinuousClass::kOvalClip);
  EXPECT_FALSE(config.RequestsContinuous(AvioContinuousClass::kOvalClip));
  config.policy = AvioAntialiasingPolicy::kCoverage;
  EXPECT_TRUE(config.RequestsContinuous(AvioContinuousClass::kOvalClip));
  EXPECT_FALSE(config.RequestsContinuous(AvioContinuousClass::kRectClip));
}
}  // namespace impeller::testing
