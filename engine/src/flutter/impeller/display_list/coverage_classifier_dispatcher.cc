// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/display_list/coverage_classifier.h"

#include "flutter/display_list/dl_text.h"
#include "flutter/display_list/effects/image_filters/dl_compose_image_filter.h"
#include "flutter/display_list/effects/image_filters/dl_local_matrix_image_filter.h"
#include "flutter/display_list/utils/dl_receiver_utils.h"
#include "impeller/geometry/round_rect.h"
#include "impeller/geometry/round_superellipse.h"

namespace impeller {
namespace {
// Attribute state is global in a DisplayList; only nested DisplayLists reset
// it. Matrix and clips follow save/restore. No renderer or GPU calls occur.
class CoverageDispatcher final : public flutter::IgnoreAttributeDispatchHelper,
                                 public flutter::IgnoreClipDispatchHelper,
                                 public flutter::IgnoreTransformDispatchHelper,
                                 public flutter::IgnoreDrawDispatchHelper {
 public:
  CoverageDispatcher(CoverageDisplayListPlan& plan,
                     Rect bounds,
                     bool use_sdfs,
                     bool sample4_image1x)
      : plan_(plan),
        bounds_(bounds),
        use_sdfs_(use_sdfs),
        sample4_image1x_(sample4_image1x) {}
  void setAntiAlias(bool aa) override { paint_.aa = aa; }
  void setColor(flutter::DlColor color) override {
    paint_.alpha = color.getAlphaF();
  }
  void setDrawStyle(flutter::DlDrawStyle style) override {
    paint_.stroke = style != flutter::DlDrawStyle::kFill;
  }
  void setBlendMode(flutter::DlBlendMode mode) override {
    paint_.blend = static_cast<BlendMode>(mode);
  }
  void setStrokeWidth(float width) override { paint_.width = width; }
  void setStrokeMiter(float miter) override { paint_.miter = miter; }
  void setColorSource(const flutter::DlColorSource* source) override {
    paint_.uniform = !source;
    paint_.supported_source =
        !source || source->type() != flutter::DlColorSourceType::kRuntimeEffect;
  }
  void setColorFilter(const flutter::DlColorFilter* filter) override {
    paint_.filtered = filter != nullptr;
  }
  void setImageFilter(const flutter::DlImageFilter* filter) override {
    paint_.filter = filter;
  }
  void setMaskFilter(const flutter::DlMaskFilter* filter) override {
    paint_.mask = filter != nullptr;
  }
  void setInvertColors(bool invert) override { paint_.invert = invert; }
  void save() override {
    if (!matrices_.push_back(matrix_) || !opacities_.push_back(opacity_))
      plan_.Invalidate();
    plan_.Save();
  }
  void saveLayer(const flutter::DlRect& bounds,
                 const flutter::SaveLayerOptions options,
                 const flutter::DlImageFilter* backdrop,
                 std::optional<int64_t>) override {
    const bool attributes = options.renders_with_attributes();
    const bool reads = backdrop || (attributes && IsAdvanced(paint_.blend));
    const bool elided =
        options.can_distribute_opacity() && !options.content_is_unbounded() &&
        !reads &&
        !(attributes && (paint_.filter || paint_.mask || paint_.filtered));
    const Rect physical =
        options.content_is_unbounded() && !options.bounds_from_caller()
            ? bounds_
            : LayerBounds(bounds, attributes);
    if (!matrices_.push_back(matrix_) || !opacities_.push_back(opacity_))
      plan_.Invalidate();
    plan_.SaveLayer(physical, elided, reads);
    if (elided && attributes)
      opacity_ *= paint_.alpha;
    if (!elided) {
      plan_.RecordScratchDemand(
          physical, FilterImages(backdrop) +
                        (attributes ? FilterImages(paint_.filter) : 0) +
                        (reads ? 2 : 0));
    }
  }
  void restore() override {
    if (matrices_.empty())
      return;
    matrix_ = matrices_.back();
    matrices_.pop_back();
    opacity_ = opacities_.back();
    opacities_.pop_back();
    plan_.Restore();
  }
  void translate(float x, float y) override {
    matrix_ = matrix_ * Matrix::MakeTranslation({x, y, 0});
  }
  void scale(float x, float y) override {
    matrix_ = matrix_ * Matrix::MakeScale({x, y, 1});
  }
  void rotate(float degrees) override {
    matrix_ = matrix_ * Matrix::MakeRotationZ(Degrees(degrees));
  }
  void skew(float x, float y) override {
    matrix_ = matrix_ * Matrix::MakeSkew(x, y);
  }
  void transformReset() override { matrix_ = initial_; }
  void transform2DAffine(float a, float b, float c, float d, float e, float f)
      override {
    matrix_ = matrix_ *
              Matrix::MakeRow(a, b, 0, c, d, e, 0, f, 0, 0, 1, 0, 0, 0, 0, 1);
  }
  void transformFullPerspective(float a,
                                float b,
                                float c,
                                float d,
                                float e,
                                float f,
                                float g,
                                float h,
                                float i,
                                float j,
                                float k,
                                float l,
                                float m,
                                float n,
                                float o,
                                float p) override {
    matrix_ = matrix_ *
              Matrix::MakeRow(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p);
  }
  void clipRect(const flutter::DlRect& rect,
                flutter::DlClipOp op,
                bool aa) override {
    Clip(rect, op, aa, matrix_.IsAligned2D());
  }
  void clipOval(const flutter::DlRect& rect,
                flutter::DlClipOp op,
                bool aa) override {
    Clip(rect, op, aa, false);
  }
  void clipRoundRect(const flutter::DlRoundRect& rect,
                     flutter::DlClipOp op,
                     bool aa) override {
    Clip(rect.GetBounds(), op, aa, false);
  }
  void clipRoundSuperellipse(const flutter::DlRoundSuperellipse& rect,
                             flutter::DlClipOp op,
                             bool aa) override {
    Clip(rect.GetBounds(), op, aa, false);
  }
  void clipPath(const flutter::DlPath& path,
                flutter::DlClipOp op,
                bool aa) override {
    flutter::DlRect rect;
    const bool is_rect = path.IsRect(&rect) && matrix_.IsAligned2D();
    Clip(is_rect ? rect : path.GetBounds(), op, aa, is_rect);
  }
  void drawColor(flutter::DlColor color, flutter::DlBlendMode blend) override {
    const auto saved = paint_;
    paint_ = {};
    paint_.alpha = color.getAlphaF();
    paint_.blend = static_cast<BlendMode>(blend);
    plan_.RecordDraw(Facts(CoverageGeometryKind::kRect, bounds_));
    paint_ = saved;
  }
  void drawPaint() override {
    plan_.RecordDraw(Facts(CoverageGeometryKind::kRect, bounds_));
  }
  void drawLine(const flutter::DlPoint& p, const flutter::DlPoint& q) override {
    Draw(CoverageGeometryKind::kPath,
         Rect::MakeLTRB(std::min(p.x, q.x), std::min(p.y, q.y),
                        std::max(p.x, q.x), std::max(p.y, q.y)),
         true);
  }
  void drawDashedLine(const flutter::DlPoint& p,
                      const flutter::DlPoint& q,
                      float,
                      float) override {
    drawLine(p, q);
  }
  void drawRect(const flutter::DlRect& rect) override {
    Draw(CoverageGeometryKind::kRect, rect, false, true);
  }
  void drawOval(const flutter::DlRect& rect) override {
    Draw(CoverageGeometryKind::kRoundRect, rect, false, true);
  }
  void drawCircle(const flutter::DlPoint& center, float r) override {
    Draw(CoverageGeometryKind::kRoundRect,
         Rect::MakeLTRB(center.x - r, center.y - r, center.x + r, center.y + r),
         false, true);
  }
  void drawRoundRect(const flutter::DlRoundRect& rect) override {
    Draw(rect.GetRadii().AreAllCornersCircular()
             ? CoverageGeometryKind::kRoundRect
             : CoverageGeometryKind::kEllipticalRoundRect,
         rect.GetBounds(), false, rect.GetRadii().AreAllCornersCircular());
  }
  void drawRoundSuperellipse(
      const flutter::DlRoundSuperellipse& rect) override {
    // Mirrors RoundSuperellipseParam::MakeBoundsRadii's actual UberSDF
    // branch. Empty/asymmetric corners require the native geometry consumer.
    const auto& radii = rect.GetRadii();
    Draw(CoverageGeometryKind::kRoundRect, rect.GetBounds(), false,
         radii.AreAllCornersSame() && !radii.top_left.IsEmpty());
  }
  void drawDiffRoundRect(const flutter::DlRoundRect& outer,
                         const flutter::DlRoundRect&) override {
    Draw(CoverageGeometryKind::kBorder, outer.GetBounds());
  }
  void drawPath(const flutter::DlPath& path) override {
    Draw(CoverageGeometryKind::kPath, path.GetBounds());
  }
  void drawArc(const flutter::DlRect& rect, float, float, bool) override {
    Draw(CoverageGeometryKind::kArc, rect);
  }
  void drawPoints(flutter::DlPointMode,
                  uint32_t count,
                  const flutter::DlPoint* points) override {
    if (!count)
      return;
    float left = points[0].x, right = left, top = points[0].y, bottom = top;
    for (uint32_t i = 1; i < count; i++) {
      left = std::min(left, points[i].x);
      right = std::max(right, points[i].x);
      top = std::min(top, points[i].y);
      bottom = std::max(bottom, points[i].y);
    }
    const auto bounds = Rect::MakeLTRB(left, top, right, bottom);
    Draw(CoverageGeometryKind::kPath, bounds, true);
  }
  void drawVertices(const std::shared_ptr<flutter::DlVertices>& vertices,
                    flutter::DlBlendMode) override {
    Draw(CoverageGeometryKind::kOther, vertices->GetBounds());
  }
  void drawText(const std::shared_ptr<flutter::DlText>& text,
                float x,
                float y) override {
    Draw(CoverageGeometryKind::kText, Rect(text->GetBounds()).Shift({x, y}));
  }
  void drawShadow(const flutter::DlPath&,
                  flutter::DlColor,
                  float,
                  bool,
                  float) override {
    // Light projection and penumbra bounds are not inferred from path bounds.
    plan_.RecordDraw(Facts(CoverageGeometryKind::kOther, bounds_));
  }
  void drawImage(const sk_sp<flutter::DlImage> image,
                 const flutter::DlPoint& point,
                 flutter::DlImageSampling,
                 bool attributes) override {
    Image(Rect::MakeXYWH(point.x, point.y, image->width(), image->height()),
          attributes, true, image->isOpaque());
  }
  void drawImageRect(const sk_sp<flutter::DlImage> image,
                     const flutter::DlRect& source,
                     const flutter::DlRect& dst,
                     flutter::DlImageSampling,
                     bool attributes,
                     flutter::DlSrcRectConstraint) override {
    const Rect source_rect = source;
    const bool complete_source =
        source_rect.IsFinite() && !source_rect.IsEmpty() &&
        Rect::MakeXYWH(0, 0, image->width(), image->height())
            .Contains(source_rect);
    Image(dst, attributes, complete_source, image->isOpaque());
  }
  void drawImageNine(const sk_sp<flutter::DlImage>,
                     const flutter::DlIRect&,
                     const flutter::DlRect& dst,
                     flutter::DlFilterMode,
                     bool attributes) override {
    Image(dst, attributes, false);
  }
  void drawAtlas(const sk_sp<flutter::DlImage>,
                 const flutter::DlRSTransform*,
                 const flutter::DlRect*,
                 const flutter::DlColor*,
                 int,
                 flutter::DlBlendMode,
                 flutter::DlImageSampling,
                 const flutter::DlRect* bounds,
                 bool) override {
    Draw(CoverageGeometryKind::kImage, bounds ? Rect(*bounds) : bounds_);
  }
  void drawDisplayList(const sk_sp<flutter::DisplayList> list,
                       float opacity) override {
    const auto paint = paint_;
    const auto initial = initial_;
    initial_ = matrix_;
    paint_ = {};
    if (opacity < 1) {
      if (!matrices_.push_back(matrix_) || !opacities_.push_back(opacity_))
        plan_.Invalidate();
      plan_.SaveLayer(Physical(list->GetBounds()),
                      list->can_apply_group_opacity(), false);
      if (list->can_apply_group_opacity())
        opacity_ *= opacity;
    } else {
      save();
    }
    list->Dispatch(*this);
    restore();
    paint_ = paint;
    initial_ = initial;
  }

 private:
  struct PaintFacts {
    bool aa = true, stroke = false, uniform = true, supported_source = true;
    bool filtered = false, mask = false, invert = false;
    float width = 0, miter = 4, alpha = 1;
    BlendMode blend = BlendMode::kSrcOver;
    const flutter::DlImageFilter* filter = nullptr;
  } paint_;
  static bool IsAdvanced(BlendMode blend) { return blend > BlendMode::kScreen; }
  static uint64_t FilterImages(const flutter::DlImageFilter* filter) {
    if (!filter)
      return 0;
    if (const auto* compose = filter->asCompose())
      return FilterImages(compose->inner().get()) +
             FilterImages(compose->outer().get());
    if (const auto* local = filter->asLocalMatrix())
      return FilterImages(local->image_filter().get());
    // Source plus two sequential intermediates bounds the current separable
    // and downsampled filter leaves; composition sums leaves conservatively.
    // This is nominal graph demand, not an allocator-residency estimate.
    return filter->type() == flutter::DlImageFilterType::kColorFilter ? 1 : 3;
  }
  Rect Physical(Rect local) const {
    return matrix_.IsFinite() ? local.TransformBounds(matrix_) : bounds_;
  }
  Rect LayerBounds(Rect local, bool attributes) const {
    if (attributes && paint_.mask)
      return bounds_;
    if (attributes && paint_.filter) {
      flutter::DlRect out;
      if (!paint_.filter->map_local_bounds(local, out))
        return bounds_;
      // A shrinking outer filter does not make its larger input or inner
      // intermediates disappear while it executes.
      local = local.Union(out);
    }
    return Physical(local);
  }
  CoverageDrawFacts Facts(CoverageGeometryKind kind, Rect physical) const {
    return {
        .kind = kind,
        .physical_bounds = physical,
        .anti_alias = paint_.aa,
        .stroke = paint_.stroke,
        .uniform_source = paint_.uniform && !paint_.filter && !paint_.mask &&
                          !paint_.filtered && !paint_.invert,
        .source_is_opaque = paint_.uniform && paint_.alpha >= 1 &&
                            opacity_ >= 1 && !paint_.filter && !paint_.mask &&
                            !paint_.filtered && !paint_.invert,
        .analytic_source_supported = paint_.supported_source && !paint_.mask,
        .reads_destination = IsAdvanced(paint_.blend),
        .blend = paint_.blend};
  }
  void Draw(CoverageGeometryKind kind,
            Rect local,
            bool force_stroke = false,
            bool legacy_sdf_geometry = false) {
    const bool stroke = paint_.stroke || force_stroke;
    if (stroke)
      local = local.Expand(std::max(
          .5f, std::abs(paint_.width) * .5f * std::max(1.f, paint_.miter)));
    auto facts = Facts(kind, LayerBounds(local, true));
    facts.stroke = stroke;
    facts.uses_legacy_sdf = legacy_sdf_geometry &&
                            CanRenderLegacySdfAt1x(facts, matrix_, use_sdfs_);
    facts.direct_1x_eligible = facts.uses_legacy_sdf;
    // This is a candidate for the whole-scope one-sample target. Mixed/clipped
    // scopes retain their original analytic source in native4 replay. Source
    // eligibility alone cannot certify sample equality or fringe correlation.
    if (kind == CoverageGeometryKind::kRect && !stroke &&
        facts.uniform_source && matrix_.IsFinite() && matrix_.IsAffine()) {
      auto points = local.GetPoints();
      for (auto& point : points)
        point = matrix_ * point;
      facts.uniform_geometry = CoverageConvexQuad4::Make(points);
    }
    plan_.RecordDraw(facts);
    plan_.RecordScratchDemand(facts.physical_bounds,
                              FilterImages(paint_.filter) +
                                  (paint_.mask ? 3 : 0) +
                                  (facts.reads_destination ? 2 : 0));
  }
  void Image(Rect local,
             bool attributes,
             bool image_edge_supported = true,
             bool image_is_opaque = false) {
    const auto saved = paint_;
    if (!attributes)
      paint_ = {};
    auto facts = Facts(CoverageGeometryKind::kImage, LayerBounds(local, true));
    facts.uniform_source = false;
    facts.source_is_opaque =
        image_is_opaque && paint_.alpha == 1 && opacity_ == 1;
    facts.direct_1x_eligible = !paint_.aa && !paint_.filter && !paint_.mask &&
                               !paint_.filtered && !paint_.invert &&
                               paint_.blend == BlendMode::kSrcOver;
    facts.image_has_edge =
        !matrix_.IsAligned2D() ||
        !CanUseRectClipScissor4(Physical(local), ClipOperation::kIntersect);
    // Native4 still rasterizes fractional/rotated geometry when a paint asks
    // for non-AA. Do not change that device sample membership by moving it to
    // a one-sample pass merely because the paint flag is false.
    facts.direct_1x_eligible &= !facts.image_has_edge;
    facts.sample_values_are_equal = facts.direct_1x_eligible;
    // Ordinary sample4 image shaders evaluate their existing texture source
    // once at the pixel center. The authorized consumer resolves the quad's
    // four membership bits once, after that same source transfer. Filters or
    // per-sample/continuous sources cannot borrow this proof.
    facts.analytic_image_1x_eligible =
        sample4_image1x_ && image_edge_supported && facts.source_is_opaque &&
        paint_.aa && !paint_.filter && !paint_.mask && !paint_.filtered &&
        !paint_.invert && paint_.blend == BlendMode::kSrcOver &&
        matrix_.IsFinite() && matrix_.IsAffine() && matrix_.IsInvertible() &&
        matrix_.Invert().IsFinite();
    plan_.RecordImage(local, matrix_, facts);
    paint_ = saved;
    plan_.RecordScratchDemand(facts.physical_bounds,
                              attributes ? FilterImages(paint_.filter) +
                                               (paint_.mask ? 3 : 0) +
                                               (facts.reads_destination ? 2 : 0)
                                         : 0);
  }
  void Clip(Rect local, flutter::DlClipOp op, bool aa, bool rect) {
    plan_.RecordClip(Physical(local), matrix_,
                     op == flutter::DlClipOp::kIntersect
                         ? ClipOperation::kIntersect
                         : ClipOperation::kDifference,
                     rect, aa);
  }
  CoverageDisplayListPlan& plan_;
  Rect bounds_;
  bool use_sdfs_;
  bool sample4_image1x_;
  Matrix matrix_, initial_;
  Scalar opacity_ = 1;
  CoverageFixedVector<Matrix, 512> matrices_;
  CoverageFixedVector<Scalar, 512> opacities_;
};
}  // namespace

std::shared_ptr<const CoverageDisplayListPlan> ClassifyCoverageDisplayList(
    const sk_sp<flutter::DisplayList>& list,
    std::shared_ptr<CoverageDisplayListPlan> storage,
    Rect bounds,
    size_t bpp,
    bool owner_use_sdfs,
    bool owner_sample4_image1x) {
  // The ContentContext owns the sole standing reference. A retained parent or
  // nested replay must not overwrite its exact plan. No frame allocation is
  // used to recover: absence selects the existing conservative native4 route.
  if (!storage || storage.use_count() != 2)
    return nullptr;
  storage->Reset(bounds, bpp);
  CoverageDispatcher dispatcher(*storage, bounds, owner_use_sdfs,
                                owner_sample4_image1x);
  // Classify the complete root subtree. Damage-culling differences in actual
  // replay cannot turn absent or ambiguous facts into an optimization proof.
  list->Dispatch(dispatcher);
  storage->Finalize();
  return storage;
}

}  // namespace impeller
