// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "../gallery/settings.hpp"
#include "sample_theme.hpp"
#include <array>
#include <cmath>
#include <format>
#include <numbers>

namespace wasm_gallery {
/** @brief Stationary, UI-thread-only level with owning sensor values and no sensor callbacks. */
class LevelScene {
public:
  /** @param options Borrowed settings, alive through scene destruction.
   * @param theme Borrowed shared preset index, alive through scene destruction.
   */
  LevelScene(portals::ui::OptionsRef<example::Options> options, std::size_t &theme)
      : theme_(theme), scene_(portals::ui::scene([this](portals::ManagedScene &, portals::Application &) {
          return recipe();
        })) {
    using namespace portals;

    scene_.applicationOptions([options] {
      ApplicationOptions result;

      result.writer.colorMode = options.get().ansi16 ? ColorMode::ANSI16 : ColorMode::TRUE_COLOR;

      return result;
    });
    scene_.style(ui::style("Button:focused { bold: true; } .level-green { foreground: #51c878; } .level-yellow { foreground: #e5bf55; } .level-red { foreground: #ef7777; }"));
    scene_.configure([this](ManagedScene &owner, Application &app) {
      owner_ = &owner;
      app_ = &app;
      applyTheme();

      const AxisSize fixed{.preferred = 1, .mode = SizeMode::FIXED};
      GridOptions grid;

      grid.columns = {AxisSize{.mode = SizeMode::FLEX}};
      grid.rows = {fixed, AxisSize{.mode = SizeMode::FLEX}};
      grid.cells = {{title_.node().id(), 0, 0}, {body_.node().id(), 1, 0}};
      example::take(setGridOptions(app.tree(), grid_.node(), grid));
      example::take(app.tree().setEnabled(zero_.node(), false));
      example::take(app.router().setFocus(themeButton_.node()));
      paint();
    });
    scene_.onKey("F2", [this] { changeTheme(); });
    scene_.onKey("F10 | Ctrl+C", ui::quit);
  }

  LevelScene(const LevelScene &) = delete;
  LevelScene &operator=(const LevelScene &) = delete;
  LevelScene(LevelScene &&) = delete;
  LevelScene &operator=(LevelScene &&) = delete;

  /** @return Borrowed facade; this stationary sample owns all its captures. */
  portals::ui::Scene &scene() noexcept {
    return scene_;
  }

  /** @param beta Owning finite device pitch, in radians.
   * @param gamma Owning finite device roll, in radians.
   * @param angle Validated screen orientation: 0, 90, 180 or 270 degrees.
   * Values are retained only; widget work occurs at the next step boundary.
   */
  void orientation(double beta, double gamma, int angle) noexcept {
    beta_ = std::remainder(beta, TWO_PI);
    gamma_ = std::remainder(gamma, TWO_PI);
    angle_ = angle;
    state_ = READY;
    valid_ = true;
    samplePending_ = true;
    dirty_ = true;
  }

  /** @param state Validated state: 0 unavailable, 1 waiting, 2 paused. */
  void sensorState(int state) noexcept {
    state_ = state;
    dirty_ = true;
  }

  /** @brief Apply retained values between scene operations; no host callbacks or waits. */
  void beforeStep() {
    if (samplePending_) {
      samplePending_ = false;
      updateAngles();
    }

    if (dirty_ && app_) {
      paint();
      dirty_ = false;
    }
  }

private:
  static constexpr int READY = 3;
  static constexpr int INTERIOR_COLUMNS = 23;
  static constexpr int INTERIOR_ROWS = 7;
  static constexpr int CENTER_COLUMN = INTERIOR_COLUMNS / 2;
  static constexpr int CENTER_ROW = INTERIOR_ROWS / 2;
  static constexpr double TWO_PI = 2.0 * std::numbers::pi;
  static constexpr double DEGREES_PER_RADIAN = 180.0 / std::numbers::pi;
  static constexpr double FILTER_WEIGHT = 0.25;
  static constexpr double DEADZONE_DEGREES = 0.7;
  static constexpr double FULL_SCALE_DEGREES = 30.0;
  static constexpr double WARNING_DEGREES = 10.0;

  portals::ui::Description<portals::Grid> recipe() {
    using namespace portals;

    auto body = ui::vbox(body_, ui::label("========================="),
                         ui::button(themeButton_, "Theme |   F2 ", [this] { changeTheme(); }),
                         ui::button("Quit  |   F10", [] { ui::quit(); }), ui::label(""),
                         ui::label(angles_, "X: --       Y: --"), ui::label(""),
                         ui::label("┌───────────────────────┐"),
                         ui::label(rows_[0], ""), ui::label(rows_[1], ""), ui::label(rows_[2], ""),
                         ui::label(rows_[3], ""), ui::label(rows_[4], ""), ui::label(rows_[5], ""),
                         ui::label(rows_[6], ""), ui::label("└───────────────────────┘"), ui::label(""),
                         ui::button(zero_, "Zero", [this] { zero(); }), ui::label(status_, "Waiting for sensor"));

    return ui::grid(grid_, ui::label(title_, " Phone level").id("level-title"), std::move(body));
  }

  void applyTheme() {
    example::take(owner_->theme(sampleTheme(theme_, "level-title")));
  }

  void changeTheme() {
    theme_ = (theme_ + 1) % example::THEME_PRESETS.size();
    applyTheme();
  }

  void updateAngles() noexcept {
    const double deviceX = std::remainder(gamma_ - zeroGamma_, TWO_PI) * DEGREES_PER_RADIAN;
    const double deviceY = std::remainder(beta_ - zeroBeta_, TWO_PI) * DEGREES_PER_RADIAN;
    double x = deviceX;
    double y = deviceY;

    // Rotate device-relative axes into the current screen coordinate system.
    if (angle_ == 90) {
      x = deviceY;
      y = -deviceX;
    } else if (angle_ == 180) {
      x = -deviceX;
      y = -deviceY;
    } else if (angle_ == 270) {
      x = -deviceY;
      y = deviceX;
    }

    if (!filtered_ || filteredAngle_ != angle_) {
      x_ = x;
      y_ = y;
      filtered_ = true;
    } else {
      x_ += FILTER_WEIGHT * (x - x_);
      y_ += FILTER_WEIGHT * (y - y_);
    }

    filteredAngle_ = angle_;
  }

  void zero() {
    if (!valid_ || state_ != READY) {
      return;
    }

    zeroBeta_ = beta_;
    zeroGamma_ = gamma_;
    x_ = 0;
    y_ = 0;
    paint();
  }

  void setText(portals::ui::Ref<portals::Label> &label, std::string_view value) {
    example::take(portals::setControlText(app_->tree(), label.node(), example::text(value, app_->widthPolicy())));
  }

  void paint() {
    const bool ready = valid_ && state_ == READY;
    const double x = std::abs(x_) < DEADZONE_DEGREES ? 0.0 : x_;
    const double y = std::abs(y_) < DEADZONE_DEGREES ? 0.0 : y_;
    const int bubbleX = CENTER_COLUMN + static_cast<int>(std::lround(std::clamp(x / FULL_SCALE_DEGREES, -1.0, 1.0) * CENTER_COLUMN));
    const int bubbleY = CENTER_ROW + static_cast<int>(std::lround(std::clamp(y / FULL_SCALE_DEGREES, -1.0, 1.0) * CENTER_ROW));
    const double tilt = std::max(std::abs(x), std::abs(y));
    const std::string severity = tilt == 0 ? "level-green" : tilt < WARNING_DEGREES ? "level-yellow" : "level-red";

    setText(angles_, ready ? std::format("X: {:+5.1f}°  Y: {:+5.1f}°", x, y) : "X: --       Y: --");
    setText(status_, state_ == 0 ? "Sensor unavailable" : state_ == 2 ? "Paused" : ready ? "Zero sets current pose" : "Waiting for sensor");
    example::take(app_->tree().setEnabled(zero_.node(), ready));

    for (int row = 0; row < INTERIOR_ROWS; ++row) {
      std::string line = "│";

      for (int column = 0; column < INTERIOR_COLUMNS; ++column) {
        line += ready && row == bubbleY && column == bubbleX ? "●"
                : row == CENTER_ROW && column == CENTER_COLUMN ? "┼"
                : row == CENTER_ROW ? "─" : column == CENTER_COLUMN ? "│" : " ";
      }

      line += "│";
      setText(rows_[row], line);

      portals::StyleTarget target;

      target.type = "Label";

      if (ready && row == bubbleY) {
        target.classes.push_back(severity);
      }

      example::take(owner_->style(rows_[row].node(), std::move(target)));
    }
  }

  std::size_t &theme_;
  portals::ui::Ref<portals::Grid> grid_;
  portals::ui::Ref<portals::Column> body_;
  portals::ui::Ref<portals::Label> title_, angles_, status_;
  std::array<portals::ui::Ref<portals::Label>, INTERIOR_ROWS> rows_;
  portals::ui::Ref<portals::Button> themeButton_, zero_;
  portals::ManagedScene *owner_{};
  portals::Application *app_{};
  double beta_{}, gamma_{}, zeroBeta_{}, zeroGamma_{}, x_{}, y_{};
  int state_{1}, angle_{}, filteredAngle_{};
  bool valid_{}, filtered_{}, samplePending_{}, dirty_{};
  portals::ui::Scene scene_;
};
} // namespace wasm_gallery
