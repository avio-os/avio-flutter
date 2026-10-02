// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_NATIVE_TEARDOWN_STATUS_H_
#define FLUTTER_IMPELLER_RENDERER_NATIVE_TEARDOWN_STATUS_H_

namespace impeller {

// An owning caller's terminal teardown receipt. Only its exact Context supplies
// proof; losing that Context later cannot replace an unknown result with
// success.
class NativeTeardownStatus {
 public:
  bool RecordProof(bool safe) {
    safe_ = safe_ && safe;
    return safe_;
  }

 private:
  bool safe_ = true;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_NATIVE_TEARDOWN_STATUS_H_
