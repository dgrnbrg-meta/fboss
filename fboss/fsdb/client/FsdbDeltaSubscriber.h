// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include "fboss/fsdb/client/FsdbSubscriber.h"
#include "fboss/fsdb/if/gen-cpp2/fsdb_oper_types.h"

namespace facebook::fboss::fsdb {
class FsdbDeltaSubscriber : public FsdbSubscriber<OperDelta> {
  using BaseT = FsdbSubscriber<OperDelta>;

 public:
  using FsdbOperDeltaUpdateCb = typename BaseT::FsdbSubUnitUpdateCb;
  using BaseT::BaseT;
  ~FsdbDeltaSubscriber() override {
    cancel();
  }

 private:
#if FOLLY_HAS_COROUTINES
  folly::coro::Task<void> serviceLoop() override;
#endif
};
} // namespace facebook::fboss::fsdb
