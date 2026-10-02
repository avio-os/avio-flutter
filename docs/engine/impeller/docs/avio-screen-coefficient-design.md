# Avio Screen coefficient design

Status: implemented source slice of ENDGOAL-PLAN EG-2, awaiting engine build,
GPU evidence and visual review. This note records the design for review; it
does not claim the required pre-implementation DN review occurred. The full
EG-2 AA contract and its TL-3 harness prerequisite remain incomplete.

## Decision owners and scope

`impeller/geometry/color.h::kLastCoefficientBlendMode` owns the boundary
between coefficient and advanced blends. `Entity::kLastPipelineBlendMode`
aliases it. Canvas, Flow's root readback decision, DisplayList dispatch,
AtlasContents, BlendFilterContents and VerticesSimpleBlendContents consume
that authority. Screen is the last coefficient blend; Overlay and later
modes retain their advanced routes. Flow still treats a root backdrop filter
as a readback, and conservatively requests full repaint for advanced blends
without requiring backend capabilities in DiffContext.

`ContentContextOptions::ApplyToPipelineDescriptor` owns fixed-function
factors; `kPorterDuffCoefficients` owns the image/filter/vertex shader
coefficients. The static table-size assertion uses the shared boundary.
Premultiplied Screen is `src + dst * (1 - src)`: RGB factors ONE and
ONE_MINUS_SRC_COLOR; alpha factors ONE and ONE_MINUS_SRC_ALPHA. The shader
row is `{1, 0, 1, 0, -1}` and Screen is its own inverse when texture/vertex
inputs exchange roles. The GPU fixtures cover both representations instead
of folding a solid-color filter through `Color::Blend` on the CPU.

No public ABI, SDK contract, host negotiation, sample policy, pool key or
depth/stencil policy changes in this slice. Existing render/encode/submit
failure propagation remains intact. The separately adapted upstream
flutter#193306 and #193176 backports prevent single-sample feedback and keep
shared-backdrop content depth; they do not enable a 1x policy. EN48's Flip
secondary and EN50's gradient implementation remain independent. The original
EN50 patch reverse-applies without reverting the Screen or backdrop changes.

The shared boundary is a constant comparison, with no new runtime allocation,
input-path work, cross-process GPU wait, release timer or buffer custody.
Ordinary Screen draws stop allocating advanced-blend source snapshots and,
on non-fetch backends, stop requesting backdrop flips. First-use pipeline
creation can still allocate: prewarm and the required no-first-frame-compile
measurement are outstanding. This slice does not claim the entire engine
raster path is allocation-free.

## Pixel operator and acceptance

The mathematical identity does not establish parity with the previous
multisampled implementation. On Vulkan/ANV, the previous Screen path rendered
an 8-bit MSAA source snapshot. `framebuffer_blend.frag::ReadDestination`
averages all four destination samples; the shader writes the resulting
Screen value to each covered sample. The new fixed-function blend instead
runs on each sample with the unquantized fragment source. With partial clip
coverage over a high-contrast destination, these are different operators.
Source snapshot quantization and shader/fixed-function precision can also
change interior rounding.

`ScreenPreviousFetchAndPipelineClippedEdgesGolden` is a comparison fixture,
not a look-acceptance oracle. It renders the old production fetch contents
and new pipeline under the same actual Vulkan 4x rounded clip (radius 22),
gradient and two-tone destination, in light/dark appearances at scales 1,
1.25 and 2. A third render identifies actual partially covered clip pixels.
The test requires real edge pixels and a delta greater than 1 somewhere on
the edge, checks uncovered pixels stay identical, and records maximum edge,
fully covered interior and uncovered deltas. Its golden puts old on the left
and new on the right. A narrow Canvas test peer submits the old contents with
normal clip depth and draw ordering; production routing has no test switch.

The comparison's clip mask is not TL-3's general aliased-versus-msaa4 edge
mask. It is evidence about this specific operator change. ENDGOAL-PLAN §1.6
still requires byte-identical pixels outside its edge mask, a maximum delta
of 1 inside it, and user-approved captures for any larger or intended change.
Passing a comparison test that detects differences does not approve them.
The 1/255 tolerances in the new CPU-oracle GPU fixtures test the new Screen
semantics, not parity with the previous path.

## Regression and execution gates

| Fixture | What it exercises | Required execution |
| --- | --- | --- |
| `DisplayListLayerDiffTest.AdvancedRootBlendsConservativelyGateImpellerOnly` | Impeller Screen has no root readback; Overlay/Multiply still do, and the non-Impeller classification is unchanged | `flow_unittests` |
| `ContentContextOptionsTest.ScreenBlendIsAPipelineBlend` | Fixed-function RGB/alpha factors at 1x and 4x | `impeller_unittests` |
| `BlendCoefficientsTest.*` | Production coefficient equation and vertex inverse against `Color::Blend` | `impeller_unittests` |
| `AiksTest.ScreenPipelineMatchesCpuOracleWithoutOffscreen` | Actual rendered Screen paint against the CPU reference | [laptop] Vulkan and GLES |
| `AiksTest.ScreenImageFiltersAndVerticesMatchCpuOracleWithoutOffscreen` | DrawImage/DrawImageRect Screen color filters and textured colored DrawVertices, reaching coefficient shaders | [laptop] Vulkan and GLES |
| `ScreenVulkanComparisonTest.ScreenPreviousFetchAndPipelineClippedEdgesGolden` | Previous fetch versus pipeline on real Vulkan 4x clip edges, six paired images and recorded deltas | [laptop] Vulkan with fetch and MSAA |
| `AiksTest.BackdropGroupSharedSnapshotReservesContentDepth` | Shared backdrop content ordering | [laptop] Vulkan and GLES |
| `CanvasGLESTest.AdvancedBlendWithoutOffscreenMSAAHasNoFeedbackLoop` | Mock GLES rejects a sampled attached target on the 1x restore path | `impeller_unittests` |

The cache recorder overrides both `CreateOffscreen` and
`CreateOffscreenMSAA`. New Screen oracle draws require no cache request of
any kind. The previous fetch comparison must record its MSAA source snapshot,
so the allocation assertions cannot pass by overlooking that path. Source
image texels use explicit private RGBA8 textures, and the CPU reference
decodes the uploaded premultiplied bytes. Readback allocation, encoding,
submission and completion failures fail through an empty result assertion.

In a dependency-complete debug engine tree, build `flow_unittests` and
`impeller_unittests` and run the named fixtures with gtest XML output. Select
the actual `/Vulkan` and `/OpenGLES` parameterizations for the laptop GPU
checks, not an unsupported backend. The Vulkan comparison requires fetch
and 4x support with assertions. Report each named test as "N passed, 0
skipped [laptop]"; backend/setup skips do not satisfy the gate. Run the full
engine suites and the preserved-target multisampling regression too.

TL-3's headless Linux Vulkan screenshotter, SwiftShader target and strict
edge-mask diff tooling are not implemented by this patch. The stock Linux
golden stub skips tests and supplies no look proof. TL-3 must run the Screen
golden and site replicas, plus the EN50-reverted matrix, before the larger
AA program can satisfy its acceptance gates.

On the laptop, capture Spaces in light and dark at scales 1, 1.25 and 2 with
the plan's `scripts/dev-screenshot.sh` procedure, against the frozen L0
references. Preserve candidate/base images and diffs, include the real card
clip and glow edges, and obtain the user's visual approval for changed pixels.
Do not merge or package this candidate as a validated look change before
that evidence and review. No laptop capture or approval is claimed here.

## Remaining coverage prerequisites and local evidence

Current depth/stencil consumers remain: Canvas clip-depth ordering and replay,
ClipContents difference/intersection clips, InlinePassContext depth/stencil
requirements, stencil-then-cover fills, and overdraw-preventing geometry.
The fractional-rect 0.124-pixel clip rule assumes the existing sample grid.
Patch 45's foreground masks use the default MSAA snapshot request. Explicit
layer-versus-coverage sample requests, coverage clips/paths, bounded coverage
and layer regions, classifier reports, pool keys and prewarm are still needed
before lowering root/layer samples or removing depth/stencil. No such policy
change is enabled here.

Local source checks compile the production coefficient header and
`Color::Blend` in a standalone C++20 oracle (50 color/decal cases), check
changed-line clang-format and whitespace, and reverse-check the original
EN50 patch. They are not an Impeller build or GN/GPU result. This checkout
lacks full engine dependencies, generated shader headers, a configured GN
output and a GPU. The newly authored engine fixtures have not been built or
executed here; engine, golden, first-use and laptop gates above remain open.
The patch-inventory script passes its eight new Screen checks but retains the
base's miss for the Animator global-frame-clock pattern; `animator.cc` is
unchanged by this slice. The standalone compiler also reports the existing
`Color::Blend` exhaustiveness warning. Neither is reported as a green full
engine build.
