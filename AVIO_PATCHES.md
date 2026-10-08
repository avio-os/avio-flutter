# Avio Engine Patch Stack

This fork of flutter/flutter carries Avio's engine + framework patches as a
curated, rebased stack (the flutter-tizen model). Canonical branch naming:
`avio/<upstream-ref>` (e.g. `avio/main-2026-08`); the published fork uses
`main`. Every commit subject starts with `[avio]` (`[avio][framework]` for
packages/flutter changes), except commits which deliberately retain an
upstreamable `fix`/`feat` subject or preserve an original cherry-pick subject.

Contract for what these patches may and may not do:
`avio/docs/engine-contract.md` in the Avio repo.

## Current upstream baseline

The 2026-08 refresh was performed at the Flutter 3.44.8 stable cutoff:

| Identity | Pin |
|----------|-----|
| Official stable release | Flutter 3.44.8, `058e0af2c2b57e369d905a03ac9748b0ebf543c6` (2026-07-23) |
| Avio rebase target | `upstream/main` at `5a2a94a5a971471ad940709c75463b0798df7e5c` (2026-08-03) |
| Previous upstream base | `b79192e735bb13bfcb20f982689e9792d5c485cf` |
| Pre-refresh rollback tag | `avio-pre-flutter-3.44.8-2026-08-03` |

This fork follows upstream `main` at stable cutoffs; it does not merge the
release branch into main. Flutter stable is a release branch with selected
cherry-picks, not a newer linear ancestor: the previous Avio main base was
already 915 main commits ahead of the stable branch point while stable carried
78 branch-only commits. Rebasing onto the stable release commit would therefore
discard newer mainline Engine work. The stable-only delta must instead be
audited for fixes not already represented on main. For this refresh, the
relevant Impeller fixes for AHB swapchain teardown (`145475453cbe`), GLES
texture cleanup (`d742b87b7836`), and text-shadow masks (`308ba65eaeadd`) were
already ancestors of the selected main target under their original commits.

## Patch inventory

| # | Patch | Kind | Upstream replacement? |
|---|-------|------|----------------------|
| 1 | Add Vulkan Impeller backing store support for embedder API | permanent ABI extension | none |
| 2 | Add DMA-BUF external textures with GPU-side acquire fences | permanent ABI extension | open: flutter/flutter#117937 |
| 3 | Multi-rect damage tracking with DlRegion and per-buffer Vulkan support | permanent (upstreamable in principle) | open: flutter/flutter#109724 |
| 4 | Synchronous-resize plumbing, texture dirty awareness, integration fixes | permanent (configure_serial gating is live; blocking API removed by #13) | none |
| 5 | Per-display scheduling, empty-frame callback, EVE frame-damage metadata | permanent ABI extension | none |
| 6 | cherry-pick: dispose thread-local Vulkan caches in embedder compositor path | temporary — drop when upstream lands the EVE-path fix (original PR flutter/flutter#183268 never merged; #182265/#182402 cover other paths only) | watch |
| 7 | fix(embedder): make PublishDmabufTexture non-blocking | upstreamable bugfix | submit upstream |
| 8 | feat(embedder): expose render_complete_sync_fd via ExternalSemaphoreVK | permanent ABI extension (explicit sync) | none — no upstream fence/sync surface exists |
| 9 | Typed shell layer targets and per-window chrome presentation | permanent ABI extension | none |
| 10 | Vulkan Impeller embedder lifetime hardening and Linux build fixes | permanent | partially subsumed (SDF flags became stock `Settings::impeller_use_sdfs` + `impeller::Flags` in 2026-05; our plumbing was dropped at the 2026-06 rebase) |
| 11 | Timeline-semaphore Vulkan completion and ContextVK-owned transients pool | permanent | none |
| 12 | View-scoped frame scheduling via PlatformDispatcher.scheduleFrameForDisplayViews | permanent dart:ui extension | none |
| 13 | Remove synchronous immediate-frame resize path and stale RSS probes | permanent deletion (contract §8) | n/a |
| 14 | [framework] Wire engine per-display scoped frame callbacks into framework | permanent framework extension | none |
| 15 | [framework] Route Dart-originated scheduleFrame through per-display scoped engine API | permanent framework extension | none |
| 16 | [framework] Fix Phase 2 ordering bug — register dirty view BEFORE first scheduleFrame | permanent framework fix | none |
| 17 | [framework] Make unattributable dirty marks force the global frame path | permanent framework fix | none |
| 18 | [framework] Fall back to global frame path when a view's display is unregistered | permanent framework fix | none |
| 19 | [framework] Guard the remaining display lookup in dirty-view forwarding | permanent framework fix | none |
| 20 | [framework] Make MouseTracker device-update phase exception-safe | upstreamable bugfix | submit upstream |
| 21 | Make DlRegion total over empty rect inputs | upstreamable bugfix (latent upstream infinite loop) | submit upstream |
| 22 | [framework] Preserve scoped render authority and schedule residual view work | permanent framework correctness fix; input-queue portion superseded by #31 | none while Avio carries view-scoped frame admission |
| 23 | Explicit root-target compositor mode and semantic extension negotiation | permanent ABI extension | none |
| 24 | Engine-local vsync leases and exact per-target frame-opportunity terminality | permanent ABI/lifecycle extension | none |
| 26 | Share the raster pipeline reservation across displays | permanent while display-scoped scheduling drains through one raster pipeline | none |
| 27 | Transfer external Vulkan image queue-family ownership | permanent explicit-sync/ownership extension | none |
| 28 | Keep normalized degrees below a full circle | upstreamable Impeller correctness fix | submit upstream |
| 29 | [framework] Maintain typed O(1) element-to-view ownership | permanent framework correctness/performance fix | none while Avio carries view-scoped frame admission |
| 30 | [framework] Route texture frames to their exact render consumers | upstream-aligned framework/engine fix adapted for compositor pacing | open: flutter/flutter#179874 |
| 31 | [framework] Give each View an independently admitted BuildScope | permanent framework correctness fix; deletes #22's deferred-input compensation | none while Avio carries view-scoped frame admission |
| 32 | Acquire the exact embedder target before damage and keep its root pass multisampled | permanent ABI/rendering extension | none |
| 33 | Bound pipeline-cache files before mapping or allocation | permanent defensive I/O contract | upstreamable in principle |
| 34 | Enforce hard transient budgets and trim only idle resources | permanent resource-lifecycle contract | none |
| 35 | Negotiate exact resource profiles and principal-scoped pipeline-cache access | permanent ABI/lifecycle extension | none |
| 36 | Suppress raster demand with exact per-view visibility | permanent ABI/lifecycle extension | none |
| 37 | Carry retained compositor-material nodes with the exact Flutter frame | permanent ABI/scene extension | none |
| 38 | Keep a refused render target's frame demand instead of stranding the view | permanent lifecycle correctness fix | none |
| 39 | Replace ambiguous target refusal with exact typed acquisition outcomes | permanent lifecycle correctness fix | none |
| 40 | Bind a newly added view to its initial display before first-frame scheduling | permanent per-display lifecycle fix | none |
| 41 | Report exact view-scoped frame-request acceptance to the framework scheduler | permanent framework/engine lifecycle fix | none |
| 42 | Give the GTK framebuffer real multisampling | upstreamable bugfix — attach to flutter/flutter#191171 | open: flutter/flutter#191171, flutter/flutter#191234 |
| 44 | Carry typed analytic clips with retained compositor materials | permanent ABI/scene extension | none |
| 45 | Author semantic foreground coverage for an external linear-light backdrop | permanent composition-contract extension | none — Flutter otherwise cannot know that its transparent target receives a backdrop later |
| 46a | One transient attachment set per key on the single graphics queue | permanent resource-lifecycle correction (restores patch 11's one-entry-per-key pool; the incoming depth/stencil dependency is upstreamable) | partial: flutter/flutter#144617 recycles one onscreen set upstream |
| 46b | A transient set lives while an existing view holds its extent | permanent resource-lifecycle owner (ships only with Avio G2, which keeps a ShellItem's view across extent changes) | none — upstream frees with the swapchain |
| 46u | Report-only render-resource accounting | permanent diagnostics (internal C++ API, no ABI change) | none |
| 47 | RenderTargetCache complete keys and miss telemetry | upstreamable bugfix + diagnostics (offer on flutter/flutter#190613) | open: flutter/flutter#190613 |
| 53 | Backport single-sample backdrop restore safety and shared-backdrop content depth | temporary backport — drop when rebasing past flutter#193306 and #193176 | merged upstream: flutter/flutter#193306, #193176 |
| 54 | Screen is a coefficient blend | upstreamable memory/performance correction | submit upstream |
| 55b | Explicit coverage AA policy, bounded coverage/layer regions, and typed resource reports | permanent opt-in ABI/rendering contract (ABI9, introduced in v8); integrated source capability enabled, native release checks pending | none |
| 56 | Exact bufferless empty content, root revision opacity and bounded output ground | permanent opt-in ABI9/scene/framework contract | none |
| 57 | Ready Static/Live content identity with explicit Static-only sealing | permanent opt-in ABI9/scene/framework contract | none |
| 58 | GTK/GLES bounded native-four-sample Coverage with honest descriptor-only accounting | permanent negotiated backend contract; source checks, native/GPU release checks pending | none |
| 59 | Headless Linux Vulkan goldens and strict native4/Coverage/aliased1 edge comparisons | test-only TL3 renderer and fixtures; source/CPU checks, full native/GPU execution pending | none |
| 60 | Opt-in continuous classes with joint shape/clip coverage and native4 destination prefixes | permanent explicit Vulkan class contract; default mask zero, native/GPU approval gates pending | none |
| 61 | Exact clip segments, full native recipes, bounded typed recording and native submission custody | permanent bounded rendering/lifetime contract; source and CPU checks, native/GPU release checks pending | none |
| 62 | A cached Vulkan render pass is replayed only for its exact attachment policy | upstreamable correctness fix (latent upstream; fatal for the single-sample Coverage root) | submit upstream |
| 48 | Flip allocates a single-sample secondary | upstreamable memory fix | submit upstream |
| 51 | RenderTargetCache ages once per raster frame | upstreamable correctness fix (offer on flutter/flutter#190613) | open: flutter/flutter#190613 |
| 52 | SDF colour sources: no mask when the shape contains the clip; single-sample snapshots otherwise | upstreamable memory/performance fix | submit upstream |
| 50 | cherry-pick: shade linear and radial gradients inside UberSDF (flutter#192124, #192962) | temporary backport — drop at the next rebase onto a base that contains #192124 and #192962, after re-checking the two Avio deltas below | merged upstream: flutter/flutter#192124, #192962 |
| 55a | Prewarm the pipelines Avio's SDF and single-sample patches use | permanent startup-latency extension (tied to patches 48, 50 and 52) | none — upstream compiles every variant on first use |

Patch #5 also owns the later exact empty-frame and global-request corrections:
global requests may not be consumed by a display-scoped frame; sibling-render,
PipelineFull, unhomed-view, unresolved-view, and cached-tree-reuse exits must
terminalize the affected scoped request exactly once. The intermediate
revert/reapply commits preserve review history but do not define separate
runtime contracts.

### Patch 5: per-display scheduling is an embedder opt-in

Display-scoped frame driving belongs only to embedders that can consume a
baton naming a display. Two signals grant it, and only these two: a
`vsync_for_display_callback` supplied at engine start, which the waiter owns
and reports through `VsyncWaiter::SupportsPerDisplayVsync`, or the first
`FlutterEngineSetViewDisplay` homing a view on a display. Display registration
is deliberately not a signal. `FlutterEngineNotifyDisplayUpdate` is stock
upstream API describing topology, and every GTK application calls it through
`fl_display_monitor`; letting it flip the frame-driving mode moved those
embedders onto a clock no display drives, so each `scheduleFrame` was dropped
by the per-display loop and the app painted once and froze.

The unscoped `RequestFrame` path additionally falls through to the global vsync
path whenever the display states cannot speak for every view — views parked in
`default_state_`, or displays registered while no view is homed. Suppression
stays exact: the fall-through keys on view ownership, not on whether a display
accepted the request, so a display whose views are all non-renderable keeps its
deliberate refusal instead of having a global frame resurrect it. Together the
opt-in and the fall-through make silent frame starvation unrepresentable rather
than merely unlikely. The regressions are
`DisplayRegistrationAloneDoesNotEnterPerDisplayMode`,
`RegisteredDisplaysWithoutHomedViewsStillScheduleFrames`, and
`UnhomedViewsKeepTheGlobalFrameClockInPerDisplayMode`.

Never make the GTK embedder call `FlutterEngineSetViewDisplay` to work around
this. That opts it into per-display frame opportunities
(`RequestFrameForDisplayInternal`) whose baton, target, and completion
obligations `fl_view`/`fl_engine` cannot satisfy.

### Patch 11: Vulkan completion and upstream buffer recycling

Avio replaces per-submit fences with one persistent timeline semaphore. A
queue-local binary semaphore orders the render batch before the timeline marker
batch, so CPU retirement cannot race render execution. The completion callback
is the sole successful retirement edge for tracked render objects, imported and
exported semaphores, and the upstream `GpuSubmissionTracker` submission ID.
Preserving that tracker is mandatory: current upstream uses its monotonic GPU
completion watermark to decide when HostBuffer storage can be reused. A failed
queue submission retires its never-executed ID immediately; failure to register
the timeline callback waits for the exact submitted value and otherwise leaves
the ID pending rather than asserting unsafe GPU completion.

The ContextVK-owned transients pool keeps exactly one MSAA + depth/stencil set
per key `(width, height, color format, MSAA)`. Patch 46a restores that rule:
an acquisition returns the key's one entry whatever its lease, so same-sized
views in one frame, and a view's next frame while the previous one is still on
the GPU, share it. Sharing is safe because every user records on the raster
thread and submits to ContextVK's one graphics queue, each root pass clears
both attachments, and the framebuffer cache lives on the per-target resolve
image. The invariant is written on `SwapchainTransientsVK`. Patch 46a also
widens the incoming render-pass dependency (`deps[0]`) of passes that have a
depth/stencil attachment to the early/late fragment-test stages and
depth/stencil read/write access, so a pass's depth/stencil clear is ordered
after earlier passes' writes to the shared attachment (the same latent hazard
exists upstream for its shared swapchain depth and recycled RenderTargetCache
depth). Passes without depth/stencil keep the color-only scope. The
squashed `0bdaa23b3b` per-lease exclusivity, which allocated a second full set
for every concurrent same-key acquisition, must not return. Regressions:
`ContextVKTest.TransientsPoolSharesOneEntryPerKeyWhileTracked`,
`TransientsPoolSharesOneEntryPerKeyWhileWrapperLeased`,
`TransientsPoolDistinctKeysStayDistinct`,
`TransientsPoolTrimKeepsSharedEntryWhileTracked`,
`RenderPassBuilder.IncomingDependencyCoversDepthStencil`, and the Vulkan
playground synchronization-validation pair (run with
`VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`,
for example on SwiftShader; they skip without it):
`RendererTest.TwoSameSizeRootTargetsShareTransientsWithoutHazard` (two root
targets sharing one pool entry, back to back in one submission, report no
hazard) and its negative control
`RendererTest.SharedDepthStencilNeedsTheWidenedIncomingDependency` (the same
passes recorded with the pre-46a color-only incoming dependency must report a
depth/stencil hazard; with the builder's pass, none).

Patch 46b gives each set a lifetime owner. The embedder acquires a root
target's set on behalf of its `FlutterBackingStoreConfig.view_id`; the pool
records each owner's current key and moves it when the view acquires another
extent. `Rasterizer::CollectView` releases the owner
(`Context::ReleaseTransientOwner`), and once per raster frame, after every
view, `Context::ReleaseOrphanedTransients` frees idle orphans: entries some
owner held that no existing owner holds any more. A busy orphan stays until a
later check finds it idle; nothing is freed inside `Acquire`'s hit path, and
at the caps an idle orphan is evicted before another view's warm set. Hidden
views keep ownership, so warmth is unchanged, and the all-hidden trim
(patch 36) stays. Without Avio's G2 an extent toggle (a new view id per
change) would turn every return into a fresh set, so 46b is kept on the
branch only together with G2. Regressions:
`TransientsPoolRemovedViewReleasesItsOnlyKey`,
`TransientsPoolResizedViewReleasesOldKey`,
`TransientsPoolSharedKeySurvivesOneOwnerRemoval`,
`TransientsPoolMakeBeforeBreakRemountKeepsEntry`,
`TransientsPoolOrphanFreedOnlyWhenIdle`, `TransientsPoolCapEvictsOrphansFirst`.

### Patch 22: scoped render authority

A framework frame carrying `activeFrameViewIds` may render only those view
IDs. Residual render work schedules a separate compositor-authorized frame,
preserving view-scoped dispatch whenever the dirty set resolves to one display.
Patch #31 supersedes this patch's original deferred-pointer compensation by
removing the shared widget-build condition that required it.

Never restore the superseded `frameRenderViews` widening path from
`7a007de9ffc`: synchronously rendering dirty siblings produces presents without
compositor grants and can overload the embedder pipeline. Never restore the
superseded deferred-pointer queue either: its finite overflow path rewrote real
down/up sequences because it compensated below the shared-build violation. The
focused regressions are `packages/flutter/test/widgets/view_test.dart` and
`packages/flutter/test/widgets/view_scoped_frame_scheduling_test.dart`;
`avio-verify-patches.sh` asserts both the replacement primitives and the
absence of the superseded path. The Avio-side normative contract is
`avio/docs/engine-contract.md` core invariant 10.

### Patch 24: exact cadence, work, and cancellation

Each engine instance owns at most one delivered vsync baton per display; the
old process-global 512-token registry and silent eviction are gone. Returning
a baton names a non-empty target set and opens an engine-local
`FrameOpportunityId`. Every target must claim that record exactly once through
the root-target callback or a typed non-render outcome. A produced-target claim
and cancellation are one mutex-serialized race, so late raster work from a
retired epoch cannot escape to the host.

Pending batons and already-returned future opportunities have distinct exact
cancellation entry points. Both acknowledge only after the UI-side Animator
state has settled. Pipeline rejection releases its reservation immediately;
typed `Backpressured` completion and fresh demand are separate edges. The
semantic feature request requires per-display vsync, root-target mode, explicit
render completion, and the terminal-outcome callback as one indivisible
contract.

The admitted target set travels with the opportunity through the timing
recorder into the Animator. At the UI boundary it is partitioned exactly once:
only requested, active, admitted targets may render; admitted active targets
without work terminalize as `NoVisualChange`; admitted targets removed before
the callback terminalize as `TargetRemoved`; requested targets absent from the
grant cannot render. This reconciliation is what closes the conservation
ledger for clean and concurrently removed views without a watchdog.

### Patch 27: external Vulkan image ownership

External render targets carry their foreign queue-family ownership into the
Impeller render pass. The acquire and release barriers cover the complete
`TextureDescriptor`, including its array-layer count; passing only the texture
type is both incompatible with current upstream and would lose the descriptor's
explicit `array_layer_count` for `kTexture2DArray`.

### Patch 29: O(1) element view ownership

Every mounted `Element` carries a typed `BuildViewIdentity`. Real `View`
boundaries seal one `SingleBuildView` token for themselves and descendants;
roots and cross-view widget ownership carry `AllBuildViews`. Mount inherits the
parent token and GlobalKey reparenting updates the affected subtree until the
next View boundary, so `BuildOwner.scheduleBuildFor` never walks ancestors or
guesses a render root on the hot dirty-mark path. A stale view token widens to
the explicit all-views scheduler path. Focused tests pin initial attachment,
cross-view reparenting, and unowned-root behavior.

### Patch 30: exact texture invalidation

Native texture-frame notifications reach Dart with the texture ID that changed.
`RendererBinding` owns an ID-keyed callback map, so lookup is O(1) in the
number of registered texture IDs while a shared texture explicitly fans out to
all of its consumers. `TextureBox` registration follows attach, detach, and ID
retargeting; frozen consumers remain clean.

The stock platform texture API still schedules its compatibility frame after
the exact invalidation. Avio's DMA-BUF publication path deliberately does not:
it only marks the raster texture and notifies Dart, allowing the dirty owner to
request work while the compositor-issued frame opportunity remains the sole
cadence authority. Never add `Engine::ScheduleFrame` to
`EmbedderEngine::PublishDmabufTexture`; that would widen one producer update
back into a global frame.

### Patch 31: View-owned build scopes

Each real `View` owns and registers one native framework `BuildScope` with its
`BuildOwner`. `WidgetsBinding` resolves the engine-admitted view IDs through
that O(1) registry. Every frame may settle the non-rendering root scope so it
can create or retire structural `View` boundaries; mounting a boundary leaves
its visual descendants dirty in the new view-owned scope. A global frame then
builds every registered view in stable ID order, while a scoped frame builds
only its engine-admitted views. This makes the first exact frame a valid
bootstrap without letting it build or render an unadmitted sibling. Dirty
entries settle as their scope returns, so a later scope dirtying an
already-built sibling remains demand for another opportunity and requests that
opportunity at the end of the current widget frame.
Retiring a view also retires its scheduler custody at the same lifecycle edge,
so a removed scope cannot leave a stale ID widening later frame requests.

This keeps an inactive view on its last coherent widget, render, and hit-test
tree. Pointer events therefore stay on Flutter's ordinary ordered dispatch
path; there is no Avio backlog, overflow policy, synthetic cancel, or wait for
KMS presentation. A parent-driven update to a nested `View` marks that view's
own scope dirty instead of synchronously crossing the scope boundary. The
frame-driving Flutter test binding calls the same protected build operation as
production so conformance cannot drift behind a cloned global-build step. A
binding-level regressions begin from an unbuilt root, admit one of two views,
and pin both that only the admitted child boots and that the unprocessed scope
requests the next frame before its demand can be forgotten.

### Patch 32: selected-target partial raster

Root-target embedders select the exact backing target before Flutter finalizes
damage. `FlutterBackingStoreContentState` names that target and content epoch,
and supplies either exact catch-up damage for preserved pixels or unknown
history for a mandatory full repaint. The engine keeps logical frame damage
separate from selected-buffer damage in `FlutterBackingStorePresentInfo`.

The root pass prefers multisampling. Impeller relies on it for arbitrary path
and clip-edge antialiasing; disabling it to obtain partial repaint loses visual
quality. For selected Vulkan images with an explicit external ownership and
GENERAL layout contract, the engine now bounds MSAA clear, raster and resolve
to the exact rounded damage rectangle. The resolved image preserves pixels
outside that region because its initial layout is known, not UNDEFINED.
Every scissor is intersected with the render area, and viewport/scene coordinates
stay unchanged. The factory advertises this ability before materialization and
preroll. Other Vulkan embedders and backends still use the full-target path.

Bounded passes bypass the existing framebuffer/render-pass cache because its
key omits resolve preservation policy. They neither consume nor overwrite a
full-pass entry. Unknown contents and root readback require a full repaint
before culling; a bounded pass may never silently widen afterward. There is no
scratch-copy path or second image-ownership mechanism.

If the transients budget cannot seat a multisample reservation, the frame
degrades to a single-sample pass rather than failing: a refused reservation
used to return `nullptr` and lose the whole frame, which live sessions hit
during window-chrome reconfiguration bursts. Only that fallback path renders
into the embedder's image directly, and only it takes the preserved `Load`
first pass. Successfully materialized single-sample targets emit the
`EmbedderRootSingleSampleTarget` TRACE counter, keyed by their Impeller context.
This describes attachment selection, not proof of rendering or presentation.
There is no process-global sample transition state or ERROR line when targets
alternate their sample counts.

Impeller's existing external-image queue-family barriers and exact
render-complete semaphore remain the only GPU ownership path. There is no
scratch image, copy pass, or second fence source. Unknown or malformed target
history fails safely to full, and an empty exact buffer update terminates as
`NoVisualChange` without raster.
An empty current display list is not itself a no-change proof: when its exact
buffer damage can remove a previously submitted root, the engine performs a
clear-only render, replaces retained coverage, and publishes that damage. An
initial empty root has no prior scene to remove and terminalizes as
`NoVisualChange` without publishing an unknown target. After a root has been
submitted, only exact empty selected-buffer damage may terminalize a selected
target as `NoVisualChange`.
Because `Load` reads the retained color attachment, both the foreign-queue
acquire barrier and the render pass's incoming dependency declare color
attachment read access alongside write access. Clear targets remain
write-only.

Deferred: reducing attachment dimensions. Bounded rendering avoids work, but
MSAA and depth attachments still have the full target dimensions. A smaller
scratch attachment would require explicit origin, clipping, readback, copy and
completion contracts. The attachment-size comparison with a single-sample,
color-only compositor is not a measured whole-desktop memory or speed ratio.

Logical frame damage remains sparse. The renderer's one rectangular canvas and
Impeller dispatch are lowered once to an explicit raster/replacement region;
that same region is the layer-tree clip, transparent clear, retained-coverage
replacement, and reported buffer damage. This prevents unchanged translucent
content between disjoint logical changes from blending over preserved pixels.
The old sparse-rect coalescer was removed because every consumer immediately
reduced its result to the same bounding rectangle, so it neither reduced raster
work nor described the pixels actually replaced.

Every target selected by the embedder is returned exactly once even when target
construction fails. Pixel tests rotate three real targets, exercise sparse
catch-up, transparent blur removal, complete scene removal, disjoint damage
around unchanged translucent content, initial empty-root suppression, and
compare both partial and heuristic-full fallbacks byte-for-byte against a
forced full repaint.

Only the root pass over the embedder's own target may keep the load action its
target declares. `InlinePassContext` clears the first pass over every other
target, because every other target comes from `RenderTargetCache`, is recycled
across frames, and declares `kDontCare` to mean "nobody chose", not "these
pixels are disposable". The opt-in is an explicit constructor argument on
`InlinePassContext` and `LazyRenderingConfig`, passed true at exactly the two
sites in `Canvas` that wrap the caller's render target, so a save layer cannot
inherit the preserved-root exemption and paint over another layer's leftovers.

A frame whose diff records a readback cannot be a partial repaint however small
its damage is: Impeller rasters such a frame into an offscreen and copies that
offscreen, whole, over the target, replacing every preserved pixel outside the
damage. `Damage::has_readback` carries that from the diff, and the frame falls
back to a full repaint next to the existing partial-repaint cost heuristic --
before the cull rect narrows Preroll, because a tree already culled to the
damage cannot be promoted back to a full repaint without erasing the rest of
the scene.

`EmbedderExternalView::Render` returns a typed outcome rather than a bool. When
every requested damage rectangle falls outside the target no pass runs, and
that is reported as `NoVisualChange` rather than as a present: the target still
holds its previous contents, and recording the frame as its history would
compute the next frame's damage against a frame that was never drawn.

Published paint coverage is the recorded draw-op region unioned with the
frame's compositor material rects. A material emits no draw ops -- the
compositor paints it -- so the recording's rtree omits it while the layer's
`Diff` puts its bounds in damage; without the union, coverage claims every
glass surface was never painted, which is exactly the region a later partial
frame would then decline to preserve.

### Patch 33: bounded Impeller pipeline-cache I/O

Impeller validates an already-open pipeline-cache file as a regular file and
checks its profile-owned byte ceiling before mapping it. The compatibility
header's declared payload must fit both that ceiling and the mapped file
remainder. Truncated, oversized, sparse, non-regular, or length-mismatched
cache entries therefore fall back to an empty Vulkan cache without exposing an
unbounded mapping or span.

Persistence applies the same payload ceiling before allocation and after the
driver fills the cache blob. The existing worker serialization and atomic
replacement remain the sole write path. Tests pin every malformed read shape
and an over-budget driver result; cache policy/configuration is supplied by the
separate resource-lifecycle extension rather than global process state.

### Patch 34: hard transient budgets and idle-only trim

The Vulkan context's cached MSAA and depth/stencil attachments obey exact
entry and byte limits. Admission reclaims only entries with no external
wrapper owner and no texture reference from submitted GPU work. If every
candidate is leased, the acquisition fails instead of dropping a live entry
from accounting and silently exceeding the configured limit. Footprint
arithmetic is checked before allocation, and explicit profiles bypass the
legacy process-environment override.

Since patch 46a the entry count is one per distinct key, so the limits bound
distinct live extents, not concurrent leases: a leased key is shared and never
refused. Only an acquisition of a new key can be refused at the caps.

Patch 46u keeps admission and both caps on nominal accounting; real bytes are
reported, not enforced.

Memory-pressure cleanup reaches Impeller in Slimpeller builds on the raster
thread. The backend-neutral `TrimIdleResourceCaches` seam reports exact
before/after usage, while Vulkan removes only the same provably idle entries;
active render targets remain untouched. Per-frame thread-local descriptor and
command-pool disposal is unchanged. Focused tests pin lease conservation,
entry and byte rejection, overflow rejection, idle-only trimming, and GPU
tracked-texture preservation.

### Patch 35: negotiated resource profiles and cache capabilities

Avio's Vulkan Impeller embedder profile supplies one complete resource policy
before any platform view or GPU resource exists. The versioned extension binds
hard transient entry/byte limits to an explicit pipeline-cache mode: disabled,
read-only, or read-write. A cache-capable profile carries an already-open
directory descriptor that the engine duplicates during initialization, so the
embedder selects the principal namespace and the engine never interprets a
cache path. Missing, unnegotiated, truncated, non-directory, or internally
inconsistent profiles fail initialization rather than reverting to process
environment policy.

Pipeline-cache persistence now obeys the negotiated access mode and byte cap.
The compatibility header separately versions its serialized schema and
Impeller pipeline ABI in addition to the Vulkan device, driver, pointer ABI,
and driver UUID. Incompatible or corrupt data still falls back to an empty
Vulkan cache. Stock embedders that do not negotiate the feature retain their
existing ContextVK defaults.

### Patch 36: exact per-view render visibility

Avio supplies each hosted view's render relevance explicitly as visible,
obscured, or suspended. This is neither Dart application lifecycle nor input
focus: every view remains registered, while only visible views may create new
raster demand. A visibility change never rewrites an opportunity that the
embedder already admitted. At the UI boundary that target instead terminates
exactly once as no-visual-change; subsequent demand stays suppressed until an
explicit visible update schedules one fresh view-scoped frame.

The feature can be negotiated independently for legacy global-vsync clients.
Global clients register all views without opting into per-display scheduling;
only an explicitly all-hidden view set suspends the shared clock. An already
requested global baton is consumed on return and returns its semaphore without
raster work. A visible sibling keeps the clock eligible, with hidden views
excluded from both new and cached-tree raster submissions. Restore schedules a
fresh frame. Exact frame-opportunity clients retain their existing prerequisites
and terminal outcomes; visibility does not weaken those contracts.

The Linux GTK embedder negotiates this feature and maps each FlView's map/unmap,
window withdrawn/iconified, and toplevel visibility events to render relevance.
GTK Wayland reports xdg_toplevel suspended as FULLY_OBSCURED, including minimized
windows, without necessarily reporting ICONIFIED. Partial occlusion and loss of
focus stay renderable. This bridge does not change Dart application lifecycle.

A realized but unmapped cold renderer retains permission to produce its first
actual drawable frame, because the stock GTK runner shows its window from
FlView's first-frame signal. The existing renderer-owned have_first_frame
receipt ends this bootstrap permission before first-frame handlers run; if the
window stays hidden it is then suspended. Monitor replacement/re-realization
queries that retained receipt, so it does not grant a second bootstrap. Explicit
compositor occlusion or iconification still suppresses cold rendering, and
monitor disposal always suspends. No timer or guessed ever-visible state owns
this boundary.

When the last renderable view becomes hidden, the raster thread trims only already-idle
Impeller resources. Dart timers and application policy remain controlled by
Avio's separate typed shell lifecycle channel, so the engine never infers
authority, lock state, or suspension from missing vsync.

### Patch 37: exact retained compositor material

`SceneBuilder.pushAvioCompositorMaterial` adds a bounded, non-painting retained
node whose transformed rectangle, rectangular clip, opacity, recipe, and stable
identity are collected from the complete scene. The immutable set rides
`SurfaceFrame::SubmitInfo` into `FlutterPresentViewInfo` or
`FlutterPresentRenderTargetInfo`, so an embedder cannot pair generation N's
pixels with a later material registry. Metadata changes participate in layer
diff damage; partial-raster culling cannot erase unchanged material nodes.

The feature is negotiated as
`kFlutterAvioExtensionFeatureAtomicCompositorMaterials`, capped by
`FLUTTER_AVIO_MAX_COMPOSITOR_MATERIALS`, and rejects overflow or scene shapes
the external rectangle vocabulary cannot express. Root-target rejection is a
typed pre-raster terminal that does not quarantine a healthy GPU target. Linux
GTK exposes the same exact-frame sidecar immediately before drawing the paired
frame. This patch does not precompile shaders or allocate material render
targets: non-material trees keep the stock preroll path and all GPU resources
remain demand-driven in Avio.

### Patch 38: refused-target demand retention (superseded)

This patch proved that scarcity must not consume demand, but its null target
collapsed scarcity, withdrawal, removal, and stale authority. Patch 39 removes
that ambiguity and the blanket rearm mechanism.

### Patch 39: exact typed render-target acquisition

Root-target mode uses `FlutterRenderTargetAcquisitionCallback`, never the stock
boolean backing-store create callback. Every request carries its exact
`FlutterFrameOpportunityId`, display, and target configuration and returns one
of `Granted`, `Backpressured`, `Withdrawn`, `Removed`, `EpochStale`, or
`HostRejected`.
Unavailable acquisition never reaches the presentation callback: the
opportunity ledger terminalizes the target directly with the same typed cause.

Only `Backpressured` preserves the retained pixel baseline and asks the
Animator for later demand. Withdrawal, removal, stale authority, and invalid
host results are terminal without rearm. A freshly built tree that acquired no
lease never becomes the damage baseline. The extension ABI is version 2 and
negotiates `kFlutterAvioExtensionFeatureTypedRenderTargetAcquisition`; root
mode rejects the old untyped callback.

### Patch 40: atomic initial view/display ownership

When per-display vsync is active, `FlutterEngineAddView` registers the new
view's `FlutterWindowMetricsEvent.display_id` in the Animator before publishing
the view to Dart. This ordering matters because Dart's synchronous
`onMetricsChanged` callback may call `scheduleFrame()` before `_addView`
returns. That request must already resolve to the view's exact display instead
of collapsing into an outstanding targetless startup request.

Initial registration itself never schedules. Dart publication remains the
RuntimeController's single operation, and its stock post-success
`ScheduleFrame` requests the first frame after publication. Failed publication
rolls back only the newly registered mapping; a duplicate add cannot move or
remove the existing view. `FlutterEngineSetViewDisplay` remains the later
reassignment API and continues to schedule the destination display.

Generic embedders that have not opted into per-display vsync retain the stock
global `ScheduleFrame` behavior. The regression is
`EmbedderTest.AddViewPublishesInitialDisplayBeforeMetricsScheduleFrame`; it
schedules synchronously from `onMetricsChanged` and requires display 19, not
the default display or a silently coalesced request.

### Patch 41: exact scoped frame-request acceptance

`SchedulerBinding._hasScheduledFrame` may represent only a request the engine
actually retained. The scoped scheduling API therefore returns one synchronous
acceptance bit from `PlatformConfiguration` through `RuntimeController`,
`Engine`, and `Animator` to the framework. Unknown displays and requests whose
exact target set contains no homed renderable view return false, even when an
unrelated request is already pending on that display. An already-retained
request for the caller's own target returns true.

The framework latches `_hasScheduledFrame` only on true. Refusal leaves the
dirty view eligible for a later independent scheduling edge; making an
obscured view visible still creates the one current scoped request at the
engine owner. No timer, synthetic begin-frame, global fallback, or empty frame
is invented to repair a rejected request. Generic scheduling remains accepted
because the global engine path always retains its request.

`TestPlatformDispatcher` forwards the same acceptance result. Letting its
forward-compatibility `noSuchMethod` absorb this typed method returned `null`
as `bool`, so ordinary multi-view widget tests failed while scheduling their
first frame instead of exercising the contract.

`ShellTest.ScopedFrameRequestReportsWhetherItWasRetained` pins the engine
contract. The framework regression `a rejected engine request does not latch
the framework scheduler` proves a subsequent dirty edge can ask again, and
`TestPlatformDispatcher forwards scoped frame request acceptance` pins the
test binding to the same result-bearing API.

### Patch 42: real multisampling for GTK backing stores

`MakeRenderTargetFromBackingStoreImpeller` declares the GTK backing store as a
4x multisample target whenever the driver exposes
`GL_EXT_multisampled_render_to_texture`, but `fl_framebuffer_new` built a plain
single-sample framebuffer. Impeller binds a wrapped FBO exactly as handed to it
(`render_pass_gles.cc` `is_wrapped_fbo`) and skips its own resolve for wrapped
targets, so the declaration bought nothing: every Flutter app on Linux
rasterized at one sample and every path, clip and circle edge came out
aliased. The declaration is now truthful.

Two paths, chosen by capability. With
`GL_EXT_multisampled_render_to_texture` the colour texture is attached through
`glFramebufferTexture2DMultisampleEXT` and the depth-stencil renderbuffer
allocated with `glRenderbufferStorageMultisampleEXT`; the driver resolves into
the texture on read, so no consumer changes. Without it, colour and
depth-stencil become multisample renderbuffers and the texture moves to a
second framebuffer that `fl_framebuffer_resolve()` blits into; the compositor
calls that before either of its two consumers (`glBlitFramebuffer` for the
first layer, the texture for the rest). In the fallback Impeller declares the
wrapped FBO single-sample, which is inert: the FBO is bound as-is, no
attachment is created or sized from the descriptor, and the declared sample
count reaches only `GetTextureTypeFromDescriptor`, whose result the wrapped
path never consults. GLES pipelines carry no sample count.

The explicit resolve constrains the colour format, and that constraint has one
owner. Resolving a multisample framebuffer is only defined between identical
read and draw formats (OpenGL ES 3.0 §4.3.2) — a mismatch is `INVALID_OPERATION`
and the blit is silently dropped — and `GL_RGBA8` is the only colour format
portable across the multisample renderbuffer storage entry points. So an
explicit resolve forces the texture to RGBA8 as well, overriding the caller's
preferred format. `fl_framebuffer_get_sized_format()` is the single answer for
what the framebuffer actually holds; `create_opengl_backing_store` reads it
back for `FlutterOpenGLFramebuffer.target` instead of deriving the format a
second time, so caller and framebuffer cannot disagree about byte order. Byte
order costs nothing here: every consumer is a GPU one. The compositor's
presentation framebuffer, which `glReadPixels` does read, is single-sample and
keeps its own format.

Sample counts are negotiated, never assumed: `GL_MAX_SAMPLES` clamps the
request, a driver with no multisample capability keeps the original
single-sample framebuffer, a framebuffer the driver refuses to complete is torn
down and rebuilt single-sample, and the resolve itself is performed once at
construction so a driver that rejects it degrades then rather than dropping
frames silently forever.

`FlFramebufferTest` covers each outcome:
`ImplicitMultisampleNeedsNoResolve`, `ImplicitMultisampleKeepsTheRequestedFormat`,
`ExplicitMultisampleResolvesWithABlit`, `ExplicitMultisampleResolveFormatsMatch`,
`SamplesClampedToDriverMaximum`, `IncompleteMultisampleFallsBackToSingleSample`,
and `RejectedResolveFallsBackToSingleSample`.

### Patch 44: typed analytic material clips

Retained compositor materials carry a closed clip kind and four validated
parameters through Dart, the layer tree, the embedder ABI, and Linux frame
publication. `roundedRectangle` preserves the original material vocabulary;
`bottomEdgePull` describes the Dock's deforming analytic boundary without
flattening it into a rounded envelope or an untyped payload. Unsupported or
malformed clip descriptors reject the exact frame at the engine boundary.

### Patch 45: external linear-backdrop foreground coverage

Stock Linux Impeller's glyph correction and UberSDF luma correction both
assume Flutter owns the destination against which the coverage is painted.
Avio's transparent shell target deliberately violates that assumption for
direct-wallpaper foreground: edge or mask coverage survives into the root
target, and Smithay supplies the wallpaper during a later linear-light blend.
The same mismatch affects paragraph glyphs, analytic chrome, and transparent
image marks, so a glyph-only exponent cannot be the owning contract.

`Paint.avioCoverageMode` makes the missing destination contract explicit.
`platformDefault` preserves upstream behavior. `externalLinearBackdrop` is
carried as display-list paint state into the existing glyph-atlas, UberSDF,
direct-texture, and tiled-texture shaders. Each shader first combines authored
opacity with edge or mask coverage, then converts that final alpha exactly as
`1 - srgbToLinear(1 - alpha)` while preserving unpremultiplied source color.
This restores the display-space dark-on-light coverage response after
Smithay's linear-light blend. It changes no color, geometry, shaping, font
selection, or atlas sample and adds no draw, surface, texture, or compositor
pass. Stock glyph and UberSDF correction remains authoritative in
`platformDefault`; the external mode does not apply either approximation a
second time.

The mode belongs only on semantic foreground roles known to remain translucent
until external composition. It must not become a platform-wide default or a
Smithay heuristic over generic alpha: both would also alter text already
flattened into opaque Flutter pixels and translucent materials whose linear
coverage is intentional.

### Patch 46u: report-only render-resource accounting

The engine-private render-resource caches report exactly what they hold, and
reporting frees, ages and leases nothing. `TransientsPoolVK::ReportUsage` and
`RenderTargetCache::ReportUsage` (through the
`RenderTargetAllocator::ReportUsage` virtual) return an
`impeller::RenderResourceUsage`: entries, nominal bytes (the cache's own texel
accounting), real bytes, leased entries (a wrapper, an open frame, or
submitted GPU work references them), distinct keys and duplicate entries (the
transient pool must report 0 since patch 46a; the offscreen cache reports
entries minus distinct keys), and per-interval counters: peak leased nominal
bytes, created entries and the real bytes they allocated (an entry created
and freed within one interval still counts), and orphans released by patch
46b (transient pool only). `ReportUsage(true)` starts a new interval.

Real bytes come from the backend: `Texture::GetAllocatedByteSize` (0 where
unknown) is `vmaCreateImage`'s allocation size for `AllocatedTextureSourceVK`
images, and `SwapchainTransientsVK::GetAllocatedByteSize` sums the
attachments a set has materialized. Every transient-pool erase (cap eviction,
orphan release, idle trim) goes through one accounting edge.

This is an internal C++ API: no embedder ABI, no new policy, and
`FLUTTER_AVIO_EXTENSION_VERSION` stays 6. It is the accounting half of the
dropped idle-release patch (kept for reference on `avio/vram-dropped-r`);
nothing releases on the host's request, nothing is stamped with a last use,
and the resource manager gains no flush. A later ABI extension exposes the
report. Regressions: `ContextVKTest.TransientsPoolAccountsNominalAndRealBytes`,
`TransientsPoolPeakLeasedResetsOnReport`,
`TransientsPoolReportsDistinctKeysDuplicatesAndOrphans`,
`TransientsPoolReportsCreatedEntriesAndBytes`, and
`RenderTargetCacheResourceTest.ReportsExactBytes` (GPU-free: a fake allocator
with page-rounded allocations).

### Patch 47: complete RenderTargetCache keys and miss attribution

`RenderTargetConfig` is the complete identity of an offscreen's textures:
extent, mip count, MSAA, depth/stencil, and now the color format, the color
and resolve storage modes, and the depth/stencil format and storage. Before
this patch a request for a different pixel format or storage mode could
reuse a cached texture of another format; no current Avio caller differs in
those fields, so hit rates are unchanged, but the latent bug is closed.

Every miss emits a `RenderTargetCacheMiss` timeline instant with the request
(`w`, `h`, `msaa`, `format`, `ds`, `label`) and a reason: `no_entry`,
`extent_mismatch` (an unleased entry differs only in extent), `all_leased`
(every entry of the key is leased), `aged_out` (one of the last 32 keys that
aging dropped, unused for the keep-alive number of frames) or `disabled`.
When its size changes the cache traces a `RenderTargetCache` counter with
patch 46u's accounting: `entries`, `bytes` (nominal) and `real_bytes`. All of
it is TRACE/timeline only; nothing is logged. Regressions:
`RenderTargetCacheResourceTest.KeyDistinguishesPixelFormat`,
`KeyDistinguishesStorageModes`, `MissReasonClassification`.

### Patch 48: single-sample Flip secondary

Vulkan reports `SupportsReadFromResolve() == false`, so a backdrop inside
another layer flips the MSAA pass target's resolve texture with a lazily
allocated secondary. Upstream obtained that secondary from
`CreateOffscreenMSAA` and kept only its resolve texture, allocating a 4x
color texture and a 4x depth/stencil texture to be dropped (about 198 MiB
requested to keep about 20 MiB at 2880x1800). On the explicit-resolve path
the secondary is now one single-sample `CreateOffscreen` in the resolve
texture's format and storage, labelled "EntityPassTarget Secondary", with no
depth/stencil. The implicit-resolve (GLES) path is unchanged. The swap and
return contract is unchanged. Regressions:
`EntityPassTargetFlipTest.FlipAllocatesOneSingleSampleTexture`,
`SecondFlipAllocatesNothing`, `FlipSwapAndReturnUnchanged`,
`FlipKeepsNonDefaultResolveFormat`, and the pixel test
`AiksTest.BackdropInsideOpacityLayerMatchesDirectDraw`: the snap-overlay
shape (a backdrop filter inside an opacity layer, drawing after the
backdrop) must equal the same picture drawn without a backdrop within
1/255, and on Vulkan it must have taken the labelled secondary.

### Patch 51: one RenderTargetCache aging epoch per raster frame

The cache's header promised that textures live "for at least one frame" and
die after keep-alive "frames", but `Canvas::SetupRenderPass` and
`Canvas::EndReplay` called `Start`/`End` on every Canvas: every view's
replay and every toImage snapshot reset every lease and aged every entry. In
Avio's multi-view engine an offscreen died after a few foreign Canvases
(flutter/flutter#190613 describes the same misnomer upstream). A nested
Canvas could also be handed a texture its outer Canvas was still rendering
into.

Leases and aging are now separate. A Canvas opens a lease scope in
`SetupRenderPass` and closes it in `EndReplay` (or its destructor);
`EndScope` releases only that scope's leases, a nested scope never reuses an
outer scope's lease, and `DisableCache` counts belong to the scope that set
them. The Rasterizer ages the cache once per raster frame, after every view
(`End`, from `Rasterizer::EndRasterFrameResources`). Snapshot Canvases
(`DisplayListToTexture`, `RenderToTarget`) never age it. The Impeller
interop toolkit's `Surface::DrawDisplayList` is its own frame and ends one
epoch per draw. Patch 46u's report reads leases from the scopes. No new knob
is added. Retention rises by at most the entries unused for keep-alive raster
frames. Idle behaviour: with no raster frames nothing ages, so the last
frame's entries stay until later raster frames age them out. Nothing else
frees them: the engine has no idle release on request, and patch 36's
all-hidden trim frees idle transient attachment sets only, never this
cache. Regressions:
`CanvasFailureTest.CanvasReplayLeasesButNeverAgesRenderTargetCache`,
`DestroyedCanvasReleasesItsLeaseScope`,
`RenderTargetCacheResourceTest.ForeignCanvasesWithinOneFrameDoNotAge`,
`EntryUnusedForKeepAliveFramesIsDropped`, `ToImageCanvasDoesNotAge`,
`NestedScopeKeepsOuterLeases`, `DisableCacheIsPerScope`,
`ReportReadsLeasesFromScopes`, and the unchanged playground
`CachesUsedTexturesAcrossFrames*`.

### Patch 52: color sources UberSDF cannot shade

With `use_sdfs`, an antialiased srcOver rect, rrect, oval or circle with a
color source takes UberSDF, which cannot shade the color source itself, so
`AddRenderSDFEntityToCurrentPass` blends the SDF mask with the color source
through `ColorFilterContents::MakeBlend(kSrcIn, ...)`: two snapshots and a
"Pipeline Blend Filter" target per draw. This patch removes what carries no
information, without depending on patch 50.

(a) A filled rect whose device bounds, under an axis-aligned transform,
contain the pixel-aligned bounds of the current clip coverage has an SDF
mask of exactly 1 at every visible pixel center (each lies at least half a
pixel inside the rect, where the SDF's half-pixel fade has ended). It draws
its color source directly with rect geometry under the clip, as `drawPaint`
does: no mask, no snapshot, no blend target, and no shape edge inside the
clip, so antialiasing is unchanged. The containment test also requires the
coverage as seen from the current pass's origin, so it never depends on
which coordinate space a nested pass recorded. Lines (shape transforms),
rounded or stroked shapes, and paints with a per-draw image filter (which
moves or spreads the rect's edges into the clip after the test) keep the
mask. This covers the greeter's full-screen halftone shader rect (about
416 MiB of snapshots per repaint before).

(b) Every other color-source SDF draw snapshots both blend inputs
single-sample without depth/stencil (`FilterInput::Make(..., msaa_enabled,
depth_stencil_enabled)`, `Contents::SnapshotOptions::depth_stencil_enabled`):
12 instead of 84 bytes per pixel with the blend target. Each input is one
draw over the SDF quad and the analytic SDF masks the quad's edges, so
multisampling and depth/stencil carried no information.

Regressions: `CanvasTest.SDFFillRectContainingClipNeedsNoMask`,
`CanvasFailureTest.SingleSampleSnapshotRequestsNoDepthStencil`, and on every
playground that renders with SDFs (`Playground::EnsureContextUsesSDFs`: the
OpenGL ES and Metal SDF backends, and Vulkan with SDFs enabled, the only
multisampled one on Linux) `AiksTest.ShaderRectContainingClipAllocatesNoOffscreen`,
`ShaderRRectInsideClipStillMasks`, `ShaderRectContainingClipMatchesDrawPaint`,
`ShaderRectWithImageFilterKeepsMask`, and the pixel goldens against the
pre-patch composite: `ShaderRectContainingClipMatchesMaskedComposite` ((a)
against the masked path, forced by a clip the rect does not contain) and
`SingleSampleMaskInputsMatchMultisampledInputs` ((b): the same kSrcIn
composite with multisampled, depth/stencil inputs as before, within 1/255).

### Patch 50: gradients inside UberSDF (backport)

Upstream moved linear and radial gradients into UberSDF (`fdb1d09ef8`,
flutter#192124; `419f9dd65e`, flutter#192962, the storage-buffer variant).
The fork base predates them and their prerequisites, and Avio's UberSDF
carries patch 45's coverage fields, so this is an adapted backport in one
commit rather than literal cherry-picks: `uber_sdf.frag` is split into
`uber_sdf_common.glsl` plus a ramp-texture variant (`uber_sdf.frag`) and a
storage-buffer variant (`uber_sdf_ssbo.frag`, used wherever the backend
supports SSBOs, i.e. Avio's Vulkan Shell and greeter, for every UberSDF
draw). `Canvas::AddRenderSDFEntityToCurrentPass` shades a linear or radial
gradient whose local matrix (with the inverse shape transform) is a
similarity directly in UberSDF: no snapshots, no blend target, no deferred
coverage pass. Conical and sweep gradients, images, runtime effects and
non-similarity gradient matrices keep the masked kSrcIn path that patch 52
slims.

Avio deltas against upstream, each to re-check at the rebase that drops this
patch:
- Edges keep Avio's look. A gradient's coverage uses the light-foreground
  gamma correction of the white mask it was blended through before, not the
  gradient color's luma; the storage-buffer variant dithers like the
  storage-buffer gradient shaders, and like the masked composite it dithers
  the gradient color before coverage scales it, so a pixel the shape does not
  cover stays exactly transparent (upstream does not dither in UberSDF).
  `AiksTest.SdfGradientEdgesMatchMaskedComposite` compares every pixel (rect,
  rounded corners, oval, circle; opaque and glass; both coverage modes) with
  the masked composite: alpha within 2/255, color within 2/255 on the
  ramp-texture variant and 5/255 on the dithered storage-buffer variant (two
  dither grids), and no pixel the composite leaves transparent may gain any
  value. It runs on Vulkan with SDFs enabled, the storage-buffer variant
  Avio ships, and on the OpenGL ES SDF backend, the ramp-texture variant.
  `SdfGradientLeavesUncoveredQuadPixelsTransparent` checks the quad corners
  around a circle are exactly zero in both coverage modes.
- Linear gradients need a similarity matrix too (upstream checks only
  affinity): mapping the end points through a shear or non-uniform scale does
  not map the gradient's field.
- Patch 45's `external_linear_backdrop` / `defer_coverage_transform` stay;
  a gradient shaded in UberSDF takes the coverage transfer in the shader.
- `#191925` and `#191980` (texture-gradient fixes and wide-gamut ramp
  textures) are not brought in: the ramp-texture variant uses the fork's
  current `GradientData`, and Avio's SDF users take the storage-buffer
  variant.

Revert this one commit to restore the masked path exactly; nothing else on
the branch depends on it. Regressions:
`UberSDFContentsTest.ApplyColorFilterWithGradient`,
`AsBackgroundColorGradientReturnsNullopt`, and on every playground that
renders with SDFs (`Playground::EnsureContextUsesSDFs`, patch 52)
`AiksTest.SdfLinearGradientRectAllocatesNoOffscreen`,
`SdfRadialGradientCircleUnderNonUniformScaleAllocatesNoOffscreen`,
`UnsupportedColorSourceStillBlends`, `SdfGradientEdgesMatchMaskedComposite`,
`SdfGradientLeavesUncoveredQuadPixelsTransparent`.

### Patch 55a: asynchronous prewarm of the variants Avio's SDF and single-sample paths draw with

`ContentContext` compiles each pipeline's default asynchronously at
construction, but almost no draw uses a default: `OptionsFromPass` selects
depth compare `kGreaterEqual`, while every default is built with
`ContentContextOptions`' `kAlways` (and mostly `kTriangle`). Every draw is
therefore a variant, and `CreateIfNeeded` compiles a missing variant
synchronously on the calling thread (`CreateVariant(/*async=*/false)` after
waiting for the default). With the persistent pipeline cache (patch 35) warm
this is a cache hit; after an engine update that changes a shader it is a
full compile on the raster thread. Patch 50 changed every UberSDF shader and
moved all SDF draws to the storage-buffer variant, and patch 52 moved its
snapshots to single-sample targets without depth/stencil, so after Build 2
those variants compiled on the raster thread at their first use, for the
masked path during an interaction (a snap preview, a dock reveal).

On a legacy-policy context that renders with SDFs, the
constructor queues the variants these paths draw with, behind the
defaults, on the same asynchronous path the defaults take.
`Variants::Prewarm` derives each descriptor from the default descriptor with
the options applied, exactly what `CreateIfNeeded` would derive, and labels it
`"<label> V#<n> Prewarmed"`. The first use finds the handle and waits for, or
takes over, its job. This legacy loop does not join in the constructor;
first-use latency still depends on actual compilation and worker progress. `MakeAvioPrewarmVariants(MakeAvioPrewarmTargets(capabilities))`
is the set (27 variants on Avio's Vulkan engines):

- Passes Canvas draws into, multisampled with depth/stencil when the context
  supports offscreen MSAA, in the offscreen format (save layers) and in Avio's
  root format `kB8G8R8A8UNormInt` (DRM ARGB8888 dma-bufs; offscreens are
  `kR8G8B8A8UNormInt` on Vulkan): UberSDF srcOver (patch 50; the
  storage-buffer variant where the backend has SSBOs), the masked composite
  as a srcOver texture (patch 52 b), the backdrop restored after a Flip as a
  kSrc texture without depth write (patch 48's path), and patch 52 (a)'s
  direct color source draws: tiled texture, fast gradient (`kTriangle`) and
  the storage-buffer linear and radial gradients, srcOver and opaque kSrc
  with depth write.
- Patch 52 (b)'s single-sample offscreens without depth/stencil: the UberSDF
  mask snapshot, the "Pipeline Blend Filter" texture draws (kSrc, then
  kSrcIn), and the linear and radial gradient snapshots for a gradient
  UberSDF cannot shade.

Patch 48 adds no variant of its own: `Flip` swaps only the resolve texture,
so the pass keeps its multisampled attachment, sample count and format
(`RenderTarget::GetRenderTargetPixelFormat` reads the resolve texture, which
patch 48 allocates in the same format). Legacy contexts without SDFs keep the
stock startup. Coverage, including negotiated GTK/GLES, uses the separate
caller catalogue and controlled queue-all/validate barrier described in patch
61; it does not enqueue these legacy pass keys. Not prewarmed by the legacy
27-key catalogue: runtime effect
pipelines (keyed by shader, created through
`GetCachedRuntimeEffectPipeline`), conical and sweep gradients (no Avio Dart
source uses them), uniform and ramp-texture gradients (Avio's SDF users have
SSBOs), the single-sample root fallback of patch 32, and every other stock
variant. The root format is Avio's contract, not something the engine learns
before the first frame; if Avio's root fourcc changes, change
`kAvioRootColorFormat`. Regressions (`impeller_unittests`):
`ContentContextPrewarmSetTest.*` (the set's passes, sample counts, blend
filter, Flip restore and color source variants, backends without SSBOs or
MSAA, a root in the offscreen format), GPU-free
`ContentContextPrewarmOptionsTest.SetIsWhatTheDrawSitesCompute` (over render
targets built as Canvas and `MakeSubpass` build them, the set equals the
options the draw sites compute from `OptionsFromPass`) and
`AvioTargetsComeFromTheCapabilities`, and on every playground
`ContentContextPrewarmTest.FirstUseFindsThePrewarmedVariant` (SDF backends:
each draw site's getter returns the prewarmed pipeline, built for exactly
its pass; a variant outside the set still compiles on first use) and
`ContextsWithoutSDFsDoNotPrewarm`.

## Known baseline debt

- ~~Generic embedder compositor and platform-view tests required broad
  exclusions~~ FIXED by patch #23. Stock `present_layers_callback` /
  `present_view_callback` semantics remain the default generic mode. Avio's
  single-root behavior requires an explicit negotiated `RootRenderTarget`
  mode, and every root submission returns a typed terminal result—including
  unsupported platform-view, backing-store, and raster failures—rather than
  abandoning a callback latch.
- `embedder_unittests`: the **GL present-info damage family**
  (`EmbedderTest.PresentInfo*` and related populate-existing-damage render
  tests) hangs after rendering one frame. The mechanism is exact and is not a
  flake: patch #3 made a zero-damage frame skip submission entirely
  (`Rasterizer::DrawToSurfaceUnsafe` returns `kNoVisualChange`), while these
  four upstream tests render identical content twice and then block forever on
  an untimed latch waiting for the second frame's *present* to arrive carrying
  empty damage. They hang identically in isolation; only
  `PresentInfoReceivesFullScreenDamageWhenPopulateExistingDamageIsNotProvided`
  passes, because without `populate_existing_damage` there is no buffer damage
  and the skip cannot engage. The 300 s per-test timeout then aborts the whole
  binary, which is why the runbook excludes the family rather than tolerating
  failures. Avio is Vulkan-only and never exercises the GL present path, so
  this is unused-path debt; the damage behavior Avio does rely on is gated by
  integration telemetry (`flutter_full_damage_fallback_total`). Fixing it means
  rewriting the four tests against the fork's contract (no present for a
  zero-damage frame), not changing the engine. Investigate only if a GL
  deployment ever becomes relevant.
- `flow_unittests`: the three performance-overlay golden variants
  (`PerformanceOverlayLayerDefault.Gold`,
  `PerformanceOverlayLayer90fps.Gold`, and
  `PerformanceOverlayLayer120fps.Gold`) abort locally when the test-font
  fixture is unavailable. The remaining 317 tests pass; do not broaden this
  fixture exception to other flow tests.
- ~~Frame-path high-water probes and the signal-driven raster watchdog~~
  REMOVED by patch #24. Exact opportunity identities and terminal outcomes are
  the causal evidence now; production starts no polling thread, owns no
  process-global signal handler, and emits no per-frame `IMPORTANT` stream.
- ~~`on_empty_frame_callback` never fires at runtime~~ FIXED (25c4418f63,
  2026-07-03): both silent abort paths in `Animator` (PipelineFull before
  BeginFrame, and a vsync whose requested views resolve to none) now notify
  `OnAnimatorEmptyFrameForDisplay` with the latched/requested view set, and
  the PipelineFull retry stays view-scoped. Patch #24 supersedes the old
  recovery-watchdog inference entirely: backpressure terminalizes the exact
  target and re-arms typed demand as a separate edge.
- Per-view frame-request completion is now a **contract guarantee** of
  patch #5 (extended by 321a62c83b + the reuse-frame notify, 2026-07-08,
  squash into #5 at the next rebase): every view named in a view-scoped
  frame request completes exactly once — produced through its root-target
  callback or terminalized by a typed exact-opportunity outcome
  immediately when (a) the request targets an unregistered display, (b) a
  view not homed on that display (`LatchDisplayFrameRequest` used to filter
  those silently; the 2026-07-08 live RCA measured ~550 half-second watchdog
  timeouts per 5 minutes), or (c) the vsync resolves to the cached-tree
  reuse path (`BeginFrameForDisplay` early return), which consumes the
  latched set without running a UI frame. Preserve this guarantee when
  rebasing `Animator::RequestFrameForDisplayInternal` /
  `BeginFrameForDisplay`.

- Avio-side follow-up (rendering-plan step 4): scene assembly latches
  window elements on a newly-hosting output without their chrome binding;
  `synthesize_window_chrome_binding` in `presentation/latch.rs` papers over
  it from the chrome view's latest accepted entry. Attach bindings properly
  at assembly time and the synthesis becomes a cold fallback.

## Rebase runbook (quarterly, at upstream stable cutoffs)

1. `git fetch upstream main` (remote `upstream` = flutter/flutter; refspec
   `+refs/heads/main:refs/remotes/upstream/main` is configured).
2. Tag the current state: `git tag pre-upgrade-<date>`.
3. Record the stable release SHA, audit its branch-only delta, pin the selected
   `upstream/main` SHA, and create or rename the local maintenance branch to
   `avio/<new-ref>`. Do not merge the stable release branch into main.
4. `git rebase --onto <target> <old-merge-base> avio/<new-ref>`.
   Conflict hot zones: `shell/platform/embedder/embedder_external_view*`
   (preserve upstream generic mode and port rendering fixes into the explicit
   Avio root-target mode), `impeller/renderer/backend/vulkan/**` (interface churn — adapt
   our DmabufTextureSourceVK / timeline files to new virtuals),
   `mock_vulkan.cc` (merge feature advertising into upstream's walker).
5. `git range-diff <old-base>..<old-branch> <target>..<new-branch>` — every
   patch must be accounted for.
6. `bash avio-verify-patches.sh` (in this directory) — all assertions must
   pass. This is the gate that guarantees fixes (e.g. the RSS
   DisposeThreadLocalCachedResources cherry-pick) survive.
7. `gclient sync -D` then **regenerate GN args** for every out config:
   `./flutter/tools/gn --unoptimized` / `--runtime-mode=profile` /
   `--runtime-mode=release` (stale `dart_version` stamps cause gen_snapshot
   `ApiError` failures). Existing output directories can also retain an old
   copied Dart SDK and `flutter_tester` because checkout timestamps do not
   invalidate those outputs. Explicitly build `flutter/build/dart:dart_sdk`
   and `flutter_tester`, then verify both report the Dart version in
   `flutter/third_party/dart/tools/VERSION` before running framework tests.
8. Build per config: `ninja -C engine/src/out/<config> libflutter_engine.so
   libflutter_linux_gtk.so gen_snapshot flutter_patched_sdk
   flutter/build/dart:dart_sdk flutter_tester embedder_unittests
   shell_unittests impeller_unittests flow_unittests`.
9. Run the test suites with explicit, narrow platform boundaries:
   - `shell_unittests` and `embedder_proctable_unittests` run unfiltered.
   - `embedder_unittests` excludes only
     `EmbedderTest.PresentInfo*:EmbedderTest.PopulateExistingDamage*`; generic
     compositor and platform-view tests must run.
   - On Linux, run `impeller_unittests` with the generated SwiftShader ICD,
     validation-layer path, and `VK_LAYER_KHRONOS_validation`, excluding only
     the interactive `Play`, `Compute`, and `FrameBufferObject` playground
     families. Upstream's interactive Impeller runner is macOS-only; the
     non-playground Linux result is the supported automation boundary.
   - Run `flow_unittests` in full and account for only the three named missing
     font-fixture goldens above.
   - Run
     `packages/flutter/test/widgets/view_scoped_frame_scheduling_test.dart`
     with both `--local-engine` and `--local-engine-host` naming the rebuilt
     host output.
10. Rebuild Avio (`cargo build --profile profile`) — bindgen recompiles
    against the fork header and fails loudly on ABI drift — then run the
    validation matrix (Avio `docs/engine-contract.md` §11).

Notes:
- This clone is shallow + blob:none. `bin/internal/content_aware_hash.sh`
  skips merge-base logic on shallow clones, so the flutter tool bootstrap
  would 404; Avio's build pins `FLUTTER_PREBUILT_ENGINE_VERSION` to the
  merge-base content hash automatically
  (`build_support/shell_builder/src/flutter_sdk.rs`). The same value for a
  manual framework test comes from
  `cargo run -p avio_shell_builder -- tool-engine-version <avio-root>`.
- Watch list: PR flutter/flutter#183382 (Impeller Vulkan desktop backend —
  engine-managed VkSurfaceKHR, architecturally opposite to our
  embedder-managed model), `material_ui`/`cupertino_ui` first real releases
  (Material decoupling — placeholders as of 2026-06), windowing API
  stabilization (`WidgetsBinding.windowingOwner` — future Avio shell
  integration point).

## Deferred target admission recovery (2026-09-07)

Selected-target materialization failure precedes GPU submission. Report the
negotiated `AllocationFailedBeforeSubmit` terminal with its exact backing store
instead of `RasterFailed`; otherwise a safety-correct host permanently
quarantines an unchanged slot and eventually retries an exhausted grant. Hosts
request `PreSubmitFailure`; selected-target initialization rejects hosts that
do not understand this proof. Real raster failures retain their prior meaning.

Focused tests force attachment-factory failure, assert one exact terminal and
collection, and exhaust both sample modes of the six-entry Avio profile before
releasing an outstanding lease. No GPU OOM or battery state is required.

## Root attachment pressure diagnostics (2026-09-07)

`EmbedderRootTargetAdmission` records dimensions, materialization outcome and
both multisample/fallback refusal witnesses at TRACE, keyed by Impeller context.
Admission backpressure never emits a per-frame ERROR. Admitted attachment
materialization failures emit at most one process-level diagnostic; repeated
evidence remains in the trace counter. Successful multisample targets add no
new diagnostic event. Sample-count reporting follows surface validation and
never claims that attachment creation proves a successful render.

## Post-acquisition rendering result and failure propagation (2026-09-07)

The accepted terminal result is distinct from successful target acquisition.
Deferred refusal keeps the last painted layer tree and requests another
opportunity without a second terminal callback. Rejected callbacks cannot
advance paint-region history. Required color and depth allocations form one
pre-submit transaction; a missing pooled depth attachment cannot trigger an
uncached retry in the generic attachment helper.

Canvas reports explicit render/encode/enqueue/flush failure through the
DisplayList dispatcher. Cleanup still drains earlier queued work, and a failed
inline pass is consumed exactly once so destruction cannot retry its commands.
Those failures retain the conservative raster-failed classification; only
factory failure before any GPU work carries pre-submit proof.

### Patch 54: Screen is a coefficient blend

Premultiplied Screen is `src + dst * (1 - src)`, so it needs no sampled
destination, framebuffer-fetch extension, or backdrop Flip. Screen now ends
`kLastCoefficientBlendMode` in `impeller/geometry/color.h`, with
`Entity::kLastPipelineBlendMode` as its alias. The fixed-function RGB factors are ONE and
ONE_MINUS_SRC_COLOR, and alpha factors are ONE and ONE_MINUS_SRC_ALPHA.
Canvas, Flow root readback classification, DisplayList readback classification,
AtlasContents, filters and vertex blends all follow that boundary. Overlay and later modes retain the advanced path.

The existing Screen shader coefficient row was unreachable through normal
coefficient routing and omitted the source term. It is corrected to
`{1, 0, 1, 0, -1}`. Shader specialization is now built from the single
`kPorterDuffCoefficients` table rather than its separate duplicate; Screen
also inverts to itself for texture/vertex blends. No embedder ABI or SDK
interface changes. Root and layer sample counts and all depth/stencil
attachments retain their existing policy.

Regressions: `ContentContextOptionsTest.ScreenBlendIsAPipelineBlend` checks
RGB/alpha descriptors at both 1x and 4x; `BlendCoefficientsTest` compares the
production coefficient shader equation against `Color::Blend` for transparent,
opaque and translucent pairs and checks vertex inversion;
`AiksTest.ScreenPipelineMatchesCpuOracleWithoutOffscreen` checks painted
rectangles, and `ScreenImageFiltersAndVerticesMatchCpuOracleWithoutOffscreen`
checks DrawImage/DrawImageRect with Screen color filters and textured colored
DrawVertices against the CPU oracle within 1/255. The cache recorder covers
both single-sample and MSAA requests; Screen must allocate no offscreen at all.
`ScreenVulkanComparisonTest.ScreenPreviousFetchAndPipelineClippedEdgesGolden`
compares actual 4x clipped gradient edges with the previous framebuffer-fetch
path, records edge/interior deltas, and emits paired light/dark images at
scales 1, 1.25 and 2. It deliberately detects a changed result, and does not
approve that result. Existing `BlendModeScreen` and `BlendModeSrcAlphaScreen`
playground/golden cases remain relevant. Full
engine builds and init/first-use pipeline measurements remain packaging gates.
The user waived the visual-comparison campaign for this memory iteration;
comparison fixtures remain available but their presence is not executed GPU
evidence. The standalone CPU oracle
checks the coefficient math only; it is not an Impeller build or GPU result.

#### Starting coverage-antialiasing audit (before patch 55b)

Patch 54 alone did not authorize lowering any sample count. At that point the
code still had coupled layer/coverage sample policy and depth/stencil consumers:

- `ContentContext::MakeSubpass` selected 4x using `SupportsOffscreenMSAA` and a
  boolean. `Contents::SnapshotOptions` defaulted that boolean to true;
  `ExternalCoverageContents` (patch 45's geometric foreground mask) used that
  default. Lowering the capability alone would have removed its multisample
  edge mask. Patch 55b adds explicit layer and coverage requests instead.
- `ClipContents::Render` wrote depth for difference clips and used stencil
  preparation plus depth-writing cover draws for other clips. The Canvas
  assigned clip depth to normal draws and replayed it after backdrop reads;
  `InlinePassContext::GetRenderPass` required both depth and stencil. Patch 55b
  retains that legacy route and adds sample4 mask replay and bounded native4
  islands over a color-only 1x parent for the explicit coverage policy.
- Path NonZero/EvenOdd fills and overdraw-preventing geometry consumed stencil
  modes in `ContentContextOptions`. Patch 55b moves those stencil consumers to
  its bounded native4 path atlas or draw islands instead of deleting their
  fill/stroke semantics with the root attachments.
- `EntityPassClipStack::RecordClip` retained the fractional-rect 0.124-pixel
  rounding rule, which assumes the native 4x sample grid. The explicit coverage
  path now records fractional edges with quad or path masks; legacy rendering
  retains its existing rounding behavior.
- Patch 53 removed the single-sample self-restore feedback hazard. On its own
  it did not provide bounded scratch or separate layer/coverage regions.
  Patch 55b adds those regions and frozen prefix snapshots for deferred
  backdrop readers; advanced blend operators retain their existing semantics.

The old Vulkan/ANV path rendered an 8-bit MSAA source snapshot, averaged all
four destination samples in its fetch shader, and wrote that result to each
covered sample. Fixed-function Screen blends per sample with an unquantized
shader source. The real clipped-edge result can differ by more than 1/255;
interior rounding can differ too. The algebraic identity does not establish
pixel parity. The required design note, implementation scope, comparison
fixtures, and the unexecuted native/GPU comparison protocol are in
[Screen coefficient design](docs/engine/impeller/docs/avio-screen-coefficient-design.md).
Screen pixel parity is not established. The user waived the visual-comparison
campaign for this iteration only; the existing fixtures do not count as
executed comparisons. Continuous analytic coverage remains unsupported and
would require a separate contract and validation before advertisement.

Known follow-up: upstream `63768f5568` (flutter#192988, offscreen
advanced-blend texture coordinates) is not backported here. Patch 55a prewarms
the common coverage pipelines and reports successful first-use compiles, but
application-specific runtime compilation and its device cost remain
unmeasured. Removal of unused normal-route Screen advanced pipelines remains
outstanding; the comparison fixture intentionally retains the old Screen fetch
pipeline. Patch 50 code is independent, and the patch
inventory keeps its original adjacent context so EN50 reverse-applies alone.

### Patch 53: backdrop prerequisites for single-sample rendering

This adapts upstream `95feccb1e78b20d4001a508218a04a1438f94530`
(flutter#193306) and `4ae563fe6016468a142842cfd268acffe8e0f1f7`
(flutter#193176). A single-sample pass resumes by loading its existing color
attachment. `Canvas::FlipBackdrop` must not eagerly restore that same texture
with a sampled draw: sampling the attached target is an undefined feedback
loop. Multisample passes still restore their resolved backdrop. The fork's
failure propagation and patch 48's single-sample secondary remain intact.
The shared-backdrop fast path also reserves the saved layer's real content
depth instead of zero before drawing its children.

Regressions use the upstream GLES mock feedback detector and the existing
Canvas fixture: `CanvasGLESTest.AdvancedBlendWithoutOffscreenMSAAHasNoFeedbackLoop`
uses Multiply, which remains an advanced blend, and
`AiksTest.BackdropGroupSharedSnapshotReservesContentDepth` draws through a
cached shared snapshot. The GLES fixture exercises the legacy depth/stencil
route; patch 53 alone does not enable color-only roots. On its own this
prerequisite leaves root/layer MSAA and the negotiated embedder ABI unchanged.
Full engine tests and Vulkan/GLES captures are required
before packaging that prerequisite on its own. For the combined patch 55b
iteration the user waived the visual-comparison campaign; native build and
correctness gates remain explicit below.

### Patch 55b: explicit coverage policy and resource census (EG-1 through EG-5)

**Design note and scope.** Coverage is an explicit negotiated Impeller policy
on Vulkan or supported GLES3, not an environment switch or an
allocation-pressure fallback. The appended config, introduced in ABI8 and
carried by the current ABI9 contract,
`FlutterAvioAntialiasingConfig` selects legacy `msaa4` or coverage with 1x color
layers and native 4x masks, requests continuous classes, and gives separate
bounded coverage/layer regions. The Avio starting profile is 8 MiB coverage and
4 MiB layers. Vulkan advertises the implemented opt-in continuous classes
listed in patch 60; GLES and Metal advertise zero. The host request remains
zero. Unsupported backend or logical-device requests fail instead of silently
changing edge behavior. The legacy policy keeps
4x/4x and no region budgets. Legacy transient budgets must be positive;
coverage requires both legacy transient caps to be zero.

The header, initialization validator, Settings, Vulkan surface and ContextVK
carry the same policy. Structural bounds, unknown policy values, contradictory
samples/budgets, missing negotiated dependencies and renderer mismatches reject
initialization before platform-view creation. Context setup additionally
requires standard native sample locations and the required sampled/color and
stencil 4x formats; unsupported devices fail before the allocator is created.
No alternate AA policy is selected after refusal.

Under coverage, imported root targets contain the host's 1x color image only.
They bypass the legacy root transient pool and root depth/stencil attachments,
load preserved contents or clear fresh contents, and store to that same imported
image. Its original texture source still carries external queue ownership,
completion-fence export and backing-store collection. Masks retain native
sample4 quantization and all four sample identities; the implementation does
not resolve a coverage mask to a scalar and multiply that scalar into every
draw. The coverage geometry/path atlas, clip mask cache, tiled draw replay and
bounded layer region are separate construction contracts. Painter segments
replay in order through fixed native4 color tiles; ordinary image edges retain
their original native geometry. Direct 1x non-AA/filter passes avoid native4
islands where geometric coverage is not requested. The layer region uses
independent color images instead of a same-image layer atlas, avoiding sampled
attachment feedback. Logical snapshot rectangles and resource custody must
survive every deferred reader and filter; physical texture dimensions remain
the backing allocation's dimensions. Frozen backdrop inputs and the patch 53
single-sample restore fix remain prerequisites. Region
exhaustion may report an explicit layer overflow allocation; it must never
silently re-enable full-root MSAA or omit geometric coverage.

This bounded-memory renderer differs from the research sketch's anticipated
per-draw scalar masks, fringe-only replay and image analytic fast paths. Those
performance fast paths are not implemented here. Tiled replay can repeat work
and destination traffic; no predicted traffic reduction, GPU speedup or memory
savings is claimed without runtime measurements.

**Reports.** ABI v8 appends `FlutterEngineRequestAvioRenderResourceReport` and
separate AA/report capability bits. An accepted platform-thread call posts a
raster task and receives one callback there; rejection receives no callback.
Shutdown drains accepted callbacks, including typed EngineUnavailable
cancellations, before returning. Callback arrays are borrowed for that callback
only and bounded to 64 resource entries and 32 coverage reasons. Transport
preserves unknown kind/reason IDs. No reporting call creates a renderer, leases
or ages a cache, or waits for GPU completion; interval counters reset only when
the request asks to start a new interval.

ContextVK reports its physical allocated-image ledger once and merges a
separate pipeline ledger. Vulkan ContentContext does not add pool/cache/region
image bytes a second time. Image registration follows the actual deferred VMA
image resource; imported images are excluded. The reported real bytes are VMA
allocation sizes, not a measurement of unused VMA blocks or total driver heap
usage. Pipeline registrations include compute and immutable-sampler variants
and survive descriptor-cache eviction while objects remain referenced. Vulkan
does not expose their driver allocation sizes: pipeline byte/key fields are
unavailable and zero by convention.

Counter availability is explicit. `first_use_compiles` counts successful native
pipeline creation whose cache-miss origin was an actual raster frame, including
asynchronous workers; warm initialization, cache hits and failed creation do
not increment it. Raster allocation counters measure scoped GPU image
allocations, not every C++ allocation. `coverage_flushes` counts successful
encoded native color-island tile-to-parent composites, not atlas resets,
direct 1x passes, failed copies or GPU completion.
Layer overflow events and their real allocation bytes have separate counters;
they are not draw counts or pixel areas. Unimplemented kinds/counters/reasons
remain unavailable rather than pretending to be measured zeros.

**Validation and packaging gate.** The pure production ABI/report target has
10 passing tests, also run with address/undefined-behavior sanitizers. The public
header compiles as C. The three modified Vulkan pipeline implementation files
pass an actual-header C++ syntax check using fetched upstream dependencies.
The root-target GTest source is registered separately so it can link against
the real RenderTarget/Texture implementations without a GPU; both tests pass
in that targeted build. The actual proc-table test source also passes a syntax
check with `FLUTTER_ENGINE_NO_PROTOTYPES`, using its proc-table API calls.
These targeted checks do not establish a complete engine/GN build,
generated-shader link, GPU
execution, pixel parity or an under-500-MiB memory result.

`kAvioCoveragePolicyImplemented` is true for the integrated source policy.
AA capability advertisement and public coverage initialization share that
implementation fact; Linux Vulkan Impeller must be compiled and device format
support must still pass initialization. The default config remains legacy
4x/4x; coverage requires an explicit negotiated config. Actual generated shader
headers and targeted C++ integration checks passed before enabling the source
capability. This does not satisfy the independent native build, artifact and
GPU execution release gates. The user waived the visual-comparison campaign
for this iteration.
Native profile/debug/release artifacts have not been rebuilt for this change;
their existing checksums are not evidence for these sources and must be updated
from actual build outputs before packaging. No source-only check approves a new
look or fabricates a native artifact digest. The existing Screen/EN50 code and
backdrop failure propagation retain their separate ownership.


### Patch 56: exact empty content and retained root frame facts (EG-6, EG-7, DS2)

**Design note.** ABI9 negotiates empty-content, item-effects and output-ground
features with exact root-target opportunities. These require root-target mode;
a generic compositor cannot opt in. `EmptyContent` consumes the matching
view/display/opportunity once, with no backing store, damage, render completion
or GPU submission. It clears old content rather than acknowledging a
`NoVisualChange`. Whole-scene preroll checks actual root paint bounds before
surface/target acquisition. It ignores the raster cache and never paints the
scene. Material or client-preview sidecars prevent true empty; authored output
ground is a separate pixel-free Scene fill and can accompany empty content.
Rejected empty revisions preserve the prior engine/producer baseline. Accepted
empty revisions retire the engine's cached root target through normal collection
and release its transient owner; actual submitted GPU readers retain custody.
The next pixel frame uses ordinary exact typed acquisition again.

Root item opacity, output ground and Ready Static/Live revisions are immutable frame facts,
not placement properties. Native validation permits only a sole-child root
prefix, including mandatory DPR/offset transforms and other root fact decorators.
Sibling, duplicate, clip/filter/opacity ancestors and malformed facts reject
before target acquisition. Scene image snapshots reject these view-only facts.
The metadata layer paints original child pixels without an opacity saveLayer,
including at external alpha zero. Framework composited-layer updates reuse the
same framework layer and retain child painting. Changing root facts promotes the
root to a fresh ordinary buffered raster; it does not reuse a held producer BO
through an unimplemented metadata-only custody shortcut. Since Patch 64 that
promotion is whole-target catch-up damage on a diffed frame: the buffer is
rendered whole while the published logical frame damage stays the exact pixel
change (usually empty).

Nullable root effect opacity means no author. An authored identity opacity is
an explicit group declaration; its nonzero monotonic declaration ID travels with
the exact revision and changes when the body author is replaced. The engine does
not infer group lifetime from alpha or WM placement. Per-view raster TRACE spans
carry the actual Flutter view ID passed to the matching submit operation, rather
than the optional platform-view identifier used for composition slices.

Ground is either one full logical-output ARGB convenience fill or up to four
non-overlapping finite, positive logical view-root rectangles with packed
straight ARGB. Split ScreenWidget regions retain their exact geometry before
DPR; they do not turn into one full physical-output color. Native/root validation
checks bounds against the logical frame; the host/Scene checks its current visible
logical extent. Null author means absent authority, an authored empty descriptor
means explicit clear. All metadata pointers and bounded region arrays are borrowed
only during the matching present callback; conversion allocates no heap memory.

Source correctness checks include the production ABI9 header oracle and real
Flow/UI/rasterizer/embedder translation-unit syntax where dependencies exist.
The registered native Flow, embedder and rasterizer regression fixtures and
framework tests need a full freshly built engine/Flutter test environment to
execute. Native debug/profile/release artifacts, linked GTK/Avio boots, driver
validation and memory captures remain release gates; no rebuilt checksum or
measured memory saving is claimed by these source changes.

### Patch 57: ready Static/Live root content (DS2)

**Design note.** `FlutterAvioReadyContent` carries one nonzero intended-content
revision with explicit Static or Live kind. Pending decode/shader loading must
not author readiness. Both kinds use strict sole-child root validation and the
exact accepted buffered generation, then report acceptance before Desktop
relinquishes its bootstrap painter. A null framework revision emits an ordinary
native offset layer while preserving framework layer identity and child paint.
Empty content cannot carry a ready revision, and unknown kinds fail validation.

Static sealing is separately requested and accepts only Static with the exact
accepted source view, revision and generation. Scene retains that selected BO
before the live engine view/pool detach. Live frames and placeholders cannot
acknowledge a static seal. This single authority replaces the earlier static-only
sketch; it does not add a second competing revision. Source tests do not establish
installed wallpaper continuity or executed removal/retained-reader fences.

### Patch 58: GTK/GLES bounded native-four-sample Coverage (EG-9)

**Design note.** GTK negotiates ABI9 AA/report before AOT data and backing-store
creation. The shared cold GL/GLES3 probe requires exact native four samples in
RGBA8 and D24S8, complete offscreen attachments, and explicit resolve into a
single-sample texture. It restores bindings/scissor state and deletes only its
temporary objects. Unsupported native capabilities retain declared legacy
MSAA4; an admitted Coverage context that later fails cannot silently fall back.
Coverage root framebuffers require complete single-sample RGBA color without
depth/stencil, validated with a current reactor context and restored bindings.

The GL renderer replays original geometry/stencil in fixed four-sample color
tiles and independent one-sample layer banks. It does not sample Vulkan's native
mask cache or use its analytic quad path. A distinct fixed prefix scratch texture
copies borrowed host-FBO pixels before seeding tiles, avoiding texture feedback
and preserving crop/orientation on both endpoints. Wrapped host names remain
borrowed and are never deleted. Warm color/scratch/layer images are realized
before raster frames; their descriptor samples, extents and mip counts fit the
8-MiB coverage and 4-MiB layer bounds.

GL has no portable physical image/program allocation-byte query. These bounds
are descriptor bounds, unlike Vulkan's exact allocation bounds. The ABI9 entry
tail reports independent availability: counts 1, nominal bytes 2, actual bytes
4, uniform texture descriptor 8, physical descriptor multiplicity 16, observed
leases 32. Vulkan's physical image census marks 1|2|4|16. Pipeline census marks
counts only. GL regions expose aggregate lifetime count rows and distinct
per-image nominal/descriptor rows; physical bytes and leases stay unavailable
with a typed reason, never measured zero. Unknown kinds, bits and raw formats
survive bounded transport. Cache ownership/idle release cannot be inferred from
these allocation counts.

Executed hosted checks include production-header capability/state fixtures,
bounded-region/packing fixtures, ABI/report contracts and allocated-image
lifetime fixtures, including focused ASAN/UBSAN runs. Actual GTK/backend/Flow/
embedder translation-unit compilation checks use real dependency headers; some
validation-only ANGLE/RapidJSON/GTK headers differ from native DEPS and do not
replace a pinned GN build. GTK and blit regression fixtures are registered and
source-compiled where available. Native debug/profile/release builds, linked
GTK/Avio boots, real driver GL/Vulkan rendering and memory captures remain
release gates. No native artifact digest, pixel parity or measured saving is
established here. Continuous-AA requests remain off by default.

### Patch 59: headless Linux Vulkan golden harness (TL-3)

**Design note.** `impeller_golden_tests_vk` renders directly with a real
ContextVK and a selected Vulkan ICD, without GLFW, a display server, WSI surface
or swapchain. Generated native shader archives and validation layers are
required. Linux golden fixtures use the actual Vulkan backend instead of the
skip-only stub; the runner rejects an empty selection and any skipped test.

Coverage fixtures render the same authored geometry through legacy native4,
negotiated Coverage, and a test-only aliased1 reference. The reference disables
scene AA and routes nested offscreen-MSAA requests through an injected allocator
that creates only one-sample attachments, preserving available load content and
rejecting multisample existing attachments. It never negotiates a fictitious
one-sample Coverage policy or alters production defaults. SDF primitive fixtures
explicitly request the existing SDF shader path. Text uses a real pinned fixture
font; missing fonts, shader libraries, native capabilities or readback fail.

The class/site catalog includes SDF primitives, text, shadows, linear/radial
gradients, fills/strokes/arcs/borders, fractional/rotated images, nested clip
classes, clear/src/screen/dstIn, clipped backdrop filters and focused Shell-site
replicas, across light/dark at scales 1, 1.25 and 2 and four fractional phases.
These replicas exercise the rendering operators; they do not establish pixels
of installed Shell widgets. The EN50-reverted run requires a separately built
source revision and is not emulated by a runtime flag.

Readback submits the actual image-to-host-buffer transfer, waits for its own
bounded completion, validates status, invalidates host memory, and preserves
exact RGBA bytes (including BGRA conversion). Callback custody survives timeout.
PNG export uses the real Skia encoder's standard straight-alpha conversion;
opaque test backgrounds avoid confusing this with render differences. Failed
comparisons still write their native4/Coverage/aliased1 PNGs, edge masks and
digest for review, while the process returns failure.

The edge mask is actual native4-versus-aliased byte inequality. Coverage must be
byte-identical outside it and differ by at most one channel byte inside it.
SDF/text/shadow/gradient classes require complete byte equality. The CPU contract
tests prove that missing readback, a one-byte interior change and a two-byte
edge change fail. The Screen old-fetch fixture independently asserts zero
interior delta; edge deltas remain documented operator changes, not accepted
parity. Source compilation and CPU tests are not an executed GPU golden run,
visual approval, EN50-reverted proof or rebuilt native-engine artifact.

### Patch 60: opt-in continuous classes and destination-aware coverage (EG-8)

**Design note.** Stable class bits name rectangle, rounded-rectangle,
rounded-superellipse and oval clips, bordered rounded rectangles, arcs, and
image edges. Requests remain zero by default; per-backend implementation masks
are separate from known class identities. GLES and Metal do not inherit Vulkan
support. A borrowed Vulkan device additionally supplies the authoritative
`native_sample_shading_enabled` logical-device creation fact: querying physical
support cannot establish that the device enabled it. Unknown classes, unavailable
backend/device support, singular transforms, expression overflow and missing
source shader variants fail closed.

Continuous clips retain an immutable ordered expression of at most 16 actual
transformed analytic primitives. Identical predicates deduplicate; exact
intersect/difference contradictions canonicalize to empty instead of producing
a smooth rim. Signed distances for original geometry and every clip combine
before one smoothstep coverage evaluation at each original native sample
position. Rectangle/rrect/oval and all four rounded-superellipse quadrants retain
their own shape parameters. Border/arc and image-edge sources defer own coverage;
nonspatial color-filter and source snapshots retain a raw source plus its owned
primitive for the final evaluation. Spatial blur/image-filter inputs consume
intrinsic shape coverage and receive only the final clip thereafter.

A fixed native4 destination prefix preserves lane identity for fractional Src,
Clear and all supported coefficient operators. Each affected ordered draw ends
its previous render pass, copies the native4 color island into a distinct native4
image, transitions it outside render passes, resumes color/depth/stencil loads,
and computes `mix(D, Blend(S,D), F)` with the original blend coefficients. It
never reads a resolved one-sample prefix or samples its render attachment.
External coverage transfer remains after the final combined source-over coverage.
The prefix is warmed and counted inside the same 8-MiB actual Vulkan bank;
requesting continuous classes reduces the color tile to 224 pixels. The default
mask-zero tile remains 256. Budget admission rejects oversized native requirements.

The common Vulkan source variants preserve original shader bindings and add two
readonly storage buffers and a native4 sampler. The immutable clip expression
uploads once per recorder state; a 704-byte draw control carries original blend
factors, raster origin and an optional deferred own primitive. Those variants
are Vulkan-only GN shader inputs, not GLES/Metal assets. Cold existing pipeline
requests also warm their available variants; actual new frame compiles remain
observed first-use events. Arbitrary runtime shader variants are not fabricated.

A Vulkan Coverage renderer prewarms a dedicated shader-data arena: one
1,024,000-byte block in each of four frame entries (4,096,000 descriptor bytes)
for native clip controls. A renderer requesting continuous classes also prewarms
128 immutable expression slots and expands this cold arena to three blocks per
entry (12,288,000 descriptor bytes). Each recorder has a fixed 128-state
upload cache. The arena retains owning buffer views through queued
recordings and submitted command buffers; a still-owned ring entry or capacity
exhaustion fails the frame instead of replacing storage, allocating a one-off
buffer, waiting on the GPU, or growing the expression cache. This bounds the new expression/control storage; Patch 61 separately bounds
the typed recorder storage. Neither claim means the whole renderer or its
existing native command implementation allocates no CPU memory.

The typed DeviceBuffers resource kind reports successful engine-owned VMA buffer
allocations with descriptor and actual VMA allocation sizes. Its registration
stays with the deferred native buffer resource and decrements after VMA buffer
destruction. Together with the image provider, the raster allocation counter
observes successful native image and buffer allocation events during raster
frames. VMA containing-block slack, driver-private allocations and physical
pipeline memory are not inferred from those sizes; pipeline compilation remains
a separate observed counter.

Local checks compile the actual production C++ and registered fixtures, compile
all common wrapper shaders through genuine Impellerc, and run production
region/budget, analytic-geometry, control-custody and ABI contracts. These are
source/CPU/shader-compiler checks. Full GN/link, GPU execution, native artifact
rebuilds and per-class changed-look approval remain separate gates. No continuous
class is enabled in the host's default request, and no pixel parity or memory
saving measurement is asserted.

**Shader size.** Impellerc inlines every GLSL call, and `ShaderLibraryVK`
creates a module for every archived shader at context creation, whose SPIR-V
the driver keeps for the context's life. The evaluator therefore has a single
call site per variant: one loop over primitive slots, where slot -1 is the
draw's own geometry (gated by `runtime.y`) and slots 0.. are the ordered
expression, and one five-tap loop (value, then the +dx, -dx, +dy, -dy central
differences, each formed by the same single add or subtract, with the same
0.01 and /0.02 arithmetic). Before this, own geometry plus the loop body each
inlined the value and four gradient taps, ten evaluator copies per variant:
the 44 variants were 20.8 MiB (489-565 KiB each, measured as 21 MiB of
NVIDIA-retained SPIR-V in the live shell host). They are now 3.0 MiB
(3,117,028 bytes, largest 139,748). Non-continuous archive shaders, GLES
outputs and all reflection (bindings, layouts, pipeline keys) are
byte-identical. `AvioContinuousShaderVK.VariantsStayWithinSpirvBudget` (CPU
only, in `impeller_golden_tests_vk`) caps each variant at 144 KiB and the set
at 3,200 KiB; the previous source fails both.

`AvioContinuousShaderVK.EvaluatorExportsExactNativeFourSamples` executes the
evaluator through `avio_continuous_solid_fill` with production primitive
packing (`AvioContinuousClip` factories, `Transform`,
`MakeAvioContinuousControl`): every primitive kind, own geometry, ordered
difference up to 16 slots, empty expressions and both final-blend paths under
fractional, rotated and sheared transforms, into native-four float32 samples.
It exports the resolved floats and their digest. Old and new evaluators
exported byte-identical floats on lavapipe, SwiftShader and the NVIDIA
driver; a mathematically equal rewrite (`inversesqrt`) changed 1,646 floats
by at most 1.2e-7, so the comparison detects last-bit drift. This fixture bypasses
`GetAvioContinuousPipeline` and the tiled recorder: it proves the evaluator,
not the full continuous replay path, which remains unexecuted on GPU.


### Patch 61: exact clip segments and bounded typed recording (EG-2/3/4)

The upstream asynchronous prewarm API and legacy 27-key catalogue are preserved.
Legacy SDF contexts queue those descriptor-derived variants without a constructor
join. Coverage uses its separate actual caller catalogue: all source keys are
queued asynchronously from their original default descriptors before any source
join, then the same keys and required replay derivatives are validated during
controlled initialization. A failed queue or compile keeps public readiness
false. The manual clip family retains its exact no-colour-write default
descriptor for that queue. The typical 160 Coverage visitor entries count source
keys, not native PSOs or measured pipeline memory; mask/default futures and
required derivatives are separate. Four production visitor tests execute the
queue-before-join and failure/readiness contract. The upstream legacy-set and
GPU playground tests remain registered; their source compilation does not imply
native GPU execution.

**Design note.** The real DisplayList pre-pass freezes exact declaration tokens,
ancestry, source/geometry facts, simultaneous layer demand and earlier/later
fringe-correlation proofs before rendering. The recorder consumes those facts
for complete root/layer routes and actual painter segments. Draw ordinals and
matching bounding boxes never substitute for a clip declaration. Incomplete
proofs keep the bounded native four-sample replay; destination-read boundaries
preserve their original ordered prefix.

Clip recipes retain all ancestors, operations and original native geometry.
Rectangle, rounded-rectangle, superellipse and oval inner/outer proofs partition
known full pixels from uncertain fringe boxes. Paths and partition overflow
remain conservative uncertain boxes; native mask replay determines every lane.
The executor renders proven interiors at 1x. Four unresolved mask lanes combine
with AND/AND-NOT before any resolve. Deep/native geometry recipes stream one
raster tile at a time through the existing fixed R8MS4 scratch and shared
bounded depth/stencil storage. The exact logical claim survives through its
consumer encoding, then releases; the native image independently remains alive
through GPU completion. Queue-ordered read-to-next-write barriers permit reuse
without accumulating leases for the entire desktop or losing older parents.

This refines the research's coefficient shortcuts where attachment rounding
invalidates their premise. Opacity alone does not prove an already encoded
source colour. A raw one-draw joint coefficient variant requires source-owned
full/uniform evidence and an exact encoded opaque value; currently exact solid
colour endpoints satisfy it. Other opaque-first uniform SrcOver groups, including
eligible immutable captured-backdrop plus fills, shade the original source into
the standing distinct 1x UNORM resolve attachment first. Each painter store
therefore preserves original source rounding before the final nearest encoded
source and complete joint clip mask composite. That private composite samples
the declared RGBA8/BGRA8 format in high precision and reconstructs its stored
UNORM byte codes before applying coverage; it does not reuse the original
source shader's relaxed-precision approximation of an already stored byte.
Actual captured texture evidence
requires full own geometry, pixel-centre source uniformity, exact opaque sample
support and the same declaration token. Ordinary F16/bilinear colour, a format
label or opacity metadata alone cannot manufacture that evidence.

Translucent groups without an opaque overwrite retain native4 partial pixels.
For example, 16 alpha-byte-1 paints over destination byte96 at quarter coverage
can produce native result100 versus transparent-group result98. Even one
translucent paint can differ by a byte if its lane is rounded before resolve.
Raw opaque shader colour can differ too if it has not yet been stored to UNORM.
Clear/Src and other original operators render proven full pixels at 1x and
partial pixels in the bounded native4 fringe, preserving original destination
lanes. No source alpha or colour is changed just to admit an optimization.

Ordinary Vulkan image quads retain the original physical grid and explicit
canonical four lane masks; source sampling, strict crop and nonlinear transfer
retain their original ordering. A coefficient 1x image route requires separate
encoded-source and correlation proof; unproven varying/translucent sources keep
native lane geometry. Analytic legacy SDF-only scope admission follows the real
owner's `use_sdfs` eligibility. Source selection and target routing are separate:
mixed or clipped scopes keep the original UberSDF, complex-superellipse and
circle source shaders in bounded native4 replay, including their existing
alpha/gamma and gradient behavior. The complete unclipped scope proof alone
permits the existing SDF source to render at 1x. Circle retains its original
eligibility independently of the EN50 flag. Candidate SDF eligibility never
proves equal samples for a neighboring clip segment.

With an explicitly requested continuous clip, Circle and complex-superellipse
variants export their original normalized signed distance before coverage and
use sample-qualified geometry inputs. The wrapper combines that own distance
with the retained clip expression before one coverage evaluation. Their default
shader bodies remain unchanged. The native4 source keys are cold-warmed for the
actual geometry/operator forms; a registered mixed glyph/clip/SDF/gradient
fixture checks first-frame compilation when run on a Vulkan device. The source
fixture has been compiled here, but that device execution remains pending.

Native tile raster origins remain aligned to eight pixels. This preserves the
original ordered-dither modulo-eight indices and derivative-quad phase when
original gradient source shaders are translated into scratch tiles. Replay
refuses an unaligned translated origin before binding or drawing. Production
planner tests cover negative/fractional bounds and odd scratch capacities;
neither those tests nor source compilation establishes native pixel parity.

A cold aggregate CPU bank holds at most 64 recorder controls, 8,192 packets,
32,768 bindings and 32,768 vertex views. Pending resources and names use fixed
inline storage, and exact weak identities never resurrect on slot reuse.

Those caps, and the DisplayList pre-pass plan's, are address space, not a
preallocation. Every bounded container is one `CoverageFixedVector`
(`coverage_recorder_storage.h`): `std::inplace_vector` semantics over aligned
uninitialized bytes, placement-constructed on append, refusing at capacity, and
destroyed explicitly by `pop_back`/`resize`/`clear`. The bank and plan have
user-provided constructors, so the single cold `std::make_shared` no longer
value-initializes them. Previously it zeroed and constructed every slot, which
made the measured 20,627,440-byte bank and the 2.7 MiB plan fully resident on
every Coverage shell host. The bank constructs a control on first demand,
reusing the lowest free one first, and never moves it. A reservation constructs
its packet, vertex views, bindings and ordinal chunk while it owns the
admission word, and `Commit` assigns those live elements. `ReclaimIdle`
destroys the whole generation, releasing its shared owners, and the next
generation reuses the lowest indices. Resident memory is therefore the touched
high-water mark, with no custom allocator, `madvise` or zero-state convention.
Element addresses, the single standing allocation and frame-path
allocation-freedom are unchanged.

`AiksContext` adds one `kCoverageCpuStorage` (kind 10) census entry for both
standing owners, next to the content context's report. `entries` is the
constructed high-water element count. `nominal_bytes` is the capacity bytes.
`real_bytes` is the high-water element bytes, which bounds residency before page
rounding. Production CPU tests construct the bank and plan the same way
`std::make_shared` does, in a fresh no-huge-page mapping, and check residency
with `mincore`. With zero-filled storage they observe 5,036 of 5,036 and 683 of
683 pages resident; the patched types stay within their vector headers plus
the touched elements. Further tests cover refusal at capacity, explicit
destruction on reclaim, and census counters that read zero before first use and
keep their high water across reclaim/reset.
Capacity or busy admission fails before submission, without frame-path bank
growth. Logical clip-write markers distinguish replayed clips from a fill's own
winding/stroke operations. Source proofs apply to exactly one accepted draw.
Encoded pipelines, buffers and textures transfer to actual native submission
custody independently of recorder reuse; retaining only a C++ command-buffer
wrapper is not a GPU fence.

Production CPU admission/lifetime tests, genuine C++ fixture typechecks and
actual official shader compilation validate the source contracts. The deep 512,
SDF-only and native first-frame cold-pipeline fixtures are authored and registered;
they are not reported as executed GPU goldens. Full GN/link, rebuilt artifacts,
driver execution, changed-look approval and measured memory/performance remain
independent release gates. The requested default continuous mask stays zero.

Selected Vulkan targets also retain their external image/view destruction and
backing-store collection callbacks in the existing native texture source.
Native framebuffer caches retire before those callbacks, and the final
submitted source reader owns the foreign baton. A required completion-sync FD
failure settles the exact target as RasterFailed for host quarantine; an
unproved idle wait never turns it into a Produced frame. Acquisition failures
keep the original collection guard, and callbacks run once in destruction then
collection order. Actual CPU tests exercise the source cache with fake Vulkan
deleters; native timeline execution remains a separate gate.

### Patch 62: exact attachment policy for cached Vulkan render passes

`RenderPassVK` caches one `VkRenderPass` and `VkFramebuffer` per texture
subresource. Upstream keyed that cache on `(sample_count, mip_level, slice)`
alone and replayed the cached render pass for any later full-area pass over the
same subresource. A framebuffer only needs render-pass compatibility, but
`vkCmdBeginRenderPass` executes the render pass object's own load/store
operations and initial layouts. A texture that is first cleared and later
loaded therefore had its later `kLoad` silently replaced by the first pass's
`CLEAR` from `UNDEFINED`.

Upstream rarely reaches this: its multisample targets always clear the
transient attachment and its single-sample targets keep one load policy. The
negotiated Coverage policy (patch 55b/61) renders straight into the imported
single-sample root without a render area. Its first segment clears that root;
every backdrop-filter or advanced-blend boundary starts a new segment whose
initialization pass, tile composites and certified clip passes load the
composited prefix. With the old key each later segment re-cleared the root, so
a non-preserved frame kept only the content painted after its last backdrop
boundary (the greeter showed only its power button on black).

The cache key now includes the exact attachment descriptions that
`RenderPassBuilderVK::Build` bakes in (`RenderPassPolicyVK`). Equal policies
still share one cached render pass and framebuffer; a texture holds one entry
per distinct policy (two or three for a Coverage root). Bounded passes still
bypass the cache. `LaterSegmentLoadsTheParentAfterAnEarlierClear` and
`PolicyDistinguishesLoadActionAndInitialLayout` cover the contract with the
mock Vulkan driver; hardware validation is the Coverage greeter.

### Patch 63: a diff baseline is a tree that was diffed

Partial repaint diffs each layer tree against the view's previous tree and
reads the paint region every old layer recorded when it was itself diffed.
Upstream stores a tree as the previous tree only after a raster that diffed
it, so every baseline carries those regions. Avio added paths that keep a tree
as the view's last successful tree without a diff: an accepted empty frame
submitted before raster (`SubmitAvioEmptyFrame`), and, until Patch 64, a frame
whose root facts changed (rasterized without `FrameDamage`). A fresh tree
rejected before raster is never stored, and a retained tree cannot newly fail
fact validation. The next diff then found no region for an old layer. A
removed child reached `DiffContext::AddDamage` with an invalid `PaintRegion`
and dereferenced null on the raster thread (Shell host SIGSEGV when a new
window's item views appeared, 2026-10-07); a changed child silently lost its
old-region damage.

`LayerTree` now records that `FrameDamage::ComputeDamage` diffed it
(`has_paint_regions`). `ComputeDamage` uses a previous tree only when it
carries paint regions; otherwise the frame is damaged whole, exactly as with
no previous tree, and the current tree's regions are recorded for the next
frame. One rule in the damage owner covers every submission path, so a new
early return cannot reintroduce the hazard. `FrameDamageRepaintsWholeAfterAnUndiffedPreviousTree`
crashes without the change; `FrameDamageNarrowsAgainstADiffedPreviousTree`
keeps exact narrow damage after a diffed tree. After Patch 64 the remaining
undiffed baselines are the accepted empty frame and upstream's surfaces without
partial repaint; both correctly become "no baseline", which matches the host's
all-slots-unknown and the producer's damage-journal reset after empty content.
The rule stays a runtime rule, never an assertion: an assertion would fire on
every empty-to-content transition.

### Patch 64: a root-facts change is whole-target catch-up damage

Patch 56 promotes a root-facts change (item opacity, output ground, ready
revision, effect declaration) to a freshly rendered buffered generation. It did
so by skipping `FrameDamage` for that frame. The frame then published no logical
damage, recorded no paint regions, and became an undiffed baseline: the hazard
Patch 63 contains by damaging the next frame whole. Reusing the MSAA
`raster_replaces_whole_target` bit or `FrameDamage::Reset()` before raster
cannot carry the obligation: `ComputeDamage` clears `Reset`, and `Raster`'s exact
no-change exit precedes the whole-target branch, so a facts-only frame (an
empty pixel diff by design) would end as `NoVisualChange` and its facts would
never reach the producer.

`Rasterizer::DrawToSurfaceUnsafe` now always diffs when partial repaint is
supported, against the same baseline it compares facts with. A facts change adds
the whole frame as additional damage, upstream's `existing_damage` idiom and
the same as a buffer of age 0 in wlroots, KWin and Smithay. Buffer damage is
therefore never empty, `Raster` resets it itself (Impeller's cost heuristic or
the multisampled branch) and renders the whole target exactly as before, and
the frame publishes exact logical damage and records its paint regions. The
two rasterizer no-change shortcuts outside `Raster` (generic metadata damage
and the post-raster check) also require unchanged facts. `FrameDamage`,
`Raster`, the empty path and the baseline storage rules are unchanged: a facts
change the producer did not accept leaves the old facts in the baseline and is
detected again at the next opportunity.

Effects: the other pool slots stay exact instead of being poisoned by absent
frame damage, the producer's damage journal continues, and the frame after a
fade step is a narrow raster again. `rootFactsOnlyChangeRastersWholeTargetWithExactFrameDamage`,
`rootFactsOnlyChangeOnMultisampledTargetReportsNoBufferDamage`,
`frameAfterRootFactsChangeDiffsNarrowly`,
`rootFactsChangeThenRemovedChildDamagesOnlyItsOldRegion`,
`rootFactsChangeNotAcceptedIsResentNextOpportunity` and
`metadataPathRootFactsChangeIsNotNoVisualChange` fail without the change;
`contentAfterAcceptedEmptyFrameIsWholeDamaged` pins Patch 63's empty-frame rule
and `FrameDamageCatchUpLeavesFrameDamageExact` pins the damage split.

### Patch 65: shell hit regions are collected from the frame they describe

Avio Shell input claims used to travel beside the frame: Dart measured boxes
after the frame (`localToGlobal`, post-frame callbacks), sent them over a
separate WM command and the compositor paired them with whichever pixels were
bound when the command arrived. Flutter-on-Fuchsia, viz `HitTestRegionList`
and SurfaceFlinger all commit the hit regions in the same present as the
content. This patch is the Flow and `dart:ui` half of that model; the embedder
ABI that delivers the set is added separately and nothing exposes it yet.

`SceneBuilder.pushAvioHitRegion({rect, enabled, kind, offset, oldLayer})`
pushes a retained `AvioHitRegionLayer`. It is a `ContainerLayer` with an
offset, like `AvioFrameMetadataLayer`: its children paint unchanged
(translated), it adds no paint bounds (a tree holding only a hot zone stays
empty content), passes the children's renderable-state flags through (an
ancestor opacity never becomes a saveLayer) and diffs as pixels only through
its offset. A changed rect, enabled bit or kind is never damage. Dart rejects
non-finite or negative rects and offsets; the layer still fails closed on a
malformed rect, offset or unknown kind if that check is bypassed.
`subtree_has_avio_hit_region` propagates through `ContainerLayer::Add` and the
`SceneBuilder` ancestor stack, including retained subtrees.

Collection happens only in the frame-facts preroll: `LayerTree::Preroll` gains
`collect_frame_facts`, which the rasterizer's whole-scene fact pass (the one
that already decides empty content before target acquisition) sets. Only that
preroll clears and refills the tree's inline `AvioHitRegionSet` (at most
`kMaxAvioHitRegionsPerFrame` = 64, no heap allocation, order-exact equality),
so a later raster preroll with a partial damage cull, or a raster-side
zero-damage exit, cannot lose or alter the claim. Each enabled layer is
resolved with the same state that positions its pixels: the current matrix,
the scene cull (every ancestor clip over the whole frame, also when the facts
preroll is given a partial cull) and the outstanding opacity. Regions are
device-space, collected pre-order. A disabled layer, a layer under zero
outstanding opacity, an empty rect and a fully clipped rect claim nothing. A
transform that is not axis-aligned (rotation other than a multiple of 90
degrees within 1e-5, skew or perspective), a non-finite result or a 65th
region invalidates the whole set: the frame fails closed rather than claiming
a derived bounding box. A hit-region layer may sit in the sole-child root
prefix above root frame facts, like a transform. Scene image snapshots ignore
claims; they never collect them.

`AvioHitRegionLayerTest`, `AvioHitRegionLayerTreeTest` and
`AvioHitRegionDiffTest` (flow_unittests) cover transform, clip, offset,
opacity zero, axis alignment, malformed authoring, the cap, root versus nested
pre-order, paint bounds and flag pass-through, paint, the root prefix, facts
versus raster prerolls and diff damage.
`AvioHitRegionSceneBuilderCollectsRetainedClaims` (ui_unittests) covers the
`dart:ui` path, retained subtrees under a moved parent, a disabled update of
the same engine layer and Dart-side rect validation.
