// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/model_position_finite.hpp>

#include <memory>

bool modelPositionHeaderProbe();

namespace {
class EmptySnapshot final : public portals::ModelSnapshot<int> {
public:
  EmptySnapshot() : ModelSnapshot(1, 0) {
  }

  portals::ModelRows<int> read(portals::RowRange) const override {
    return {portals::RangeState::READY, {}, {}};
  }
};
} // namespace

int main() {
  using namespace portals;

  auto resource = ModelResource::create();

  if (!resource || !modelPositionHeaderProbe()) {
    return 1;
  }

  auto snapshot = std::make_shared<EmptySnapshot>();
  auto resolver = FinitePositionResolver<int>::create(*resource, resource->identity, snapshot);
  auto bookmark = ModelBookmark::create(*resource, {PositionKind::GAP, {}, 0, 1});

  if (!resolver || !bookmark) {
    return 2;
  }

  PositionRequest request{resource->identity, 1, 1, *bookmark};
  auto reply = resolver->resolve(request);

  if (!reply || reply->state != PositionResolution::EXACT || !positionReplyMatches(request, *reply)) {
    return 3;
  }

  snapshot.reset();

  auto closed = resolver->resolve(request);

  return !closed && closed.error() == SourceError::CLOSED && bookmark->position().offset == 0 ? 0 : 4;
}
