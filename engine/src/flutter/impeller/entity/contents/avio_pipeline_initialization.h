// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_AVIO_PIPELINE_INITIALIZATION_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_AVIO_PIPELINE_INITIALIZATION_H_
namespace impeller {
// A partially built content context is available only to its cold pipeline
// factory. Every early constructor return invalidates it; callers cannot use
// drawing resources until all negotiated prewarming has completed.
class AvioPipelineInitialization final {
 public:
  bool IsReady() const { return state_ == State::kReady; }
  bool CanCreatePipelines() const { return state_ != State::kInvalid; }
  class Scope final {
   public:
    explicit Scope(AvioPipelineInitialization& owner) : owner_(owner) {
      owner_.state_ = State::kInitializing;
    }
    ~Scope() {
      if (owner_.state_ == State::kInitializing) {
        owner_.state_ = State::kInvalid;
      }
    }
    void Complete() { owner_.state_ = State::kReady; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    AvioPipelineInitialization& owner_;
  };

 private:
  enum class State { kInvalid, kInitializing, kReady };
  State state_ = State::kInvalid;
};
}  // namespace impeller
#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_AVIO_PIPELINE_INITIALIZATION_H_
