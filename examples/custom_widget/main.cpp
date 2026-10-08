// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../gallery/common.hpp"
#include "../replay/terminal.hpp"
#include <format>
#include <portals/model.hpp>

namespace {
using namespace portals;
using example::take;

// This provider is application data, independent of Widget and of terminal rendering.
class LevelSnapshot final : public ModelSnapshot<int> {
public:
  LevelSnapshot(std::uint64_t revision, int value) : ModelSnapshot(revision, 1), row_{{1, 1}, value} {
  }

  ModelRows<int> read(RowRange range) const override {
    return {RangeState::READY, std::span(&row_, 1).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

private:
  ModelRow<int> row_;
};

// The widget retains only presentation prepared outside the frame. Its paint method
// never reads a model, formats text, dispatches work, or writes to the terminal.
class LevelMeter final : public Widget {
public:
  void prepare(int value) {
    value_ = value;
    label_ = example::text(std::format("Custom widget | level {}% | Left/Right adjust | F10 quit", value));
  }

  std::expected<CellSize, TreeError> measure(CellSize available) override {
    return available;
  }

  std::expected<void, TreeError> arrange(CellRect rect) override {
    size_ = rect.size();

    return {};
  }

  std::expected<void, TreeError> paint(PaintContext &context) override {
    const auto area = take(CellRect::from({}, size_));
    const auto bar = take(CellRect::from(
      {0, std::min(2, size_.height)}, {static_cast<std::int32_t>(static_cast<std::int64_t>(size_.width) * value_ / 100),
                                       std::max(0, size_.height - 2)}));
    const std::array<RenderCommand, 3> commands{
      FillCommand{area, area, {RgbColor{230, 230, 230}, RgbColor{20, 24, 32}}},
      FillCommand{bar, area, {{}, RgbColor{30, 150, 120}}},
      TextCommand{{}, label_, area, {RgbColor{230, 230, 230}, RgbColor{20, 24, 32}}}};

    return context.submit(commands);
  }

private:
  int value_{};
  CellSize size_{};
  GraphemeText label_ = example::text("");
};

class MeterScene final : public ApplicationScene {
public:
  explicit MeterScene(Application &app)
      : meter_(take(app.tree().emplace<LevelMeter>(app.root()))),
        model_(take(Model<int>::create(app.tree(), meter_, std::make_shared<const LevelSnapshot>(1, level_)))) {
  }

  std::expected<void, TreeError> prepare(Application &app, CellSize) override {
    const auto pin = take(model_->pin());
    const auto value = take(pin.read({0, 1}));

    return app.tree().edit(meter_, [&](LevelMeter &meter) {
      meter.prepare(value.rows()[0].value);
    });
  }

  std::expected<bool, TreeError> input(Application &app, const InputEvent &event) override {
    if (example::quitKey(event)) {
      app.requestStop();

      return true;
    }

    const auto *key = std::get_if<KeyEvent>(&event);

    if (!key || (key->key != InputKey::LEFT && key->key != InputKey::RIGHT)) {
      return false;
    }

    level_ = std::clamp(level_ + (key->key == InputKey::RIGHT ? STEP : -STEP), 0, 100);
    take(model_->publisher().publish(std::make_shared<const LevelSnapshot>(++revision_, level_)));

    return true;
  }

private:
  static constexpr int STEP = 5;
  int level_{50};
  std::uint64_t revision_{1};
  Node<LevelMeter> meter_;
  std::unique_ptr<Model<int>> model_;
};
} // namespace

int main(int argc, char **argv) {
  try {
    const bool check = argc == 2 && std::string_view(argv[1]) == "--check";

    if (argc > 1 && !check) {
      std::cerr << "Usage: portals_custom_widget [--check]\n";

      return 2;
    }

    example::ReplayTerminal terminal({{50, "\x1b[C\x1b[C", {}}, {100, "", CellSize{50, 8}}, {200, "\x1b[21~", {}}},
                                     250);
    auto app = take(Application::create({}, check ? terminal.clock() : SchedulerClock{}));
    MeterScene scene(*app);

    return example::report(check ? app->run(scene, terminal) : app->run(scene));
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
