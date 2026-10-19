// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <source_location>
#include <string>
#include <thread>

namespace {
using namespace portals;
constexpr CellSize VIEWPORT{40, 12};

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

template <class T, class E>
T checked(std::expected<T, E> result, std::source_location where = std::source_location::current()) {
  if (!result) {
    std::cerr << "API failure at line " << where.line();
    if constexpr (std::same_as<E, TreeError>) {
      std::cerr << " tree=" << static_cast<int>(result.error());
    }
    std::cerr << '\n';
  }
  check(result.has_value(), "unexpected checked API failure");

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

bool treeCause(const ManagedError &error, TreeError cause) {
  const auto *value = std::get_if<TreeError>(&error.cause);

  return value && *value == cause;
}

void prepareViews(Application &app, ManagedScene &scene) {
  std::optional<LayoutSnapshot> layout;

  checked(app.tree().frame([&] {
    const auto bounds = checked(CellRect::from({0, 0}, VIEWPORT));

    checked(layoutTree(app.tree(), checked(app.tree().root()), bounds, bounds));
    layout = checked(LayoutSnapshot::capture(app.tree(), scene.root()));
  }));
  checked(scene.prepareViews(app, *layout));
}
} // namespace

/** @brief Check installed finite transforms, cold/live bridges and retirement without opening a terminal. */
int main() {
  using namespace portals;

  auto finite = checked(words(checked(GraphemeText::from(" one\t two "))));
  auto pinnedToken = checked(finite.at(1));

  check(finite.size() == 2 && pinnedToken.bytes() == "two", "finite whole-token split");
  check(checked(graphemes(checked(GraphemeText::from("e\xcc\x81 ")))).size() == 2,
        "graphemes include whitespace and preserve combining cluster");
  check(!words(checked(GraphemeText::from("one two")), TextSplitOptions{.maxTokens = 1}),
        "finite cap rejects whole output");
  check(!words(checked(GraphemeText::from("")), TextSplitOptions{.maxTokens = 0}), "zero cap is invalid");

  ManagedOptions budget;
  budget.maxStates = 8;
  auto app = checked(Application::create());
  auto owner = checked(ManagedScene::create(*app, budget));
  ui::Ref<LineEdit> input;
  ui::Ref<VirtualList<TextSegment>> list;
  auto unpublished = owner->liveText(input.draftText());

  check(!unpublished && treeCause(unpublished.error(), TreeError::CLOSED), "unpublished source is closed");

  std::optional<ModelFrame<TextSegment>> frame;
  int presentations{};
  auto capture = std::make_shared<int>(7);
  std::weak_ptr<int> retiredCapture = capture;
  ui::VirtualListConfig<TextSegment> config;

  config.prepared = [&, capture](const ModelFrame<TextSegment> &value,
                                 const std::optional<ModelRequest<TextSegment>> &) {
    frame = value;
    ++presentations;

    return std::expected<void, TreeError>{};
  };
  checked(ui::mount(
    *owner, ui::vbox(ui::virtualList(ui::words(input.draftText(), TextSplitOptions{.maxTokens = 2}), std::move(config))
                       .reference(list)
                       .height(2),
                     ui::lineEdit(input, "one two"), ui::virtualList(ui::graphemes(input.draftText())).height(2))));
  capture.reset();
  checked(owner->prepare(*app, VIEWPORT));
  prepareViews(*app, *owner);

  auto live = checked(owner->liveText(input.draftText()));
  auto pinnedText = checked(live.snapshot());
  auto pinnedFrame = *frame;

  check(checked(frame->locateRow({1, 1})).state == RowLookupState::FOUND, "initial token identity");
  checked(setVirtualListSelection(app->tree(), list.node(), RowId{1, 1}));
  checked((input.text() = " one  two ").result());
  checked(owner->prepare(*app, VIEWPORT));
  prepareViews(*app, *owner);
  check(frame->dataRevision() == pinnedFrame.dataRevision(), "same ordered tokens do not republish model");

  checked((input.text() = "one two three").result());
  auto failed = owner->prepare(*app, VIEWPORT);

  check(!failed && failed.error() == TreeError::NODE_LIMIT, "post-accept transform failure is typed");
  check(checked(live.snapshot())->value.bytes() == "one two three", "accepted input is not rolled back");
  check(checked(pinnedFrame.locateRow({1, 1})).state == RowLookupState::FOUND, "last good pin survives failure");

  checked((input.text() = "three four").result());
  checked(owner->prepare(*app, VIEWPORT));
  prepareViews(*app, *owner);
  check(checked(frame->locateRow({1, 2})).state == RowLookupState::FOUND, "changed tokens reset identity once");
  checked(app->tree().read(list.node(), [&](const VirtualList<TextSegment> &view) {
    check(!view.selection(), "new bound list clears missing selection");
  }));

  std::expected<std::shared_ptr<const PropertyChange<GraphemeText>>, TreeError> foreign;
  std::thread thread([&] {
    foreign = live.snapshot();
  });

  thread.join();
  check(!foreign && foreign.error() == TreeError::WRONG_THREAD, "live source is UI-affine");

  {
    auto initialApp = checked(Application::create());
    auto initialOwner = checked(ManagedScene::create(*initialApp));
    ui::Ref<LineEdit> rejected;
    auto invalid = ui::mount(
      *initialOwner, ui::vbox(ui::lineEdit(rejected, "one two"),
                              ui::virtualList(ui::words(rejected.draftText(), TextSplitOptions{.maxTokens = 1}))));

    check(!invalid && treeCause(invalid.error(), TreeError::NODE_LIMIT), "initial mount rejection is typed");
    check(rejected.node() == Node<LineEdit>{}, "failed cold mount publishes no Ref");
  }

  const int beforeRetirement = presentations;

  checked(owner->remove(input.node()));
  checked(owner->prepare(*app, VIEWPORT));
  prepareViews(*app, *owner);
  check(retiredCapture.expired() && presentations == beforeRetirement, "retirement releases callback captures");
  check(!live.snapshot() && live.snapshot().error() == TreeError::STALE_NODE, "explicit access remains stale");
  checked(setVirtualListFirst(app->tree(), list.node(), 1));
  prepareViews(*app, *owner);
  checked(owner->remove(list.node()));
  checked(owner->close());
  owner.reset();

  check(!live.snapshot() && live.snapshot().error() == TreeError::CLOSED, "source closes with owner");
  check(pinnedText->value.bytes() == "one two" && pinnedToken.bytes() == "two" &&
          checked(pinnedFrame.locateRow({1, 1})).state == RowLookupState::FOUND,
        "owning snapshots and pins survive cleanup");
}
