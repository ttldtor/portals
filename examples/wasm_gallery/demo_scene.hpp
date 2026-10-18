// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "../gallery/common.hpp"
#include "../gallery/settings.hpp"
#include "../gallery/themes.hpp"
#include "sample_theme.hpp"
#include <portals/ui_style.hpp>

namespace wasm_gallery {
/** @brief Stationary browser demo; its owning scene cancels timers before borrowed state retires. */
class DemoScene {
public:
  /** @param options Borrowed display settings, retained until this demo is destroyed.
   * @param theme Shared preset index, retained until this demo is destroyed.
   */
  explicit DemoScene(portals::ui::OptionsRef<example::Options> options, std::size_t &theme)
      : options_(options), theme_(theme), scene_(portals::ui::scene([this](portals::ManagedScene &, portals::Application &) {
          return recipe();
        })) {
    using namespace portals;

    scene_.applicationOptions([options] {
      ApplicationOptions result;

      result.writer.colorMode = options.get().ansi16 ? ColorMode::ANSI16 : ColorMode::TRUE_COLOR;

      return result;
    });
    scene_.style(ui::style("Button:focused { bold: true; }"));
    scene_.configure([this](ManagedScene &owner, Application &app) {
      owner_ = &owner;
      app_ = &app;
      scope_ = example::take(app.scheduler().makeScope());
      applyTheme(theme_);
      configureGrid();
      example::take(app.router().setFocus(editor_.node()));
    });
    scene_.onKey("F2", [this] {
      changeTheme();
    });
    scene_.onKey("F10 | Ctrl+C", ui::quit);
    scene_.onStop([this] {
      // Stop is UI-affine and precedes managed node retirement, including failed startup.
      timer_ = {};
      static_cast<void>(scope_.close());
      running_ = false;
    });
  }

  DemoScene(const DemoScene &) = delete;
  DemoScene &operator=(const DemoScene &) = delete;
  DemoScene(DemoScene &&) = delete;
  DemoScene &operator=(DemoScene &&) = delete;

  /** @return Borrowed scene; this stationary demo must outlive every active runtime step. */
  portals::ui::Scene &scene() noexcept {
    return scene_;
  }

private:
  static constexpr std::string_view WORD = "something";
  static constexpr portals::SchedulerTime ANIMATION_INTERVAL_MS = 300;

  portals::ui::Description<portals::Grid> recipe() {
    using namespace portals;

    ui::LineEditConfig editorConfig;

    editorConfig.options.editMode = EditMode::EXPLICIT;
    editorConfig.policy.width.mode = SizeMode::FLEX;

    auto nameRow = ui::hbox(ui::label("Name : "),
                            ui::lineEdit(editor_, "Portals", editorConfig));
    auto themeButton = ui::button("Theme |   F2 ", [this] {
      changeTheme();
    });
    auto runButton = ui::button(run_, "Run something", [this] {
      startAnimation();
    });
    auto body = ui::vbox(body_, ui::label("================="), std::move(themeButton),
                         ui::button("Quit  |   F10", [] { ui::quit(); }), ui::label(""), std::move(nameRow),
                         ui::checkbox("Alerts"), ui::label(""), std::move(runButton), ui::label(""),
                         ui::label(animation_, "   (S)omething"));

    return ui::grid(grid_, ui::label(title_, " Portals gallery").id("gallery-title"), std::move(body));
  }

  void configureGrid() {
    using namespace portals;

    const AxisSize fixedRow{.preferred = 1, .mode = SizeMode::FIXED};
    GridOptions grid;

    grid.columns = {AxisSize{.mode = SizeMode::FLEX}};
    grid.rows = {fixedRow, AxisSize{.mode = SizeMode::FLEX}};
    grid.cells = {{title_.node().id(), 0, 0}, {body_.node().id(), 1, 0}};

    example::take(setGridOptions(app_->tree(), grid_.node(), grid));
  }

  void applyTheme(std::size_t index) {
    example::take(owner_->theme(sampleTheme(index, "gallery-title")));
    theme_ = index;
  }

  void changeTheme() {
    applyTheme((theme_ + 1) % example::THEME_PRESETS.size());
  }

  void startAnimation() {
    if (running_) {
      return;
    }

    index_ = 0;
    returning_ = false;
    running_ = true;

    example::take(app_->tree().setEnabled(run_.node(), false));
    updateAnimation();
    scheduleNext();
  }

  void scheduleNext() {
    timer_ = example::take(app_->scheduler().at(scope_, app_->now() + ANIMATION_INTERVAL_MS, [this] {
      if (returning_) {
        --index_;
      } else {
        ++index_;
        returning_ = index_ == WORD.size() - 1;
      }

      updateAnimation();

      if (returning_ && index_ == 0) {
        running_ = false;
        example::take(app_->tree().setEnabled(run_.node(), true));

        return;
      }

      scheduleNext();
    }));
  }

  void updateAnimation() {
    std::string caption = "   ";

    caption.append(WORD.substr(0, index_));
    caption += '(';
    caption += static_cast<char>(WORD[index_] - 'a' + 'A');
    caption += ')';
    caption.append(WORD.substr(index_ + 1));
    example::take(portals::setControlText(app_->tree(), animation_.node(),
                                          example::text(caption, app_->widthPolicy())));
  }

  portals::ui::OptionsRef<example::Options> options_;
  portals::ui::Ref<portals::Grid> grid_;
  portals::ui::Ref<portals::Column> body_;
  portals::ui::Ref<portals::LineEdit> editor_;
  portals::ui::Ref<portals::Button> run_;
  portals::ui::Ref<portals::Label> title_, animation_;
  portals::ManagedScene *owner_{};
  portals::Application *app_{};
  portals::UiScope scope_;
  portals::Subscription timer_;
  std::size_t &theme_;
  std::size_t index_{};
  bool running_{}, returning_{};
  portals::ui::Scene scene_;
};
} // namespace wasm_gallery
