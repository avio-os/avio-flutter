// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <filesystem>
#include <iostream>

#include "flutter/fml/command_line.h"
#include "gtest/gtest.h"
#include "impeller/base/validation.h"
#include "impeller/golden_tests/golden_digest.h"
#include "impeller/golden_tests/working_directory.h"

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  const auto command_line = fml::CommandLineFromPlatformOrArgcArgv(argc, argv);
  std::string directory;
  if (!command_line.GetOptionValue("working_dir", &directory) ||
      directory.empty()) {
    std::cerr << "--working_dir=<existing output directory> is required\n";
    return 1;
  }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error || !std::filesystem::is_directory(directory)) {
    std::cerr << "Cannot create golden output directory\n";
    return 1;
  }
  impeller::testing::WorkingDirectory::Instance()->SetPath(directory);
  impeller::ImpellerValidationErrorsSetFatal(true);
  int result = RUN_ALL_TESTS();
  const auto* tests = ::testing::UnitTest::GetInstance();
  if (tests->test_to_run_count() == 0 || tests->skipped_test_count() != 0) {
    std::cerr << "Empty or skipped golden selection is a failure\n";
    result = 1;
  }
  // Failed comparisons still leave their actual PNGs/XML/digest reviewable.
  // Exporting artifacts cannot turn a failed or skipped run into success.
  if (!impeller::testing::GoldenDigest::Instance()->Write(
          impeller::testing::WorkingDirectory::Instance()))
    result = 1;
  std::cout << tests->successful_test_count() << " passed, "
            << tests->skipped_test_count() << " skipped\n";
  return result;
}
