# Impeller Golden Tests

This is the executable that will generate the golden image results that can then
be sent to Skia Gold via the
[golden_tests_harvester](../../tools/golden_tests_harvester).

Running these tests should happen from
[//flutter/testing/run_tests.py](../../testing/run_tests.py). That will do all
the steps to generate the golden images and transmit them to Skia Gold. If you
run the tests locally it will not actually upload anything. That only happens if
the script is executed from LUCI.

Example invocation:

```sh
./run_tests.py --variant="host_debug_unopt_arm64" --type="impeller-golden"
```

The macOS harness supports its existing backends. The additional Linux Vulkan
target renders headless without GLFW or a display server, using the selected
real Vulkan ICD and validation layers.

## Linux Vulkan coverage checks

Build `impeller_golden_tests_vk` in a dependency-complete engine checkout:

```sh
ninja -C out/host_debug_unopt impeller_golden_tests_vk
VK_ICD_FILENAMES="$PWD/out/host_debug_unopt/vk_swiftshader_icd.json" \
  out/host_debug_unopt/impeller_golden_tests_vk \
  --working_dir=/tmp/avio-goldens \
  --gtest_filter='AvioSites/*:*ScreenVulkanComparisonTest*:GoldenEdgeComparisonTest.*' \
  --gtest_output=xml:/tmp/avio-goldens/results.xml
```

The path above assumes the engine `src` directory as the working directory.
Use the variant's actual validation-layer path when it is not installed in the
loader's search path. Missing native dependencies, unsupported capabilities,
an empty test selection and every skipped test fail; no result is synthesized.
Other existing Vulkan DisplayList goldens can be selected with `*/Vulkan`.

`AvioSites` renders its class/site catalog in light and dark at scales 1, 1.25
and 2, each at four fractional phases. It writes native4, Coverage, aliased1
and actual edge-mask PNGs, XML maximum deltas and a digest with the real GPU
description. The aliased reference uses one sample for root and nested
offscreen targets and disables AA in scene commands. It does not modify
production AA defaults. Text requires the real `Roboto-Regular.ttf` fixture.

Raw premultiplied renderer bytes determine the edge mask and comparisons.
Outside the native4-versus-aliased mask Coverage must match exactly; inside it
each channel differs by at most one byte. SDF, text, shadow and gradient classes
require all bytes exact. PNG export applies normal straight-alpha encoding;
the opaque background keeps that conversion from obscuring edge differences.
Failed comparisons retain their output artifacts and still exit unsuccessfully.
Focused site replicas test operators and geometry, while installed Shell and
composed-output captures remain separate evidence.

Run the same target from a separately rebuilt EN50-reverted revision to check
the gradient backport's independent revert contract. A runtime flag cannot
substitute for that source/build check. The old-fetch Screen fixture documents
real operator changes and now asserts zero interior delta; an unexecuted test
or a recorded edge delta establishes neither pixel parity nor user approval.
Hosted source compilation and CPU oracle tests do not constitute a native
Vulkan golden run.

## Continuous evaluator checks

`AvioContinuousShaderVK.VariantsStayWithinSpirvBudget` needs no GPU: it bounds
the archived `avio_continuous_*` SPIR-V that every Vulkan context hands to the
driver. `AvioContinuousShaderVK.EvaluatorExportsExactNativeFourSamples` draws
the Patch 60 evaluator with production primitive packing into native-four
float32 samples and writes `avio_continuous_shader_samples.f32` plus its digest.
A shader-only refactor is pixel-equivalent when builds before and after it
export identical files on the same ICD:

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  out/host_profile/impeller_golden_tests_vk --working_dir=/tmp/after \
  --gtest_filter='AvioContinuousShaderVK.*'
cmp /tmp/before/avio_continuous_shader_samples.f32 \
  /tmp/after/avio_continuous_shader_samples.f32
```

Digests differ between ICDs, whose transcendental functions differ. The
fixture binds the variant directly; it does not run the tiled recorder's
continuous replay.

## Adding tests

To add a golden image test, the `impeller_golden_tests` target must be modified
to generate the correct image and modification to its generated `digest.json`.
If a test case is added to [golden_tests.cc](./golden_tests.cc), for example
"GoldenTests.FooBar", that will turn into the golden test
"impeller_GoldenTests_Foobar" automatically if the `SaveScreenshot()` function
is used.

The examples in `golden_tests.cc` use GLFW for rendering the tests, but
technically anything could be used.  Using the `SaveScreenshot()` function will
automatically update the `GoldenDigest::Instance()` which will make sure that it
is included in the generated `digest.json`. If that function isn't used the
`GoldenDigest` should be updated manually.
