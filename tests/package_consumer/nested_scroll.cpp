// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
using namespace portals;
int failures{};

void check(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    ++failures;
  }
}

template <class T, class E>
T checked(std::expected<T, E> result, std::source_location where = std::source_location::current()) {
  if (!result) {
    std::cerr << "Checked public operation failed at line " << where.line() << '\n';
    throw std::runtime_error("Installed nested scroll consumer operation failed");
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

// Test-only transport: spaced input permits normal runner preparation between assertions.
// The ASCII cursor oracle observes actual FrameWriter output, independent of widget geometry.
class MemoryTerminal : public ApplicationTerminal {
public:
  SchedulerClock clock() noexcept {
    return {[](void *context) noexcept {
              return static_cast<MemoryTerminal *>(context)->time_;
            },
            this};
  }

  PointerCaptureCapabilities pointerCaptureCapabilities() const noexcept override {
    return {true, true, true};
  }

  TerminalRead read(TerminalDeadline deadline, std::stop_token stop) override {
    if (stop.stop_requested()) {
      TerminalRead result{};
      result.status = TerminalStatus::STOPPED;
      return result;
    }

    const auto requested = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline.time_since_epoch()).count());

    if (requested <= time_) {
      TerminalRead result{};
      result.status = TerminalStatus::DEADLINE;
      return result;
    }

    constexpr std::uint64_t INPUT_INTERVAL = 100;
    constexpr std::uint64_t END = 2000;
    const auto nextAt = next_ * INPUT_INTERVAL;

    time_ = std::min(requested, END);

    if (next_ <= 18 && nextAt <= time_) {
      time_ = nextAt;

      TerminalRead result;

      result.bytes = "x";

      if (next_ == 16) {
        size_ = {26, 30};
        screen_.assign(30, std::string(26, ' '));
        result.resized = size_;
      }

      ++next_;

      return result;
    }

    TerminalRead result{};
    result.status = time_ == END ? TerminalStatus::END_OF_INPUT : TerminalStatus::DEADLINE;
    return result;
  }

  std::expected<CellSize, TerminalError> size() const override {
    return size_;
  }

  std::expected<void, TerminalError> submit(std::string bytes) override {
    if (!pending_.empty()) {
      return std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_PENDING});
    }

    pending_ = std::move(bytes);

    return {};
  }

  TerminalWrite flush(TerminalDeadline, std::stop_token) override {
    constexpr std::size_t MAX_CAPTURE = 2 * 1024 * 1024;
    const auto count = pending_.size();

    if (captured_.size() + count > MAX_CAPTURE) {
      return {TerminalStatus::FAILED, 0, count, TerminalError{TerminalErrorKind::OUTPUT_LIMIT}};
    }

    apply(pending_);
    captured_ += pending_;
    pending_.clear();

    return {TerminalStatus::PROGRESS, count, 0, {}};
  }

  std::expected<void, TerminalError> close() noexcept override {
    closed = true;

    return {};
  }

  char cell(CellPoint point) const {
    if (point.x < 0 || point.y < 0 || point.x >= size_.width || point.y >= size_.height) {
      return '\0';
    }

    return screen_[static_cast<std::size_t>(point.y)][static_cast<std::size_t>(point.x)];
  }

  bool rowContains(std::int32_t row, std::string_view text) const {
    return row >= 0 && row < size_.height && screen_[static_cast<std::size_t>(row)].find(text) != std::string::npos;
  }

  void dump(std::ostream &output) const {
    for (const auto &row : screen_) {
      output << '|' << row << "|\n";
    }
  }

  bool closed{};

private:
  void apply(std::string_view bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      const auto value = bytes[i];

      if (value == '\x1b' && i + 1 < bytes.size() && bytes[i + 1] == '[') {
        i += 2;

        std::array<int, 2> parameters{};
        std::size_t parameter{};

        for (; i < bytes.size() && (bytes[i] < '@' || bytes[i] > '~'); ++i) {
          if (bytes[i] >= '0' && bytes[i] <= '9' && parameter < parameters.size()) {
            parameters[parameter] = parameters[parameter] * 10 + bytes[i] - '0';
          } else if (bytes[i] == ';') {
            ++parameter;
          }
        }

        if (i < bytes.size() && (bytes[i] == 'H' || bytes[i] == 'f')) {
          cursor_ = {std::max(1, parameters[1]) - 1, std::max(1, parameters[0]) - 1};
        } else if (i < bytes.size() && bytes[i] == 'J' && parameters[0] == 2) {
          screen_.assign(static_cast<std::size_t>(size_.height),
                         std::string(static_cast<std::size_t>(size_.width), ' '));
        }
      } else if (value == '\r') {
        cursor_.x = 0;
      } else if (value == '\n') {
        ++cursor_.y;
      } else if (value >= ' ' && value <= '~') {
        if (cursor_.x >= 0 && cursor_.y >= 0 && cursor_.x < size_.width && cursor_.y < size_.height) {
          screen_[static_cast<std::size_t>(cursor_.y)][static_cast<std::size_t>(cursor_.x)] = value;
        }

        ++cursor_.x;
      }
    }
  }

  CellSize size_{26, 10};
  CellPoint cursor_{};
  std::vector<std::string> screen_{10, std::string(26, ' ')};
  std::uint64_t time_{};
  std::uint64_t next_{1};
  std::string pending_;
  std::string captured_;
};
} // namespace

int main() {
  using namespace portals;
  MemoryTerminal terminal;
  auto outerController = checked(ScrollController::create());
  auto innerController = checked(ScrollController::create());
  ui::Ref<ScrollArea> outer;
  ui::Ref<ScrollArea> inner;
  ui::Ref<Button> first;
  ui::Ref<Button> last;
  Application *application{};
  Node<Widget> initialButtonParent;
  int stage{};
  ui::SceneOptions sceneOptions;
  ScrollAreaOptions outerOptions;
  ScrollAreaOptions innerOptions;
  BorderOptions frame;

  frame.glyphs = PrimitiveGlyphs::ASCII;
  outerOptions.glyphs = PrimitiveGlyphs::ASCII;
  innerOptions.glyphs = PrimitiveGlyphs::ASCII;
  innerOptions.frame = frame;
  innerOptions.placement = ScrollPlacement::ON_FRAME;
  sceneOptions.clock = terminal.clock();

  auto scene = ui::scene(
    [&](ManagedScene &, Application &app) {
      application = &app;

      return ui::border(
        ui::scrollArea(
          outer,
          ui::vbox(ui::label("Outer header"),
                   ui::scrollArea(inner,
                                  ui::vbox(ui::button(first, "First",
                                                      [] {
                                                      }),
                                           ui::label("Inner 1"), ui::label("Inner 2"), ui::label("Inner 3"),
                                           ui::label("Inner 4"), ui::label("Inner 5"), ui::label("Inner 6"),
                                           ui::label("Inner 7"), ui::label("Inner 8"), ui::label("Inner 9"),
                                           ui::label("Inner 10"),
                                           ui::button(last, "Last",
                                                      [] {
                                                      })),
                                  innerOptions, innerController) |
                     ui::width(18) | ui::height(6),
                   ui::label("Tail 1"), ui::label("Tail 2"), ui::label("Tail 3"), ui::label("Tail 4"),
                   ui::label("Tail 5"), ui::label("Tail 6"), ui::label("Tail 7"), ui::label("Tail 8"),
                   ui::label("Tail 9"), ui::label("Tail 10"), ui::label("Tail 11"), ui::label("Tail 12")),
          outerOptions, outerController),
        frame);
    },
    sceneOptions);

  scene | ui::configure([&](ManagedScene &owner) -> std::expected<void, ManagedError> {
    return owner.onInput([&](const InputEvent &) -> std::expected<bool, ManagedError> {
      auto &tree = application->tree();
      auto &router = application->router();
      auto offset = [](const std::shared_ptr<ScrollController> &controller) {
        return checked(controller->snapshot()).vertical.offset.raster;
      };
      auto wheel = [&] {
        const auto layout = checked(scrollAreaLayout(tree, inner.node()));
        const auto clip = checked(inner.geometry()).clip;
        const auto point = checked(translated(layout.placement.viewportOrigin, layout.geometry.surface.origin()));
        const auto row = std::max(point.y, clip.origin().y);

        checked(router.dispatch(MouseInputEvent{point.x, row, MouseButton::WHEEL_DOWN, MouseAction::SCROLL, {}}));
      };
      auto capture = [&] {
        const auto layout = checked(scrollAreaLayout(tree, inner.node()));
        const auto point = checked(translated(layout.placement.vertical.origin, layout.geometry.surface.origin()));

        checked(router.dispatch(MouseInputEvent{point.x, point.y, MouseButton::LEFT, MouseAction::PRESS, {}}));
        check(checked(router.pointerCapture()) == inner.node(), "Nested ON_FRAME thumb did not acquire capture");
      };

      std::cout << "stage " << ++stage << '\n';

      if (stage == 1 || stage == 6 || stage == 7 || stage == 9 || stage == 11 || stage == 18) {
        terminal.dump(std::cout);
      }

      switch (stage) {
      case 1: {
        const auto layout = checked(scrollAreaLayout(tree, inner.node()));
        const auto outerGeometry = checked(outer.geometry());
        const auto point = checked(translated(layout.placement.vertical.origin, layout.geometry.surface.origin()));

        initialButtonParent = checked(tree.inspect(first.node())).parent;
        check(layout.placement.viewport == CellSize{16, 4}, "ON_FRAME charged an extra gutter or frame inset");
        check(outerGeometry.surface.origin() == CellPoint{1, 1}, "Outer Border inset was not applied once");
        check(checked(router.hitTest(point)) == inner.node(),
              "Frame-edge scrollbar input does not match its paint owner");
        check(terminal.cell({0, 0}) == '+' && terminal.cell({0, 3}) == '|',
              "Outer Border is absent from actual output");
        check(terminal.cell(point) != ' ', "ON_FRAME scrollbar was not painted at its input edge");
        check(checked(router.hitTest({0, 3})) != first.node(), "Content input escaped the outer Border clip");
        checked(inner.setOffset(ScrollAxis::VERTICAL, {7, 0}));
        break;
      }
      case 2:
        wheel();
        check(offset(innerController) == 8 && offset(outerController) == 0,
              "Partial inner wheel consumption leaked a remainder to outer");
        break;
      case 3:
        wheel();
        check(offset(innerController) == 8 && offset(outerController) == 3,
              "Boundary inner wheel did not bubble to outer");
        break;
      case 4:
        checked(outer.setOffset(ScrollAxis::VERTICAL, {}));
        checked(inner.setOffset(ScrollAxis::VERTICAL, {}));
        checked(first.focus());
        break;
      case 5:
        check(offset(innerController) == 0 && checked(router.focused()) == first.node(),
              "Initial nested focus was not revealed");
        wheel();
        break;
      case 6:
        check(offset(innerController) == 3 && checked(router.focused()) == first.node(),
              "Unchanged focus undid manual nested scrolling during preparation");
        check(checked(first.geometry()).clip.empty(), "Offviewport focused button retained a visible footprint");
        check(checked(router.hitTest(checked(first.geometry()).surface.origin())) != first.node(),
              "Offviewport focused button received input beyond the nested clip");
        check(!terminal.rowContains(0, "First"), "Offviewport focused button painted through the outer frame");
        checked(last.focus());
        break;
      case 7: {
        const auto geometry = checked(last.geometry());

        check(offset(innerController) == 8 && checked(router.focused()) == last.node(),
              "New focus did not rearm nested reveal");
        check(!geometry.clip.empty() && geometry.clip.contains(geometry.surface),
              "Revealed target is outside its cumulative clip");
        check(checked(router.hitTest(geometry.clip.origin())) == last.node(), "Revealed paint/input target disagrees");
        check(terminal.rowContains(geometry.surface.origin().y, "Last"),
              "Revealed button was not painted on the terminal");
        check(checked(tree.inspect(first.node())).parent == initialButtonParent,
              "Scrolling remounted the ordinary button subtree");
        checked(outer.focus());
        checked(outer.setOffset(ScrollAxis::VERTICAL, {}));
        checked(inner.setOffset(ScrollAxis::VERTICAL, {}));
        break;
      }
      case 8:
        check(offset(innerController) == 0 && checked(router.focused()) == outer.node(),
              "Manual reset with unchanged outer focus was not retained");
        checked(owner.ensureVisible(last.node()));
        break;
      case 9:
        check(offset(innerController) == 8 && checked(router.focused()) == outer.node(),
              "Explicit nested reveal changed keyboard focus or failed to reveal the target");
        check(terminal.rowContains(checked(last.geometry()).surface.origin().y, "Last"),
              "Explicit nested reveal did not reach actual terminal paint");
        checked(inner.setOffset(ScrollAxis::VERTICAL, {}));
        break;
      case 10:
        capture();
        checked(tree.setVisible(outer.node(), false));
        check(checked(router.pointerCapture()) == Node<Widget>{}, "Ancestor hide retained nested drag capture");
        break;
      case 11:
        check(terminal.cell({2, 3}) == ' ', "Hidden nested subtree left a painted ghost");
        checked(tree.setVisible(outer.node(), true));
        break;
      case 12:
        capture();
        checked(tree.setEnabled(outer.node(), false));
        check(checked(router.pointerCapture()) == Node<Widget>{}, "Ancestor disable retained nested drag capture");
        break;
      case 13:
        checked(tree.setEnabled(outer.node(), true));
        break;
      case 14:
        capture();
        checked(owner.remove(inner.node()));
        check(checked(router.pointerCapture()) == Node<Widget>{}, "Removing captured nested area retained capture");
        check(!inner.geometry(), "Removed nested Ref still exposed live geometry");
        break;
      case 15: {
        const auto before = offset(outerController);

        checked(router.dispatch(MouseInputEvent{100, 100, MouseButton::LEFT, MouseAction::MOVE, {}}));
        checked(router.dispatch(MouseInputEvent{100, 100, MouseButton::LEFT, MouseAction::RELEASE, {}}));
        check(offset(outerController) == before, "Late retired drag events changed the remaining outer controller");
        checked(outer.setOffset(ScrollAxis::VERTICAL, {1000, 0}));
        check(offset(outerController) > 0, "Remaining outer content has no scroll range for resize oracle");
        break;
      }
      case 16:
        break; // This input also publishes the larger terminal size; preparation follows normally.
      case 17:
        check(offset(outerController) == 0, "Resize failed to clamp outer offset after inner retirement");
        checked(owner.remove(outer.node()));
        check(!owner.requiresPointerMotion(), "Last scroll removal retained the native motion requirement");
        break;
      case 18:
        check(terminal.cell({2, 3}) == ' ', "Removing the outer area left a painted ghost");
        check(checked(router.pointerCapture()) == Node<Widget>{}, "Final retirement retained pointer capture");
        break;
      default:
        check(false, "Unexpected additional input stage");
      }

      return true;
    });
  });

  const auto code = scene | ui::runApp(terminal);

  check(code == 0 && terminal.closed && stage == 18, "Finite public runner skipped stages or terminal cleanup");
  std::cout << "Nested installed consumer: " << stage << " stages, " << failures << " failures\n";

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
