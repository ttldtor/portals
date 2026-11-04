// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/window_source.hpp>

#include <thread>

namespace {
class Generated final : public portals::WindowProvider<int> {
public:
  portals::SourceCapabilities capabilities() const noexcept override {
    return {};
  }

  std::expected<portals::SourceReadReply<int>, portals::SourceError>
  load(const portals::SourceReadCommand &command, portals::PageBuilder<int> &out, std::stop_token) override {
    auto position = command.anchor.kind == portals::SourceAnchorKind::CURSOR ? command.anchor.cursor.token[0] : 0;

    for (std::size_t i = 0; i < command.maxRows; ++i) {
      auto added = out.emplace(portals::RowId{position + i + 1, 1}, 1, static_cast<int>(position + i));

      if (!added) {
        return std::unexpected(added.error());
      }
    }

    return out.ready({command.source, {position, 0}}, {command.source, {position + command.maxRows, 0}},
                     portals::SourceEdge::MORE);
  }
};
} // namespace

int main() {
  using namespace portals;
  auto scheduler = Scheduler::create();

  if (!scheduler) {
    return 1;
  }
  auto tree = WidgetTree::create(**scheduler);

  if (!tree) {
    return 2;
  }
  auto root = (*tree)->root();

  if (!root) {
    return 3;
  }
  auto source = WindowSource<int>::create(**tree, *root, std::make_shared<Generated>());

  if (!source) {
    return 4;
  }
  auto consumer = (*source)->openConsumer();

  if (!consumer) {
    return 5;
  }
  auto request = consumer->request({}, SourceDirection::FORWARD, 3);

  if (!request) {
    return 6;
  }
  bool queued{};
  std::thread worker([&] {
    queued = request->complete(request->load()).has_value();
  });
  worker.join();

  if (!queued || !(*scheduler)->dispatch()) {
    return 7;
  }
  auto pin = consumer->pin();

  return pin && pin->size() == 3 && pin->page(0).rows()[2].value == 2 ? 0 : 8;
}
