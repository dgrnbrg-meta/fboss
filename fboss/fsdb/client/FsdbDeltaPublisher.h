// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include "fboss/fsdb/client/FsdbPublisher.h"

namespace facebook::fboss::fsdb {
class FsdbDeltaPublisher : public FsdbPublisher<OperDelta> {
  using BaseT = FsdbPublisher<OperDelta>;

 public:
  using BaseT::BaseT;
  ~FsdbDeltaPublisher() override {
    cancel();
  }

 private:
#if FOLLY_HAS_COROUTINES
  folly::coro::Task<void> serviceLoop() override;
#endif
};
} // namespace facebook::fboss::fsdb
