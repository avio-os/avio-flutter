#!/usr/bin/env bash
# Avio engine fork — must-survive patch inventory.
# Asserts every load-bearing fix / ABI extension is present in the working tree.
# Run from the fork root after any rebase or curation. Exit 0 = all preserved.
set -u
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
cd "$ROOT"
F=engine/src/flutter
W=packages/flutter
fail=0

need() { # $1=label $2=file $3=pattern
  if git grep -q -E "$3" -- "$2" 2>/dev/null || grep -qrE "$3" "$2" 2>/dev/null; then
    echo "OK   $1"
  else
    echo "MISS $1  [$3 not in $2]"; fail=1
  fi
}
absent() { # $1=label $2=pattern
  if grep -qrE "$2" $F/shell $F/impeller $F/lib/ui 2>/dev/null; then
    echo "LEAK $1  [$2 still present]"; fail=1
  else
    echo "OK   $1 (absent as intended)"
  fi
}
absent_in() { # $1=label $2=path $3=pattern
  if grep -qE "$3" "$2" 2>/dev/null; then
    echo "LEAK $1  [$3 still present in $2]"; fail=1
  else
    echo "OK   $1 (absent as intended)"
  fi
}

echo "--- RSS / lifecycle fixes ---"
need "RSS: DisposeThreadLocalCachedResources in EVE submit" \
  $F/shell/platform/embedder/embedder_external_view_embedder.cc 'DisposeThreadLocalCachedResources'
need "non-blocking PublishDmabufTexture entry point" \
  $F/shell/platform/embedder/embedder.cc 'FlutterEnginePublishDmabufTexture'

echo "--- Embedder compositor modes ---"
need "stock generic layer-present ABI" \
  $F/shell/platform/embedder/embedder.h 'FlutterLayersPresentCallback present_layers_callback'
need "stock generic per-view present ABI" \
  $F/shell/platform/embedder/embedder.h 'FlutterPresentViewCallback present_view_callback'
need "explicit root-target compositor mode" \
  $F/shell/platform/embedder/embedder.h 'kFlutterCompositorModeRootRenderTarget'
need "versioned Avio semantic capability query" \
  $F/shell/platform/embedder/embedder.h 'FlutterEngineGetAvioExtensionCapabilities'
need "typed root-target terminal results" \
  $F/shell/platform/embedder/embedder.h 'kFlutterPresentRenderTargetStatusUnsupportedPlatformView'
need "separate generic and root EVE submission paths" \
  $F/shell/platform/embedder/embedder_external_view_embedder.cc 'SubmitGenericFlutterView'
absent_in "Linux desktop embedder root-target override" \
  $F/shell/platform/linux/fl_engine.cc 'present_render_target_callback'
absent_in "Windows desktop embedder root-target override" \
  $F/shell/platform/windows/flutter_windows_engine.cc 'present_render_target_callback'
absent_in "macOS desktop embedder root-target override" \
  $F/shell/platform/darwin/macos/framework/Source/FlutterEngine.mm \
  'present_render_target_callback'

need "preview metadata propagates through SceneBuilder ancestors" \
  $F/lib/ui/compositing/scene_builder.cc 'ancestor->set_subtree_has_avio_window_preview'
need "real SceneBuilder preview regression coverage" \
  $F/lib/ui/compositing/avio_preview_scene_builder_unittests.cc \
  'AvioPreviewSceneBuilderPreservesNestedAndRetainedMetadata'

need "typed pre-submit allocation failure" \
  $F/shell/platform/embedder/embedder.h 'kFlutterPresentRenderTargetStatusAllocationFailedBeforeSubmit'
need "pre-submit failure semantic negotiation" \
  $F/shell/platform/embedder/embedder.cc 'Selected-target damage requires typed pre-submission failures'
need "real Vulkan allocation refusal regression" \
  $F/shell/platform/embedder/tests/embedder_vk_unittests.cc 'SelectedTargetBudgetRefusalPreservesExactCollectibleLease'

echo "--- Exact frame opportunities ---"
need "exact opportunity feature negotiation" \
  $F/shell/platform/embedder/embedder.h \
  'kFlutterAvioExtensionFeatureFrameOpportunityOutcomes'
need "Avio ABI extension version (v9 exact root frame facts)" \
  $F/shell/platform/embedder/embedder.h \
  'FLUTTER_AVIO_EXTENSION_VERSION 9u'
need "render-deadline semantic feature" \
  $F/shell/platform/embedder/embedder.h \
  'kFlutterAvioExtensionFeatureRenderDeadline'
need "render deadline travels in exact vsync ABI" \
  $F/shell/platform/embedder/embedder.h 'uint64_t render_deadline_time_nanos'
need "frame timing recorder retains the producer deadline" \
  $F/flow/frame_timings.h 'GetRenderDeadlineTime'
# Commit 57cd5217 deliberately keeps allocation/deadline experiment probes
# on the diagnostics branch. Main must retain deadline transport and recorder
# semantics; the optional miss-trace emitter is not a production requirement.
need "render deadline recorder regression" \
  $F/flow/frame_timings_recorder_unittests.cc 'RecordVsyncPreservesRenderDeadline'
need "engine-local opportunity conservation ledger" \
  $F/common/frame_opportunity.h 'class FrameOpportunityRegistry'
need "exact returned-opportunity cancellation ABI" \
  $F/shell/platform/embedder/embedder.h \
  'FlutterEngineCancelFrameOpportunity'
need "scheduled return names exact targets" \
  $F/shell/platform/embedder/embedder.h 'const FlutterViewId\* target_ids'
need "admitted targets travel with frame opportunity custody" \
  $F/common/frame_opportunity.h 'std::set<int64_t> target_ids'
need "Animator reconciles admitted targets at the UI boundary" \
  $F/shell/common/animator.cc 'ReconcileFrameTargets'
need "admitted-target reconciliation regression test" \
  $F/shell/common/animator_unittests.cc \
  'ExactOpportunityReconcilesEveryAdmittedTarget'
need "typed per-target terminal outcomes" \
  $F/shell/platform/embedder/embedder.h \
  'kFlutterFrameOpportunityOutcomeBackpressured'
absent_in "process-global vsync baton registry" \
  $F/shell/platform/embedder/vsync_waiter_embedder.cc \
  'PendingBatonRegistry|kMaxPendingBatons'
absent_in "Animator frame-path IMPORTANT/high-water logging" \
  $F/shell/common/animator.cc 'FML_LOG\(IMPORTANT\)|LogStateHighWatermarks'
absent_in "platform-configuration frame-path IMPORTANT/high-water logging" \
  $F/lib/ui/window/platform_configuration.cc \
  'FML_LOG\(IMPORTANT\)|LogStateHighWatermarks'
absent_in "signal/poll raster watchdog" \
  $F/shell/common/shell.cc \
  'raster_watchdog|RasterStallSigHandler|SIGUSR1|WATCHDOG:'

echo "--- Explicit sync chain ---"
need "render_complete_sync_fd ABI field" \
  $F/shell/platform/embedder/embedder.h 'render_complete_sync_fd'
need "ExternalSemaphoreVK SYNC_FD export" \
  $F/impeller/renderer/backend/vulkan 'ExternalSemaphoreVK'
need "per-texture signal semaphores" \
  $F/impeller/renderer/backend/vulkan 'CreateSignalSemaphores'

echo "--- DMA-BUF external textures ---"
need "FlutterDmabufDescriptor ABI" \
  $F/shell/platform/embedder/embedder.h 'FlutterDmabufDescriptor'
need "DmabufTextureSourceVK" \
  $F/impeller/renderer/backend/vulkan 'DmabufTextureSourceVK'
need "dmabuf acquire fence import (GPU-side wait)" \
  $F/impeller/renderer/backend/vulkan 'acquire_fence'
need "native texture notifications reach Dart" \
  $F/lib/ui/window/platform_configuration.cc 'NotifyTextureFrameAvailable'
need "framework texture invalidation is keyed by texture ID" \
  $W/lib/src/rendering/binding.dart \
  'Map<int, Set<VoidCallback>> _textureFrameCallbacks'
need "texture ownership follows attach and detach" \
  $W/lib/src/rendering/texture.dart \
  'registerTextureFrameAvailableCallback'
need "DMA-BUF texture publication notifies exact framework consumers" \
  $F/shell/platform/embedder/embedder_engine.cc \
  'engine->NotifyTextureFrameAvailable\(texture_id\)'
if sed -n '/bool EmbedderEngine::PublishDmabufTexture/,/^}/p' \
    "$F/shell/platform/embedder/embedder_engine.cc" | \
    grep -q 'engine->ScheduleFrame'; then
  echo "LEAK DMA-BUF texture publication schedules a global frame"
  fail=1
else
  echo "OK   DMA-BUF texture publication preserves compositor cadence"
fi

echo "--- Damage tracking ---"
need "frame_damage in FlutterBackingStorePresentInfo" \
  $F/shell/platform/embedder/embedder.h 'frame_damage'
need "logical and raster damage are separated" \
  $F/flow/compositor_context.h 'intentionally distinct from sparse logical frame damage'
need "dmabuf per-commit damage rects ABI" \
  $F/shell/platform/embedder/embedder.h 'num_damage_rects'
need "selected-target damage feature negotiation" \
  $F/shell/platform/embedder/embedder.h \
  'kFlutterAvioExtensionFeatureSelectedTargetDamage'
need "selected backing target carries exact content history" \
  $F/shell/platform/embedder/embedder.h \
  'FlutterBackingStoreContentState'
need "selected buffer damage is distinct from frame damage" \
  $F/shell/platform/embedder/embedder.h 'FlutterRegion\* buffer_damage'
need "root target is acquired before damage finalization" \
  $F/shell/common/rasterizer.cc 'AcquireRootRenderTarget'
need "Impeller first pass honors a preserved target load" \
  $F/impeller/entity/inline_pass_context.cc \
  'honor_declared_load_action \? declared_load_action : LoadAction::kClear'
need "embedder root target always asks for multisampling" \
  $F/shell/platform/embedder/embedder.cc \
  'GetCachedSwapchainTransientsVK\(impeller_context, desc,$'
need "a refused multisample reservation degrades instead of failing" \
  $F/shell/platform/embedder/embedder.cc \
  'frame single-sampled costs the frame its antialiasing'
need "root pass sample-count changes are reported" \
  $F/shell/platform/embedder/embedder.cc 'ReportRootPassSampleCount'
need "a whole-target raster forfeits the damage clip" \
  $F/flow/compositor_context.cc 'RasterReplacesWholeTarget'
need "a multisampled target honors no damage rectangle" \
  $F/shell/platform/embedder/embedder_external_view.cc \
  'honors_damage'
need "only the caller-owned root target opts out of the first-pass clear" \
  $F/impeller/display_list/canvas.cc \
  'honor_declared_load_action=\*/false'
need "preserved foreign target acquire declares attachment reads" \
  $F/impeller/renderer/backend/vulkan/command_buffer_vk.cc \
  'vk::AccessFlagBits::eMemoryRead \| vk::AccessFlagBits::eMemoryWrite'
need "preserved render-pass dependency declares attachment reads" \
  $F/impeller/renderer/backend/vulkan/render_pass_builder_vk.cc \
  'dstAccessMask \|= vk::AccessFlagBits::eColorAttachmentRead'
absent_in "partial-raster scratch/copy compensation" \
  $F/shell/platform/embedder/embedder_external_view.cc \
  'Embedder Partial Repaint Copy|GetImpellerPartialRepaintTarget'
need "three-target selected-damage pixel regression" \
  $F/shell/platform/embedder/tests/embedder_vk_unittests.cc \
  'SelectedTargetDamageReacquiresAndRepaintsExactRetainedTarget'
need "transparent blur removal pixel regression" \
  $F/shell/platform/embedder/tests/embedder_vk_unittests.cc \
  'SelectedTargetDamageClearsRemovedBlurWithFullRepaintParity'
need "disjoint translucent replacement pixel regression" \
  $F/shell/platform/embedder/tests/embedder_vk_unittests.cc \
  'SelectedTargetDamageKeepsSparseFrameDamageForTranslucentGap'
need "full fallback clears preserved selected target" \
  $F/shell/platform/embedder/tests/embedder_vk_unittests.cc \
  'SelectedTargetDamageFullFallbackClearsPreservedTarget'
need "preserved targets stay multisampled pixel regression" \
  $F/shell/platform/embedder/tests/embedder_vk_unittests.cc \
  'SelectedTargetDamageKeepsPreservedTargetMultisampled'
need "selected-target no-change waits for exact empty buffer damage" \
  $F/shell/platform/embedder/embedder_external_view_embedder.cc \
  '!selected_target_damage_ &&'
need "complete scene removal clears a preserved selected target" \
  $F/shell/platform/embedder/tests/embedder_vk_unittests.cc \
  'SelectedTargetDamageClearsFullyRemovedSceneBeforeNoVisualChange'
need "initial empty selected target remains unpublished" \
  $F/shell/platform/embedder/tests/embedder_vk_unittests.cc \
  'SelectedTargetDamageInitialEmptySceneDoesNotPublishUnknownTarget'

echo "--- Typed backing stores / chrome ---"
need "FlutterBackingStoreRequestType enum" \
  $F/shell/platform/embedder/embedder.h 'kFlutterBackingStoreRequestTypePerWindowChrome'
need "visual identifier plumbing" \
  $F/shell/platform/embedder 'visual_identifier'

echo "--- Frame scheduling ---"
need "empty-frame callback ABI" \
  $F/shell/platform/embedder/embedder.h 'on_empty_frame_callback'
need "animator empty-frame delegate" \
  $F/shell/common/animator.cc 'EmptyFrameForDisplay'
need "ScheduleFrame request-kind family" \
  $F/shell/platform/embedder/embedder.h 'FlutterEngineScheduleFrameForDisplayViewsWithRequestKind'
need "new views publish initial display ownership before Dart AddView" \
  $F/shell/common/engine.cc 'animator_->RegisterInitialViewDisplay'
need "initial display registration does not manufacture frame demand" \
  $F/shell/common/animator.cc 'bool Animator::RegisterInitialViewDisplay'
need "initial display registration is one-shot and demand-free in tests" \
  $F/shell/common/animator_unittests.cc \
  'InitialViewDisplayRegistrationIsOneShotAndDemandFree'
need "metrics-time display ownership is covered at the public embedder boundary" \
  $F/shell/platform/embedder/tests/embedder_unittests.cc \
  'AddViewPublishesInitialDisplayBeforeMetricsScheduleFrame'
need "the regression schedules inside synchronous metrics publication" \
  $F/shell/platform/embedder/fixtures/main.dart \
  'add_view_schedules_frame_from_metrics'
absent_in "RuntimeController-owned alternate first-frame scheduler" \
  $F/runtime/runtime_controller.h 'schedule_frame'
need "dart:ui scheduleFrameForDisplayViews" \
  $F/lib/ui/platform_dispatcher.dart 'scheduleFrameForDisplayViews'
need "scoped frame scheduling reports exact engine acceptance" \
  $F/shell/common/animator.h 'bool RequestFrameForDisplayViews'
need "framework latches only an accepted platform frame request" \
  $W/lib/src/scheduler/binding.dart \
  '_hasScheduledFrame = dispatchPlatformScheduleFrame\(\)'
need "engine exact-acceptance regression" \
  $F/shell/common/animator_unittests.cc \
  'ScopedFrameRequestReportsWhetherItWasRetained'
need "framework rejected-request regression" \
  $W/test/widgets/view_scoped_frame_scheduling_test.dart \
  'a rejected engine request does not latch the framework scheduler'
need "flutter_test forwards scoped frame acceptance" \
  $W/../flutter_test/lib/src/window.dart \
  'bool scheduleFrameForDisplayViews'
need "flutter_test scoped acceptance regression" \
  $W/../flutter_test/test/platform_dispatcher_test.dart \
  'forwards scoped frame request acceptance'
need "configure_serial resize gating (metrics event)" \
  $F/shell/platform/embedder/embedder.h 'configure_serial'
need "animator pending-serial reuse gate" \
  $F/shell/common/animator.cc 'pending_configure_serial_'
need "typed exact root-target acquisition ABI" \
  $F/shell/platform/embedder/embedder.h \
  'FlutterRenderTargetAcquisitionCallback'
need "typed host rejection at target acquisition" \
  $F/shell/platform/embedder/embedder.h \
  'kFlutterRenderTargetAcquisitionHostRejected'
need "typed acquisition crosses the exact opportunity" \
  $F/shell/platform/embedder/embedder_external_view_embedder.cc \
  'pending_frame_opportunity_->id'
need "only exact backpressure rearms demand" \
  $F/shell/common/rasterizer.cc 'backpressured_target_ids'
need "every non-backpressure acquisition terminal is tested" \
  $F/shell/common/rasterizer_unittests.cc \
  'ExactOutcomeTerminalizesWithoutRearmingDemand'
absent_in "blanket unavailable-target rearm" \
  $F/shell/common/rasterizer.cc 'RearmUnavailableTargets'
absent_in "null target ambiguity" \
  $F/shell/platform/embedder/embedder.h \
  'RenderTargetUnavailable'
absent_in "boolean refusal query" \
  $F/shell/platform/embedder/embedder_external_view_embedder.cc \
  'DidRefuseRootRenderTarget'

echo "--- Framework scoped-frame authority ---"
need "scoped renderer consumes only active view ids" \
  $W/lib/src/rendering/binding.dart 'for \(final int viewId in activeViewIds\)'
need "residual render work gets a follow-up frame" \
  $W/lib/src/rendering/binding.dart '_scheduleResidualRenderWork'
need "View roots own independent build scopes" \
  $W/lib/src/widgets/view.dart 'final BuildScope _buildScope'
need "BuildOwner indexes view build scopes" \
  $W/lib/src/widgets/framework.dart 'registerViewBuildScope'
need "widgets build only admitted view scopes" \
  $W/lib/src/widgets/binding.dart 'owner\.buildViewScope\(viewId\)'
need "scoped frames can bootstrap structural View boundaries" \
  $W/lib/src/widgets/binding.dart 'activeViewIds == null \|\| _hasUnattributableDirtyBuild'
need "new View content waits in its owned build scope" \
  $W/lib/src/widgets/view.dart 'activeFrameViewIds == null'
need "dirty-view settlement occurs at the owning scope" \
  $W/lib/src/widgets/binding.dart '_dirtyBuildViewIds\.remove\(viewId\)'
need "unsettled widget scopes request another opportunity" \
  $W/lib/src/widgets/binding.dart 'schedulePendingWidgetBuilds\(\)'
need "retired views release build-scheduling custody" \
  $W/lib/src/widgets/binding.dart 'onViewBuildScopeRetired = _retireViewBuildScope'
need "test frames use the production build-scope operation" \
  packages/flutter_test/lib/src/binding.dart 'buildDirtyWidgetScopes\(\)'
need "test frames use the production residual-build operation" \
  packages/flutter_test/lib/src/binding.dart 'schedulePendingWidgetBuilds\(\)'
absent_in "deferred pointer queue below the build-scope invariant" \
  $W/lib/src/rendering/binding.dart '_DeferredPointerEventQueue'
absent_in "synthetic pointer terminal on scoped-frame delay" \
  $W/lib/src/rendering/binding.dart '_collapseToTerminalState'
need "post-frame mouse updates remain view-scoped" \
  $W/lib/src/rendering/mouse_tracker.dart 'updateDevicesForViews'
need "elements carry typed view ownership" \
  $W/lib/src/widgets/framework.dart 'sealed class BuildViewIdentity'
need "View boundaries seal one child identity" \
  $W/lib/src/widgets/view.dart 'BuildViewIdentity\.view'
need "dirty marks consume cached O(1) ownership" \
  $W/lib/src/widgets/framework.dart \
  'onElementDirtied\?\.call\(element\.buildViewIdentity\)'
need "view ownership follows GlobalKey reparenting" \
  $W/lib/src/widgets/framework.dart '_updateBuildViewIdentityRecursively'
absent_in "dirty-mark RenderView ancestor walk" \
  $W/lib/src/widgets/binding.dart \
  'findAncestorRenderObjectOfType<RenderView>'
need "scoped-frame authority regression test" \
  $W/test/widgets/view_scoped_frame_scheduling_test.dart \
  'scoped frame leaves inactive view coherent and its input live'
need "view BuildScope isolation regression test" \
  $W/test/widgets/view_test.dart \
  'view build scopes isolate dirty sibling trees'
need "nested View updates remain behind their own scope" \
  $W/test/widgets/view_test.dart \
  'parent updates cannot rebuild a nested view outside its scope'
need "residual widget demand schedules another scoped frame" \
  $W/test/widgets/view_scoped_build_scheduling_test.dart \
  'unprocessed view scope requests another frame opportunity'
need "first scoped frame bootstrap regression test" \
  $W/test/widgets/view_scoped_build_scheduling_test.dart \
  'the first scoped frame bootstraps only its admitted view'
need "structural pointer preservation regression test" \
  $W/test/widgets/view_scoped_frame_scheduling_test.dart \
  'scoped render demand never rewrites structural pointer events'
absent_in "cross-view frameRenderViews widening (forbidden, patch #22)" \
  $W/lib/src/rendering/binding.dart 'frameRenderViews'

echo "--- Vulkan lifecycle ---"
need "timeline-semaphore completion" \
  $F/impeller/renderer/backend/vulkan 'timeline_completion'
need "render batch precedes the timeline marker batch" \
  $F/impeller/renderer/backend/vulkan/command_queue_vk.cc \
  'ImpellerSubmitCompletionDependency'
need "timeline completion preserves upstream GPU submission tracking" \
  $F/impeller/renderer/backend/vulkan/command_queue_vk.cc \
  'GetMutableSubmissionTracker'
need "timeline callback retires the exact upstream submission id" \
  $F/impeller/renderer/backend/vulkan/command_queue_vk.cc \
  'RecordCompletion\(submission_id\)'
need "transients pool budget" \
  $F/impeller/renderer/backend/vulkan 'IMPELLER_VK_TRANSIENTS_BUDGET_MIB'
need "transient admission fails instead of unaccounting leased entries" \
  $F/impeller/renderer/backend/vulkan/swapchain/transients_pool_vk.cc \
  'if \(candidate == lru_\.end\(\)\)'
need "transient trim requires wrapper and GPU idleness" \
  $F/impeller/renderer/backend/vulkan/swapchain/transients_pool_vk.cc \
  'entry\.transients\.use_count\(\) == 1u && entry\.transients->IsIdle\(\)'
need "one transient attachment set per key, shared whatever its lease" \
  $F/impeller/renderer/backend/vulkan/swapchain/transients_pool_vk.cc \
  'if \(const auto found = FindLocked\(key\); found != lru_\.end\(\)\)'
absent_in "per-lease transient exclusivity (0bdaa23b3b) stays removed" \
  $F/impeller/renderer/backend/vulkan/swapchain/transients_pool_vk.cc \
  'it->key == key && it->transients\.use_count\(\) == 1u'
need "the single-queue sharing invariant is documented" \
  $F/impeller/renderer/backend/vulkan/swapchain/swapchain_transients_vk.h \
  'Sharing invariant'
need "incoming dependency orders shared depth/stencil writes" \
  $F/impeller/renderer/backend/vulkan/render_pass_builder_vk.cc \
  'srcAccessMask \|= vk::AccessFlagBits::eDepthStencilAttachmentWrite'
need "shared transient entry regression" \
  $F/impeller/renderer/backend/vulkan/context_vk_unittests.cc \
  'TransientsPoolSharesOneEntryPerKeyWhileTracked'
need "shared transient synchronization-validation proof and control" \
  $F/impeller/renderer/backend/vulkan/transients_sharing_vk_unittests.cc \
  'SharedDepthStencilNeedsTheWidenedIncomingDependency'
need "root-target transients are acquired on behalf of their view" \
  $F/shell/platform/embedder/embedder.cc \
  'pool->Acquire\(desc, enable_msaa, owner, refusal\)'
need "a collected view releases its transient ownership" \
  $F/shell/common/rasterizer.cc 'context->ReleaseTransientOwner\(view_id\)'
need "idle orphaned transient sets are freed once per raster frame" \
  $F/shell/common/rasterizer.cc 'context->ReleaseOrphanedTransients\(\)'
need "orphans are released only when idle" \
  $F/impeller/renderer/backend/vulkan/swapchain/transients_pool_vk.cc \
  'if \(!EntryIsOrphanLocked\(\*it\) \|\| !EntryIsIdle\(\*it\)\)'
need "real bytes come from the backend allocation" \
  $F/impeller/renderer/backend/vulkan/allocator_vk.cc \
  'allocated_byte_size_ = static_cast<size_t>\(allocation_info\.size\)'
need "a transient set reports its attachments' allocations" \
  $F/impeller/renderer/backend/vulkan/swapchain/swapchain_transients_vk.cc \
  'size_t SwapchainTransientsVK::GetAllocatedByteSize\(\) const'
need "the transient pool reports usage" \
  $F/impeller/renderer/backend/vulkan/swapchain/transients_pool_vk.cc \
  'RenderResourceUsage TransientsPoolVK::ReportUsage\(bool start_new_interval\)'
need "every transient pool erase is accounted" \
  $F/impeller/renderer/backend/vulkan/swapchain/transients_pool_vk.cc \
  'it = EraseLocked\(it\);'
need "the offscreen cache reports usage" \
  $F/impeller/entity/render_target_cache.cc \
  'RenderResourceUsage RenderTargetCache::ReportUsage\(bool start_new_interval\)'
need "the usage report carries transient health" \
  $F/impeller/renderer/render_resource_usage.h 'size_t duplicate_entries'
need "the usage report carries created entries and bytes" \
  $F/impeller/renderer/render_resource_usage.h 'size_t created_real_bytes'
absent "idle render-resource release (dropped with ABI v7) stays out" \
  'ReleaseAvioIdleResources|IdleResourceRelease|kIdleReleaseMinUnused|ReleaseIdle\(|ResourceManagerVK::Flush'
need "RenderTargetCache keys include the pixel format" \
  $F/impeller/renderer/render_target.h 'color_format == o\.color_format'
need "RenderTargetCache keys include storage modes" \
  $F/impeller/renderer/render_target.h 'color_storage == o\.color_storage'
need "RenderTargetCache attributes every miss" \
  $F/impeller/entity/render_target_cache.cc '"RenderTargetCacheMiss"'
need "the RenderTargetCache size counter carries real bytes" \
  $F/impeller/entity/render_target_cache.cc '"real_bytes",'
need "Flip allocates one single-sample secondary" \
  $F/impeller/entity/entity_pass_target.cc '"EntityPassTarget Secondary"'
need "a Canvas replay only leases render targets" \
  $F/impeller/display_list/canvas.cc \
  'render_target_scope_ = renderer_\.GetRenderTargetCache\(\)->BeginScope\(\)'
absent_in "a Canvas replay never ages the render-target cache" \
  $F/impeller/display_list/canvas.cc 'GetRenderTargetCache\(\)->(Start|End)\(\)'
need "the rasterizer ages the render-target cache once per frame" \
  $F/shell/common/rasterizer.cc 'cache->End\(\);'
need "a color-source rect containing the clip draws without an SDF mask" \
  $F/impeller/display_list/canvas.cc 'SDFFillRectContainsVisibleClip\(params, transform\)'
need "a per-draw image filter keeps the color-source SDF mask" \
  $F/impeller/display_list/canvas.cc 'per-draw image filter is excluded'
need "color-source SDF snapshots are single-sample without depth/stencil" \
  $F/impeller/display_list/canvas.cc '/\*depth_stencil_enabled=\*/false\)'
need "snapshots may omit depth/stencil" \
  $F/impeller/entity/contents/contents.h 'bool depth_stencil_enabled = true;'
need "UberSDF shades linear and radial gradients (backport)" \
  $F/impeller/display_list/canvas.cc 'CreateUberSDFGradientParameters\('
need "UberSDF storage-buffer gradient variant" \
  $F/impeller/entity/BUILD.gn 'shaders/uber_sdf_ssbo.frag'
need "UberSDF gradient edges keep the masked look" \
  $F/impeller/entity/shaders/uber_sdf_common.glsl \
  'frag_info.color_source_type < 0.5 \? color.rgb : vec3\(1.0\)'
need "UberSDF gradient pixel guard" \
  $F/impeller/display_list/aiks_dl_sdf_gradient_unittests.cc \
  'SdfGradientEdgesMatchMaskedComposite'
need "UberSDF dithers a gradient before coverage" \
  $F/impeller/entity/shaders/uber_sdf_common.glsl \
  'finishGradientColor\(IPPremultiply\(color\)\) \* alpha'
need "UberSDF uncovered gradient pixels stay transparent guard" \
  $F/impeller/display_list/aiks_dl_sdf_gradient_unittests.cc \
  'SdfGradientLeavesUncoveredQuadPixelsTransparent'
need "SDF contexts prewarm their variants at construction" \
  $F/impeller/entity/contents/content_context.cc \
  'PrewarmPipelineVariant\(variant\)'
need "prewarmed variants derive from the default descriptor asynchronously" \
  $F/impeller/entity/contents/content_context.cc \
  'std::make_unique<PipelineHandleT>\(context, desc, /\*async=\*/true\)'
need "prewarm covers patch 52's single-sample snapshots" \
  $F/impeller/entity/contents/content_context.cc \
  'PassOptions\(SampleCount::kCount1, targets.offscreen_format'
need "prewarm set equals what the draw sites compute" \
  $F/impeller/entity/contents/content_context_prewarm_unittests.cc \
  'SetIsWhatTheDrawSitesCompute'
need "first use finds the prewarmed variant guard" \
  $F/impeller/entity/contents/content_context_prewarm_unittests.cc \
  'FirstUseFindsThePrewarmedVariant'
need "Slimpeller low-memory path trims idle Impeller caches" \
  $F/shell/common/rasterizer.cc 'TrimIdleResourceCaches'
need "explicit transient profiles bypass environment policy" \
  $F/impeller/renderer/backend/vulkan/swapchain/transients_pool_vk.h \
  'allow_environment_override'
need "resource lifecycle feature is negotiated" \
  $F/shell/platform/embedder/embedder.h \
  'kFlutterAvioExtensionFeatureResourceLifecycleConfig'
need "per-view visibility is negotiated" \
  $F/shell/platform/embedder/embedder.h \
  'kFlutterAvioExtensionFeatureViewVisibility'
need "hidden admitted targets terminalize without removal" \
  $F/shell/common/animator.cc \
  'renderable_view_ids'
need "visibility never masquerades as global memory pressure" \
  $F/shell/platform/embedder/embedder_engine.cc \
  'rasterizer->TrimIdleResourceCaches\(\)'
need "resource lifecycle config is paired with negotiation" \
  $F/shell/platform/embedder/embedder.cc \
  'A resource lifecycle configuration was supplied without'
need "resource cache directory is a duplicated capability" \
  $F/shell/platform/embedder/embedder.cc \
  'fml::Duplicate\(config->pipeline_cache_directory_fd\)'
need "resource profile disables environment override" \
  $F/shell/platform/embedder/embedder.cc \
  'allow_environment_override = false'
need "pipeline cache is bounded before mapping" \
  $F/impeller/renderer/backend/vulkan/pipeline_cache_data_vk.cc \
  'GetRegularFileSize'
need "pipeline cache payload fits the mapped remainder" \
  $F/impeller/renderer/backend/vulkan/pipeline_cache_data_vk.cc \
  'data_size > available_data_bytes'
need "malformed sparse pipeline cache regression" \
  $F/impeller/renderer/backend/vulkan/pipeline_cache_data_vk_unittests.cc \
  'RejectsOversizedSparseCacheBeforeMapping'
need "pipeline cache schema is explicit" \
  $F/impeller/renderer/backend/vulkan/pipeline_cache_data_vk.h \
  'kPipelineCacheSchemaVersionVK'
need "Impeller pipeline cache ABI is explicit" \
  $F/impeller/renderer/backend/vulkan/pipeline_cache_data_vk.h \
  'kImpellerPipelineCacheABIVersionVK'
need "read-only pipeline cache never persists" \
  $F/impeller/renderer/backend/vulkan/pipeline_cache_vk.cc \
  'cache_access_ != PipelineCacheAccessVK::kReadWrite'
need "pipeline cache uses profile byte ceiling" \
  $F/impeller/renderer/backend/vulkan/pipeline_cache_vk.cc \
  'max_data_bytes_'

echo "--- Per-display scheduling opt-in ---"
need "waiter reports whether it can drive display-scoped batons" \
  $F/shell/common/vsync_waiter.h 'virtual bool SupportsPerDisplayVsync'
need "embedder opt-in derives from the per-display vsync callback" \
  $F/shell/platform/embedder/vsync_waiter_embedder.cc \
  'return static_cast<bool>\(vsync_for_display_callback_\)'
need "per-display mode requires that opt-in" \
  $F/shell/common/animator.cc \
  'per_display_opt_in_ && !display_states_\.empty\(\)'
need "unscoped requests fall through to the global frame clock" \
  $F/shell/common/animator.cc \
  'display_owns_a_view && default_state_\.renderable_view_ids\.empty\(\)'
need "display-registration starvation regression" \
  $F/shell/common/animator_unittests.cc \
  'DisplayRegistrationAloneDoesNotEnterPerDisplayMode'
need "unhomed-view starvation regressions" \
  $F/shell/common/animator_unittests.cc \
  'RegisteredDisplaysWithoutHomedViewsStillScheduleFrames'
absent_in "display registration alone flipping frame semantics" \
  $F/shell/common/animator.cc 'return !display_states_\.empty\(\);'
absent_in "Linux desktop embedder opting into per-display scheduling" \
  $F/shell/platform/linux/fl_engine.cc 'SetViewDisplay'
absent_in "Linux desktop view opting into per-display scheduling" \
  $F/shell/platform/linux/fl_view.cc 'SetViewDisplay'

echo "--- Cross-display pipeline conservation ---"
need "display frames reserve the shared raster pipeline" \
  $F/shell/common/animator.cc \
  'producer_continuation_ = state\.pipeline->Produce\(\)'
absent_in "per-display raster pipeline reservation" \
  $F/shell/common/animator.h \
  'ProducerContinuation producer_continuation;'

echo "--- External Vulkan image ownership ---"
need "typed external queue-family ownership" \
  $F/impeller/renderer/backend/vulkan/texture_source_vk.h \
  'struct ExternalImageOwnershipVK'
need "command-buffer external image acquire covers every pass" \
  $F/impeller/renderer/backend/vulkan/command_buffer_vk.cc \
  'PrepareExternalImage'
need "command-buffer external image release follows every pass" \
  $F/impeller/renderer/backend/vulkan/command_buffer_vk.cc \
  'ReleaseExternalImages'
need "external ownership covers the full texture descriptor" \
  $F/impeller/renderer/backend/vulkan/command_buffer_vk.cc \
  'ToArrayLayerCount\(texture->GetTextureDescriptor\(\)\)'
need "embedder ownership ABI field" \
  $F/shell/platform/embedder/embedder.h \
  'has_external_queue_family_ownership'

echo "--- Impeller visual correctness fixes ---"
need "UberSDF uses the upstream one-pixel antialiasing ramp" \
  $F/impeller/entity/contents/uber_sdf_parameters.h \
  'kAntialiasPixels = 1\.0f'
need "degree normalization half-open range guard" \
  $F/impeller/geometry/scalar.h \
  'if \(deg >= 360\.0f\)'
need "tiny negative degree regression test" \
  $F/impeller/geometry/arc_unittests.cc \
  'TinyNegativeStartNormalizesBelowFullCircle'

echo "--- GTK framebuffer multisampling ---"
need "GTK backing stores rasterize multisampled" \
  $F/shell/platform/linux/fl_engine.cc \
  'fl_framebuffer_new_multisampled'
need "multisample content is resolved before it is read" \
  $F/shell/platform/linux/fl_compositor_opengl.cc \
  'fl_framebuffer_resolve'
need "the framebuffer is the single authority on its colour format" \
  $F/shell/platform/linux/fl_engine.cc \
  'fl_framebuffer_get_sized_format\(framebuffer\)'
need "resolve source and destination formats match" \
  $F/shell/platform/linux/fl_framebuffer_test.cc \
  'ExplicitMultisampleResolveFormatsMatch'
need "a rejected resolve degrades to single sample" \
  $F/shell/platform/linux/fl_framebuffer_test.cc \
  'RejectedResolveFallsBackToSingleSample'
need "explicit multisample resolve regression" \
  $F/shell/platform/linux/fl_framebuffer_test.cc \
  'ExplicitMultisampleResolvesWithABlit'
need "unsupported multisample degrades to single sample" \
  $F/shell/platform/linux/fl_framebuffer_test.cc \
  'IncompleteMultisampleFallsBackToSingleSample'

echo "--- Removed paths must stay removed ---"
absent "RenderViewImmediate (forbidden, contract §8)" 'RenderViewImmediate'
absent "drain_tasks_now (forbidden support ABI)" 'drain_tasks_now|DrainTasksNow'
absent "RSS probe counters" 'g_reclaim_enqueued_total'


need "exact-frame preview feature" "$F/shell/platform/embedder/embedder.h" 'kFlutterAvioExtensionFeatureAtomicWindowPreviews'
need "retained preview scene API" "$F/lib/ui/compositing.dart" 'pushAvioWindowPreview'
need "preview target metadata" "$F/shell/platform/embedder/embedder.h" 'window_previews_count'

echo "--- Screen coefficient boundary ---"
need "shared Screen coefficient boundary" \
  "$F/impeller/geometry/color.h" 'kLastCoefficientBlendMode = BlendMode::kScreen'
need "Entity aliases the shared coefficient boundary" \
  "$F/impeller/entity/entity.h" 'kLastPipelineBlendMode = kLastCoefficientBlendMode'
need "Flow root readback uses the shared boundary" \
  "$F/flow/layers/display_list_layer.cc" 'impeller::kLastCoefficientBlendMode'
need "Atlas uses the shared coefficient boundary" \
  "$F/impeller/entity/contents/atlas_contents.cc" 'blend_mode <= kLastCoefficientBlendMode'
need "Screen regression records MSAA offscreens" \
  "$F/impeller/display_list/aiks_dl_backdrop_flip_unittests.cc" 'RenderTarget CreateOffscreenMSAA'
need "Screen shader image and vertex regression" \
  "$F/impeller/display_list/aiks_dl_backdrop_flip_unittests.cc" 'ScreenImageFiltersAndVerticesMatchCpuOracleWithoutOffscreen'
need "Screen old-fetch versus pipeline edge comparison" \
  "$F/impeller/display_list/aiks_dl_backdrop_flip_unittests.cc" 'ScreenPreviousFetchAndPipelineClippedEdgesGolden'
need "Screen design and open look gates" \
  docs/engine/impeller/docs/avio-screen-coefficient-design.md 'Pixel operator and acceptance'

echo "--- Explicit coverage policy and typed resource census ---"
need "appended antialiasing ABI configuration" \
  "$F/shell/platform/embedder/embedder.h" 'avio_antialiasing_config'
need "coverage and report negotiate distinct capabilities" \
  "$F/shell/platform/embedder/embedder.h" 'kFlutterAvioExtensionFeatureAntialiasingPolicy'
need "coverage implementation fact gates advertisement" \
  "$F/shell/platform/embedder/embedder.cc" 'kAvioCoveragePolicyImplemented'
need "integrated source coverage capability is enabled" \
  "$F/impeller/core/antialiasing_policy.h" 'kAvioCoveragePolicyImplemented = true'
need "unknown or truncated coverage config fails closed" \
  "$F/shell/platform/embedder/avio_antialiasing_config.h" 'Antialiasing config was truncated'
need "native coverage resources are checked before allocation" \
  "$F/impeller/renderer/backend/vulkan/context_vk.cc" 'SupportsAvioCoverageResources'
need "context refusal reaches the engine initialization boundary" \
  "$F/shell/platform/embedder/embedder.cc" 'Could not initialize Vulkan Impeller surface'
need "root coverage wraps imported color without auxiliary attachments" \
  "$F/shell/platform/embedder/embedder.cc" 'MakeAvioCoverageRootTarget'
need "root target imported-source lifetime regression" \
  "$F/shell/platform/embedder/avio_coverage_root_target_unittests.cc" 'CoverageRootRetainsImportedColorWithoutAuxiliaryAttachments'
need "bounded coverage and layer region implementation" \
  "$F/impeller/entity/avio_coverage_region.cc" 'AvioCoverageRegion::'
need "physical sample4 path atlas" \
  "$F/impeller/entity/contents/coverage_path_atlas.cc" 'AcquirePathMask'
need "coverage clip/path cache" \
  "$F/impeller/entity/contents/coverage_mask_cache.h" 'CoverageMaskCache'
need "deferred tiled coverage render pass" \
  "$F/impeller/entity/coverage_tiled_render_pass.cc" 'CoverageTiledRenderPass::'
need "tiled coverage executor implementation is in GN" \
  "$F/impeller/entity/BUILD.gn" '"coverage_tiled_render_pass.cc"'
need "tiled coverage executor header is in GN" \
  "$F/impeller/entity/BUILD.gn" '"coverage_tiled_render_pass.h"'
need "tiled coverage executor regressions are in GN" \
  "$F/impeller/entity/BUILD.gn" '"coverage_tiled_render_pass_unittests.cc"'
need "non-AA and filter subpasses have an explicit 1x route" \
  "$F/impeller/entity/contents/content_context.cc" 'CoverageTiledRenderPass::MakeDirect1x'
need "common coverage pipelines are initialized before raster frames" \
  "$F/impeller/entity/contents/content_context.cc" 'PrewarmAvioCoveragePipelines'
need "upstream async variant authority is preserved" \
  "$F/impeller/entity/contents/content_context.cc" 'void Prewarm\(const Context& context'
need "legacy asynchronous prewarm catalogue API is preserved" \
  "$F/impeller/entity/contents/content_context.h" 'MakeAvioPrewarmVariants'
need "coverage queues all actual keys before validation" \
  "$F/impeller/entity/contents/content_context.cc" 'PrewarmAvioPipelineKeysInTwoPhases'
need "async clip variants preserve their real descriptor" \
  "$F/impeller/entity/contents/content_context.cc" 'clip\.SetDefaultDescriptor\(clip_pipeline_descriptor\)'
need "queue/join ordering regression" \
  "$F/impeller/entity/contents/avio_pipeline_prewarm_unittests.cc" 'QueuesAllKeysBeforeJoiningTheSameKeys'
need "failed async join never publishes Ready" \
  "$F/impeller/entity/contents/avio_pipeline_prewarm_unittests.cc" 'AsyncJoinFailureKeepsPublicStateUnready'
need "upstream legacy prewarm regressions remain registered" \
  "$F/impeller/entity/BUILD.gn" '"contents/content_context_prewarm_unittests.cc"'
need "runtime Canvas selects coverage explicitly" \
  "$F/impeller/display_list/canvas.cc" 'UsesAvioCoverage'
need "shared backdrop readers retain a frozen prefix" \
  "$F/impeller/display_list/canvas.cc" 'backdrop_data->frozen_prefix'
need "logical snapshots retain resource custody" \
  "$F/impeller/renderer/snapshot.h" 'resource_owner'
need "resource report uses bounded storage" \
  "$F/impeller/renderer/render_resource_report.h" 'kMaxEntries = 64u'
need "typed report entry point" \
  "$F/shell/platform/embedder/embedder.h" 'FlutterEngineRequestAvioRenderResourceReport'
need "asynchronous report custody closes before shell teardown" \
  "$F/shell/platform/embedder/embedder_engine.cc" 'requests->Close'
need "reports keep event units distinct from draw reasons" \
  "$F/shell/platform/embedder/embedder.h" 'layer_region_overflow_real_bytes'
need "physical image source lifetime census" \
  "$F/impeller/renderer/backend/vulkan/allocator_vk.cc" 'AllocatedImageLedger'
need "native pipeline lifetime census" \
  "$F/impeller/renderer/backend/vulkan/pipeline_vk.h" 'resource_registration_'
need "frame origin survives asynchronous pipeline compilation" \
  "$F/impeller/renderer/backend/vulkan/pipeline_library_vk.cc" 'AvioPipelineCreationOrigin::Capture\(\)'
need "renderer ledger/report contracts are in GN" \
  "$F/impeller/renderer/BUILD.gn" 'render_resource_contracts'
need "standalone production coverage contracts are in GN" \
  "$F/shell/platform/embedder/BUILD.gn" 'avio_coverage_contract_tests'
need "coverage design and honest packaging gates" \
  AVIO_PATCHES.md 'Patch 55b: explicit coverage policy and resource census'



echo "--- Exact empty content and root frame facts ---"
need "strict native root fact validation" \
  "$F/flow/layers/avio_frame_metadata_layer.cc" 'CollectAvioRootFrameFacts'
need "root fact layer regression inventory" \
  "$F/flow/BUILD.gn" '"layers/avio_frame_metadata_layer_unittests.cc"'
need "bufferless empty path precedes ordinary surface acquisition" \
  "$F/shell/common/rasterizer.cc" 'SubmitAvioEmptyFrame'
need "empty path releases cached root custody" \
  "$F/shell/platform/embedder/embedder_external_view_embedder.cc" 'render_target_caches_.erase\(view_id\)'
need "exact empty terminal and next-target regression" \
  "$F/shell/platform/embedder/embedder_external_view_embedder_unittests.cc" 'EmptyRootReportsOneExactFrameWithoutTargetAndTransitionsNormally'
need "root facts force ordinary raster on changed revision" \
  "$F/shell/common/rasterizer.cc" 'root_facts_changed'
need "bufferless surface/target regression" \
  "$F/shell/common/rasterizer_unittests.cc" 'EmptyRootSkipsSurfaceAndTargetThenContentAcquiresNormally'
need "bounded logical split ground" \
  "$F/shell/platform/embedder/embedder.h" 'FLUTTER_AVIO_MAX_OUTPUT_GROUND_REGIONS 4u'
need "borrowed allocation-free frame facts" \
  "$F/shell/platform/embedder/avio_frame_facts.h" 'class EmbedderAvioFrameFacts'
need "retained root framework alpha" \
  packages/flutter/lib/src/rendering/avio_item_effect.dart 'class RenderAvioItemEffect'
need "root alpha retains child painting regression" \
  packages/flutter/test/widgets/avio_item_effect_test.dart 'root fade retains child painting'
need "ready Static/Live content exact identity" \
  "$F/shell/platform/embedder/embedder.h" 'FlutterAvioReadyContent'
need "nullable ready framework author" \
  packages/flutter/lib/src/rendering/avio_item_effect.dart 'class RenderAvioReadyContent'
need "root facts design note and release gates" \
  AVIO_PATCHES.md 'Patch 56: exact empty content'
need "root effect declaration identity is carried with revision" \
  "$F/shell/platform/embedder/avio_frame_facts.h" 'item_effect_declaration_id'
need "raster spans bind actual submitted Flutter view" \
  "$F/shell/platform/embedder/embedder_external_view.cc" 'view_id_string \+ 31, flutter_view_id'

echo "--- GTK bounded native Coverage ---"
need "native GL admission exercises actual sample identity" \
  "$F/impeller/renderer/backend/gles/native_coverage_gles.h" 'colour_samples == 4 && stencil_samples == 4'
need "host GL root is validated as single-sample color-only" \
  "$F/shell/platform/embedder/embedder.cc" 'ValidateCoverageFramebufferGLES'
need "GTK uses color-only backing stores under admitted Coverage" \
  "$F/shell/platform/linux/fl_engine.cc" 'fl_framebuffer_new_color_only'
need "GTK probe is linked as its actual header-only dependency" \
  "$F/shell/platform/linux/BUILD.gn" 'gles:native_coverage_probe'
need "native GL capability regression is registered" \
  "$F/impeller/renderer/backend/gles/BUILD.gn" '"native_coverage_gles_unittests.cc"'
need "GL masks are deliberately uncached native replay" \
  "$F/impeller/entity/contents/content_context.cc" 'cache_native_masks = false'
need "descriptor-only region mode excludes invented physical bytes" \
  "$F/impeller/entity/avio_coverage_region.cc" 'config.require_exact_allocated_bytes'
need "fixed prefix scratch prevents wrapped-FBO sampling" \
  "$F/impeller/entity/coverage_tiled_render_pass.cc" 'GetColourSeedTexture'
need "wrapped GL framebuffers remain borrowed during blits" \
  "$F/impeller/renderer/backend/gles/blit_command_gles.cc" 'FramebufferBinding\{\*fbo, false\}'
need "public report distinguishes physical descriptor multiplicity" \
  "$F/shell/platform/embedder/embedder.h" 'kFlutterAvioResourceFieldDescriptorMultiplicity 0x10ULL'
need "actual allocator advertises observed descriptor multiplicity" \
  "$F/impeller/renderer/allocated_image_ledger.cc" 'kAvioResourceFieldDescriptorMultiplicity'
need "lease availability is not inferred from allocator ownership" \
  "$F/impeller/renderer/allocated_image_ledger_unittests.cc" 'PhysicalDescriptorMultiplicityIsAvailableWithoutLeaseInference'
need "GLES accounting and native release gates are documented" \
  AVIO_PATCHES.md 'Patch 58: GTK/GLES bounded native-four-sample Coverage'
echo "--- Headless Vulkan golden execution contract ---"
need "Linux Vulkan golden target is registered" \
  "$F/impeller/golden_tests/BUILD.gn" 'impeller_golden_tests_vk'
need "Linux Vulkan golden backend replaces the skip-only stub" \
  "$F/impeller/golden_tests/BUILD.gn" 'golden_playground_test_vk.cc'
need "golden readback waits for actual submission completion" \
  "$F/impeller/golden_tests/golden_renderer_vk.cc" 'WaitWithTimeout'
need "aliased reference constrains nested targets too" \
  "$F/impeller/golden_tests/golden_renderer_vk.cc" 'class SingleSampleGoldenAllocator'
need "golden edge rule asserts exact interior bytes" \
  "$F/impeller/golden_tests/golden_edge_comparison.h" 'max_interior_delta == 0'
need "missing and skipped golden execution cannot pass" \
  "$F/impeller/golden_tests/main_vk.cc" 'skipped_test_count\(\) != 0'
need "Screen prior-fetch comparison fails any interior delta" \
  "$F/impeller/display_list/aiks_dl_backdrop_flip_unittests.cc" 'EXPECT_EQ\(max_interior_delta, 0\)'
need "headless golden release gates are documented" \
  AVIO_PATCHES.md 'Patch 59: headless Linux Vulkan golden harness'

echo "--- Explicit continuous classes and native4 destination coverage ---"
need "backend implementation masks remain independent" \
  "$F/impeller/core/antialiasing_policy.h" 'AvioContinuousSupportedClasses\(AvioCoverageBackend'
need "immutable bounded analytic expression" \
  "$F/impeller/core/continuous_coverage.h" 'kAvioContinuousMaxClips = 16'
need "retained clip state reaches actual recorder" \
  "$F/impeller/entity/coverage_tiled_recorder.cc" 'pending_.continuous_clip = avio_continuous_clip_'
need "raw filtered own geometry reaches final native draw" \
  "$F/impeller/display_list/canvas.cc" 'SetAvioContinuousGeometryPrimitive'
need "native destination prefix stays in the bounded bank" \
  "$F/impeller/entity/avio_coverage_region_unittests.cc" 'ContinuousPrefixCannotEscapeActualByteCap'
need "ordered destination copy occurs outside render passes" \
  "$F/impeller/entity/coverage_tiled_render_pass.cc" 'AddCopy\(island_.GetColorAttachment\(0\).texture, prefix\)'
need "destination operator preserves fractional coverage" \
  "$F/impeller/entity/shaders/continuous_coverage.glsl" 'mix\(destination, AvioApplyOriginalBlend\(source, destination\), coverage\)'
need "continuous shader source inventory is Vulkan-only" \
  "$F/impeller/entity/BUILD.gn" 'vulkan_only_shaders ='
need "continuous control tests are registered" \
  "$F/impeller/renderer/BUILD.gn" 'continuous_coverage_pipeline_unittests.cc'
need "continuous evaluator has one inlined call site per variant" \
  "$F/impeller/entity/shaders/continuous_clip.glsl" 'taps\[tap\]=AvioContinuousLocalDistance\(v,tap_position\)'
need "own geometry and clips share one evaluator loop" \
  "$F/impeller/entity/shaders/continuous_coverage.glsl" 'slot = avio_control.runtime.y > 0.5 \? -1 : 0'
absent_in "no per-use continuous evaluator inlining" \
  "$F/impeller/entity/shaders/continuous_clip.glsl" 'AvioContinuousLocalDistance\(v,local[+-]'
need "continuous SPIR-V budget and evaluator fixture are registered" \
  "$F/impeller/golden_tests/avio_continuous_shader_goldens_vk.cc" 'VariantsStayWithinSpirvBudget'
need "borrowed logical-device enablement is explicit ABI data" \
  "$F/shell/platform/embedder/embedder.h" 'bool native_sample_shading_enabled'
need "selected Coverage target fixtures remain registered" \
  "$F/shell/platform/embedder/tests/embedder_vk_unittests.cc" 'CoverageSelectedTargetDamageKeepsNativeFourEdge'
need "Ready live content is distinct from static sealability" \
  "$F/shell/platform/embedder/embedder.h" 'kFlutterAvioReadyContentKindLive = 1'
need "continuous expression pool is cold and bounded" \
  "$F/impeller/entity/contents/continuous_clip_pool.h" 'kCapacity = 128'
need "continuous recorder upload cache is fixed" \
  "$F/impeller/entity/coverage_tiled_render_pass.h" 'std::array<ClipBuffer, kMaxContinuousStates>'
need "continuous data arena is prewarmed and bounded" \
  "$F/impeller/entity/contents/content_context.cc" 'HostBuffer::CreateBounded'
need "bounded host storage preserves pending/submitted owners" \
  "$F/impeller/core/host_buffer.cc" 'buffer.use_count\(\) != 1'
need "bounded host storage tests are registered" \
  "$F/impeller/core/BUILD.gn" 'host_buffer_bounded_unittests.cc'
need "native buffer lifetime ledger is attached to deferred resources" \
  "$F/impeller/renderer/backend/vulkan/device_buffer_vk.h" 'AllocatedBufferLedger::Registration registration'
need "native buffer census is merged once by the context" \
  "$F/impeller/renderer/backend/vulkan/context_vk.cc" 'GetAllocatedBufferReport\(start_new_interval\)'
need "native buffer census tests are registered" \
  "$F/impeller/renderer/BUILD.gn" 'allocated_buffer_ledger_unittests.cc'
need "continuous source design and release gates are documented" \
  AVIO_PATCHES.md 'Patch 60: opt-in continuous classes'


echo "--- Exact declaration clip strategies and bounded typed recording ---"
need "runtime DisplayList pre-pass is called before dispatch" \
  "$F/impeller/display_list/dl_dispatcher.cc" 'ClassifyCoverageDisplayList'
need "mixed painter ranges consume exact clip declarations" \
  "$F/impeller/entity/coverage_tiled_render_pass.cc" 'EncodeCertifiedClip\(writable, true\)'
need "geometry/tag completeness guards recorded substitutions" \
  "$F/impeller/entity/coverage_tiled_recorder.cc" 'pending_.sample4_clip_complete = avio_sample4_clip_complete_'
need "logical clip operations are distinguishable from own stencil" \
  "$F/impeller/entity/coverage_tiled_recorder.cc" 'pending_.clip_operation = IsAvioClipOperation\(\)'
need "all-depth mask recipes stream through existing scratch" \
  "$F/impeller/entity/contents/coverage_clip_mask_flattener.cc" 'AcquireClipMaskScratch'
need "shape fringe/interior plan is actually consumed" \
  "$F/impeller/entity/coverage_tiled_render_pass.cc" 'GetFringePlan\(writable\)'
need "unknown shape full pixels and partial lanes remain distinct" \
  "$F/impeller/entity/shaders/sample4/avio_sample4_fringe_solid_fill.frag" 'gl_SampleMask\[0\] = int\(mask\)'
need "full ancestor recipe cannot be truncated to newest K4" \
  "$F/impeller/entity/contents/sample4_clip_uniform.cc" 'clip.parent'
need "captured source evidence reaches real packets" \
  "$F/impeller/entity/coverage_tiled_recorder.cc" 'pending_.sample4_source_proof = sample4_source_proof_'
need "captured source rounding is materialized before joint mask" \
  "$F/impeller/entity/coverage_tiled_render_pass.cc" 'opaque_group.IsValid\(\)'
need "CPU recorder bank is cold and bounded" \
  "$F/impeller/entity/coverage_recorder_bank.h" 'kPackets = 8192'
need "bounded coverage storage is uninitialized capacity, not preallocation" \
  "$F/impeller/entity/coverage_recorder_storage.h" 'alignas\(T\) std::byte storage_\[Capacity \* sizeof\(T\)\];'
need "recorder bank is not value-initialized by make_shared" \
  "$F/impeller/entity/coverage_recorder_bank.h" 'CoverageRecorderStorage\(\) \{\}'
need "recorder bank constructs controls on demand" \
  "$F/impeller/entity/coverage_recorder_bank.h" 'CoverageFixedVector<Control, kControls> controls'
need "pre-pass plan uses the one fixed-vector authority" \
  "$F/impeller/display_list/coverage_classifier.h" 'CoverageFixedVector<CoverageClipDecision, 512> clips_'
absent_in "pre-pass plan has no second fixed-vector template" \
  "$F/impeller/display_list/coverage_classifier.h" 'class CoverageFixedVector'
need "coverage CPU storage census kind is reported" \
  "$F/impeller/display_list/aiks_context.cc" 'AvioRenderResourceKind::kCoverageCpuStorage'
need "rasterizer reports through the aiks census" \
  "$F/shell/common/rasterizer.cc" 'aiks->GetAvioRenderResourceReport\(start_new_interval\)'
need "coverage CPU storage residency tests are registered" \
  "$F/impeller/entity/coverage_recorder_bank_unittests.cc" 'CapsAreFixedAddressSpaceNotResidentMemory'
need "recorder storage is registered in GN" \
  "$F/impeller/entity/BUILD.gn" '"coverage_tiled_recorder.cc"'
need "source proof production fixtures are registered" \
  "$F/impeller/entity/BUILD.gn" 'sample4_source_proof_unittests.cc'
need "exact clip strategy refinements are documented" \
  AVIO_PATCHES.md 'Patch 61: exact clip segments and bounded typed recording'
need "encoded source sampling uses high precision" \
  "$F/impeller/entity/shaders/sample4/avio_sample4_layer_texture_fill.frag" 'uniform highp sampler2D texture_sampler'
need "encoded source reconstructs stored byte codes" \
  "$F/impeller/entity/shaders/sample4/avio_sample4_layer_texture_fill.frag" 'round\(textureLod'
need "captured group native rounding fixture remains registered" \
  "$F/impeller/golden_tests/BUILD.gn" 'avio_captured_backdrop_goldens_vk.cc'
need "foreign callback baton follows native image source" \
  "$F/shell/platform/embedder/embedder.cc" 'EmbedderExternalResourceCustody custody_'
need "foreign view is released after native frame cache" \
  "$F/shell/platform/embedder/embedder.cc" 'ReleaseCachedFrameData\(\)'
need "selected render targets require proved completion fd" \
  "$F/shell/platform/embedder/embedder_render_target_impeller.cc" 'requires_render_complete_sync_fd_'
need "required missing completion fd settles a failed frame" \
  "$F/shell/platform/embedder/embedder_external_view_embedder.cc" 'RequiresRenderCompleteSyncFD\(\)'
need "actual native-cache callback regression remains registered" \
  "$F/shell/platform/embedder/BUILD.gn" 'embedder_external_resource_custody_unittests.cc'
need "legacy analytic source choice is independent of target routing" \
  "$F/impeller/display_list/canvas.cc" 'UseLegacySdfSource'
need "legacy analytic mixed-scope source contract remains registered" \
  "$F/impeller/display_list/BUILD.gn" 'legacy_analytic_source_unittests.cc'
need "mixed analytic native source keys are cold-warmed" \
  "$F/impeller/entity/contents/avio_pipeline_prewarm_unittests.cc" 'MixedNativePassesKeepTheirAnalyticSourceKeys'
need "mixed glyph clip and SDF native fixture remains authored" \
  "$F/impeller/golden_tests/avio_coverage_goldens_vk.cc" 'MixedAnalyticSourcesUseColdNativeKeys'
need "continuous Circle exports original distance before coverage" \
  "$F/impeller/entity/shaders/circle.frag" 'avio_geometry_distance = sdf_distance'
need "continuous complex superellipse exports original distance" \
  "$F/impeller/entity/shaders/complex_rse.frag" 'avio_geometry_distance = sdf /'
need "translated native replay preserves original raster phase" \
  "$F/impeller/entity/coverage_tiled_render_pass.cc" 'PreservesOriginalRasterPhase\(origin\)'
need "native dither and derivative tile phase are tested" \
  "$F/impeller/entity/contents/coverage_atlas_unittests.cc" 'NativeDitherAndDerivativePhaseSurviveEveryTile'

echo "--- Patch 62: exact cached render-pass policy ---"
need "cached render passes are keyed by their exact attachment policy" \
  "$F/impeller/renderer/backend/vulkan/render_pass_vk.cc" 'cache_mip_level, cache_slice, &policy\)'
need "a later Coverage segment loads after an earlier clear (test)" \
  "$F/impeller/renderer/backend/vulkan/render_pass_vk_unittests.cc" 'LaterSegmentLoadsTheParentAfterAnEarlierClear'
need "exact render-pass policy is documented" \
  AVIO_PATCHES.md 'Patch 62: exact attachment policy for cached Vulkan render passes'

echo "--- Patch 63: a diff baseline is a tree that was diffed ---"
need "frame damage refuses a previous tree without paint regions" \
  "$F/flow/compositor_context.cc" 'prev_layer_tree_->has_paint_regions\(\)'
need "an undiffed previous tree repaints the whole frame (test)" \
  "$F/flow/diff_context_unittests.cc" 'FrameDamageRepaintsWholeAfterAnUndiffedPreviousTree'
need "diff baseline rule is documented" \
  AVIO_PATCHES.md 'Patch 63: a diff baseline is a tree that was diffed'

echo "--- Patch 64: a root-facts change is whole-target catch-up damage ---"
need "a root-facts change adds whole-frame catch-up damage" \
  "$F/shell/common/rasterizer.cc" 'damage->AddAdditionalDamage\(DlIRect::MakeSize\(layer_tree.frame_size\(\)\)\)'
absent_in "a root-facts change never skips the diff" \
  "$F/shell/common/rasterizer.cc" 'supports_partial_repaint && !root_facts_changed'
need "facts-only frame rasters whole with exact frame damage (test)" \
  "$F/shell/common/rasterizer_unittests.cc" 'rootFactsOnlyChangeRastersWholeTargetWithExactFrameDamage'
need "facts frame is a diffed baseline (test)" \
  "$F/shell/common/rasterizer_unittests.cc" 'frameAfterRootFactsChangeDiffsNarrowly'
need "removed child after a facts change damages its old region (test)" \
  "$F/shell/common/rasterizer_unittests.cc" 'rootFactsChangeThenRemovedChildDamagesOnlyItsOldRegion'
need "unaccepted facts change is resent (test)" \
  "$F/shell/common/rasterizer_unittests.cc" 'rootFactsChangeNotAcceptedIsResentNextOpportunity'
need "metadata path never treats a facts change as no-change (test)" \
  "$F/shell/common/rasterizer_unittests.cc" 'metadataPathRootFactsChangeIsNotNoVisualChange'
need "catch-up damage leaves frame damage exact (test)" \
  "$F/flow/diff_context_unittests.cc" 'FrameDamageCatchUpLeavesFrameDamageExact'
need "root-facts catch-up damage is documented" \
  AVIO_PATCHES.md 'Patch 64: a root-facts change is whole-target catch-up damage'

echo
[ $fail -eq 0 ] && echo "ALL PATCHES PRESERVED" || echo "FAILURES DETECTED"
exit "$fail"
