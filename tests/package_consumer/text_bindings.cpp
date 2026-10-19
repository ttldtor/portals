// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "tests/support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <cstdlib>
#include <iostream>
#include <source_location>

namespace {
using namespace portals;
constexpr CellSize VIEWPORT{40, 16};
constexpr std::string_view UNICODE = "e\xcc\x81 \xe7\x95\x8c \xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb";

void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

template <class T, class E>
T checked(std::expected<T, E> result, std::source_location where = std::source_location::current()) {
  if (!result) {
    std::cerr << "checked API failure at line " << where.line() << '\n';
    std::exit(EXIT_FAILURE);
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

// Called only by this installed gate. Keep layout/pin preparation in the test, not the native guides.
void prepareViews(Application &app, ManagedScene &owner) {
  std::optional<LayoutSnapshot> layout;

  checked(app.tree().frame([&] {
    const auto bounds = checked(CellRect::from({}, VIEWPORT));

    checked(layoutTree(app.tree(), app.root(), bounds, bounds));
    layout = checked(LayoutSnapshot::capture(app.tree(), owner.root()));
  }));
  checked(owner.prepareViews(app, *layout));
}

std::vector<std::string> rowText(const ModelFrame<TextSegment> &frame) {
  const auto range = checked(frame.read({0, static_cast<std::size_t>(frame.rowCount())}));
  std::vector<std::string> values;

  for (const auto &row : range.rows()) {
    values.emplace_back(row.value.bytes());
  }

  return values;
}

void sameFrame() {
  auto app = checked(Application::create());
  auto owner = checked(ManagedScene::create(*app));
  auto model = checked(owner->state(std::string("Hello, Portals!")));
  ui::Ref<LineEdit> input;
  ui::Ref<VirtualList<TextSegment>> wordsList;
  ui::Ref<VirtualList<TextSegment>> graphemesList;
  std::optional<ModelFrame<TextSegment>> wordsFrame, graphemesFrame;
  ui::VirtualListConfig<TextSegment> wordsConfig, graphemesConfig;

  wordsConfig.prepared = [&](const ModelFrame<TextSegment> &frame,
                             const std::optional<ModelRequest<TextSegment>> &request) {
    check(!request, "resident words require no provider work");
    wordsFrame = frame;

    return std::expected<void, TreeError>{};
  };
  graphemesConfig.prepared = [&](const ModelFrame<TextSegment> &frame,
                                 const std::optional<ModelRequest<TextSegment>> &request) {
    check(!request, "resident graphemes require no provider work");
    graphemesFrame = frame;

    return std::expected<void, TreeError>{};
  };

  auto recipe =
    ui::vbox(ui::lineEdit(input, model), ui::label("Words"),
             ui::virtualList(ui::words(input.draftText()), wordsConfig).reference(wordsList).height(4),
             ui::label("Graphemes (including spaces)"),
             ui::virtualList(ui::graphemes(input.draftText()), graphemesConfig).reference(graphemesList).height(4),
             ui::button("Quit", [] {
               ui::quit();
             }));
  auto column = checked(ui::mount(*owner, std::move(recipe)));
  const auto root = checked(app->tree().inspect(owner->root()));
  const auto children = checked(app->tree().inspect(column));

  check(root.children.size() == 1 && children.children.size() == 6, "one column with the same six children");
  checked(owner->prepare(*app, VIEWPORT));
  prepareViews(*app, *owner);
  check(rowText(*wordsFrame) == std::vector<std::string>{"Hello,", "Portals!"}, "initial owning words");

  checked(model.set(std::string(UNICODE)));
  checked(owner->prepare(*app, VIEWPORT));
  prepareViews(*app, *owner);
  check(rowText(*wordsFrame) ==
          std::vector<std::string>{"e\xcc\x81", "\xe7\x95\x8c", "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb"},
        "external model reaches words before the same frame pin");
  check(graphemesFrame->rowCount() == 5 && rowText(*graphemesFrame)[1] == " ",
        "external model reaches extended graphemes including spaces in the same frame");

  const auto pinnedWords = *wordsFrame;
  const auto pinnedGraphemes = *graphemesFrame;
  const auto retained = rowText(pinnedWords);

  checked(model.set(std::string(UNICODE)));
  checked(owner->prepare(*app, VIEWPORT));
  prepareViews(*app, *owner);
  check(wordsFrame->dataRevision() == pinnedWords.dataRevision() &&
          graphemesFrame->dataRevision() == pinnedGraphemes.dataRevision(),
        "equal external text creates no extra model revision");

  checked((input.text() = std::string("  ") + std::string(UNICODE) + "\t").result());
  checked(owner->prepare(*app, VIEWPORT));
  prepareViews(*app, *owner);
  check(wordsFrame->dataRevision() == pinnedWords.dataRevision() &&
          checked(wordsFrame->locateRow({1, 2})).state == RowLookupState::FOUND,
        "equal ordered words preserve identity across delimiter changes");
  check(graphemesFrame->dataRevision() != pinnedGraphemes.dataRevision(), "changed whitespace is grapheme data");

  checked(app->router().setFocus(input.node()));
  checked(app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'a', {.control = true}}));
  checked(app->router().dispatch(TextInputEvent{U'Z', {}}));
  checked(owner->prepare(*app, VIEWPORT));
  prepareViews(*app, *owner);
  check(rowText(*wordsFrame) == std::vector<std::string>{"Z"} &&
          rowText(*graphemesFrame) == std::vector<std::string>{"Z"},
        "real routed editor input reaches both bound lists");

  checked(owner->close());
  owner.reset();
  check(rowText(pinnedWords) == retained && pinnedGraphemes.rowCount() == 5,
        "both owning model pins survive complete scene cleanup");
}

void failedMount() {
  auto app = checked(Application::create());
  auto owner = checked(ManagedScene::create(*app));
  ui::Ref<LineEdit> input;
  auto captured = std::make_shared<int>(1);
  std::weak_ptr<int> retired = captured;
  ui::VirtualListConfig<TextSegment> config;

  config.prepared = [captured](const ModelFrame<TextSegment> &, const std::optional<ModelRequest<TextSegment>> &) {
    return std::expected<void, TreeError>{};
  };

  auto recipe = ui::vbox(ui::lineEdit(input, "one two"),
                         ui::virtualList(ui::words(input.draftText(), {.maxTokens = 1}), std::move(config)));

  captured.reset();

  const auto failed = ui::mount(*owner, std::move(recipe));

  if (!failed) {
    const auto *cause = std::get_if<TreeError>(&failed.error().cause);

    std::cout << "initial refusal phase=" << static_cast<int>(failed.error().phase)
              << " cause=" << (cause ? static_cast<int>(*cause) : -1) << '\n';
  }

  check(!failed && std::get_if<TreeError>(&failed.error().cause) &&
          std::get<TreeError>(failed.error().cause) == TreeError::NODE_LIMIT &&
          failed.error().phase == ManagedPhase::STATE,
        "initial transform refusal preserves typed phase and cause");
  check(input.node() == Node<LineEdit>{} && retired.expired(), "refusal releases captures and publishes no Ref");
  check(checked(app->tree().inspect(owner->root())).children.empty(), "refusal leaves no partial subtree");
  checked(owner->close());
}

void idle() {
  example::ReplayTerminal terminal({}, 100, VIEWPORT);
  auto app = checked(Application::create({}, terminal.clock()));
  auto owner = checked(ManagedScene::create(*app));
  ui::Ref<LineEdit> input;
  auto recipe =
    ui::vbox(ui::lineEdit(input, "Hello, Portals!"), ui::virtualList(ui::words(input.draftText())).height(4),
             ui::virtualList(ui::graphemes(input.draftText())).height(4));

  checked(ui::mount(*owner, std::move(recipe)));

  const auto stats = checked(owner->run(terminal));

  check(stats.frames == 1 && terminal.closed, "live graph stays idle and closes its own terminal");
}
} // namespace

/** @brief Drive installed live bindings, model synchronization, refusal and cleanup. */
int main() {
  sameFrame();
  failedMount();
  idle();
}
