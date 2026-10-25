// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/table_column.hpp>
#include <portals/text_format.hpp>
#include <portals/ui.hpp>

#include <array>
#include <memory>
#include <string>

namespace unicode_panels {
using namespace portals;

/** @brief Synthetic names; no directory is opened or read. */
inline constexpr std::array NAMES{
  "..", "Привет-мир.txt", "1234567890123456789012345678901234界", "1234567890123456789012345678901234👩‍💻",
  "日本語の資料.txt", "中文文件.txt", "😀-smile.txt", "👩‍💻-developer.cpp",
  "👨‍👩‍👧‍👦-family.txt", "🇯🇵-flag.txt", "👍🏽-skin-tone.txt", "❤\uFE0E-text-heart.txt", "❤️-emoji-heart.txt",
  "cafe\u0301-combining.txt", "1️⃣-keycap.txt", "·α-ambiguous.txt", "12345678901234567890123456789012345界-edge.txt",
  "12345678901234567890123456789012345👩‍💻-edge.txt", "資料😀中文👩‍💻日本語-long-long-long-long-name.txt"};

/** @brief One immutable synthetic file record, unrelated to the host filesystem. */
struct File {
  std::string name;     ///< Owning UTF-8 name used to test cell boundaries.
  std::uint64_t size{}; ///< Synthetic byte count, not a filesystem observation.
  std::string date;     ///< Fixed synthetic date shared by every run.
};

/** @brief Small immutable ready model shared by two independent table selections. */
class Files final : public ModelSnapshot<File> {
public:
  /** @brief Own all fixture rows with stable nonzero identities. */
  Files() : ModelSnapshot(1, NAMES.size()) {
    for (std::size_t index = 0; index < NAMES.size(); ++index) {
      rows_.push_back({{index + 1, 1}, {NAMES[index], index * 137, "2026-10-25"}});
    }
  }

  /** @param range Valid interval supplied by ModelRange; direct callers validate bounds.
   * @return Ready rows borrowed from this immutable snapshot through its retained lifetime.
   */
  ModelRows<File> read(RowRange range) const override {
    return {RangeState::READY, std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

  /** @param id Complete identity requested by keyboard navigation or preparation.
   * @return Constant-time fixture position, or an explicit missing identity.
   */
  RowLookup locateRow(RowId id) const override {
    return id.generation == 1 && id.value > 0 && id.value <= rows_.size()
      ? RowLookup{RowLookupState::FOUND, id.value - 1}
      : RowLookup{RowLookupState::NOT_FOUND};
  }

private:
  std::vector<ModelRow<File>> rows_;
};

/** @brief Weak UI observations also used by the deterministic scene checks. */
struct Controls {
  ui::Ref<VirtualTable<File>> left, right;    ///< Independent table focus and selection observations.
  ui::Ref<Border> leftFrame, rightFrame;      ///< Separate frame owners for geometry qualification.
  ui::Ref<Button> menu, compact, full, close; ///< Ordinary opener and popup actions.
  ui::Ref<Dialog> popup;                      ///< Retained nonmodal panel, initially closed.
  ui::Ref<Label> status;                      ///< Owning footer caption changed by popup actions.
};

/** @param error Original router/tree failure.
 * @return Managed input failure without erasing the underlying category.
 */
inline ManagedError inputError(TreeError error) {
  return {ManagedPhase::INPUT, {}, error, {}, {}};
}

/** @brief Application popup policy layered over ordinary public routing, without replacing button bindings.
 * @param app UI-only services borrowed through this synchronous scene callback.
 * @param controls Weak references retained by the owning scene.
 * @param event Input borrowed for this callback only; dispatched exactly once unless intercepted.
 * @return Consumed input or the original checked popup/focus/routing failure.
 */
inline std::expected<bool, ManagedError> routeInput(Application &app, const Controls &controls,
                                                    const InputEvent &event) {
  if (const auto *key = std::get_if<KeyEvent>(&event); key && key->action != KeyAction::RELEASE) {
    if (key->key == InputKey::F10) {
      app.requestStop();

      return true;
    }

    if (key->key == InputKey::F9) {
      auto opened = controls.popup.open().result();

      return opened.and_then([&]() -> std::expected<bool, ManagedError> {
        auto focused = controls.compact.focus();

        return focused.transform([] {
          return true;
        });
      });
    }

    if (key->key == InputKey::ESCAPE) {
      return controls.popup.dismiss().result().transform([] {
        return true;
      });
    }
  }

  auto opened = controls.popup.opened();

  if (!opened) {
    return std::unexpected(opened.error());
  }

  if (*opened) {
    if (const auto *mouse = std::get_if<MouseInputEvent>(&event);
        mouse && mouse->action == MouseAction::PRESS) {
      auto target = app.router().hitTest({mouse->column, mouse->row});

      if (!target) {
        return std::unexpected(inputError(target.error()));
      }

      auto inside = *target == Node<Widget>{} ? std::expected<bool, TreeError>(false)
                                              : app.tree().contains(controls.popup.node(), *target);

      if (!inside) {
        return std::unexpected(inputError(inside.error()));
      }

      if (!*inside) {
        auto dismissed = controls.popup.dismiss();

        if (!dismissed) {
          return std::unexpected(dismissed.error());
        }
      }
    }
  }

  // onInput runs before EventRouter; consume after this single ordinary dispatch.
  auto dispatched = app.router().dispatch(event);

  if (!dispatched) {
    return std::unexpected(inputError(dispatched.error()));
  }

  auto focused = app.router().focused();

  if (!focused) {
    return std::unexpected(inputError(focused.error()));
  }

  opened = controls.popup.opened();

  if (opened && *opened && *focused != Node<Widget>{}) {
    auto inside = app.tree().contains(controls.popup.node(), *focused);

    if (!inside) {
      return std::unexpected(inputError(inside.error()));
    }

    if (!*inside) {
      auto dismissed = controls.popup.dismiss();

      if (!dismissed) {
        return std::unexpected(dismissed.error());
      }

      // Keep the newly selected body control when dismissal retires its return record.
      auto restored = app.router().setFocus(*focused);

      if (!restored) {
        return std::unexpected(inputError(restored.error()));
      }
    }
  }

  return opened ? std::expected<bool, ManagedError>(true) : std::unexpected(opened.error());
}

/** @brief Complete native/replay scene. Settings are owned; no callbacks retain filesystem data.
 * @param options Runtime settings copied into the lazy scene; a custom clock borrow must outlive its run.
 * @param controls Optional weak observations, published only when the complete mount succeeds.
 * @return Owning lazy scene, sharing one immutable fixture model between two independent table views.
 */
inline ui::Scene form(ui::SceneOptions options = {}, Controls controls = {}) {
  auto scene = ui::scene([controls](ManagedScene &owner, Application &app)
                           -> std::expected<ui::Description<OverlayHost>, ManagedError> {
    auto model = owner.model<File>(std::make_shared<const Files>());

    if (!model) {
      return std::unexpected(model.error());
    }

    const auto policy = app.widthPolicy();
    auto columns = tableColumns(policy,
                                tableColumn("Name", &File::name, cellFormat<std::string>("{}", policy), 36),
                                tableColumn("Size", &File::size, cellFormat<std::uint64_t>("{:>7}", policy), 8),
                                tableColumn("Date", &File::date, cellFormat<std::string>("{}", policy), 10));

    if (!columns) {
      return std::unexpected(ManagedError{ManagedPhase::MOUNT, {}, TreeError::INVALID_ARGUMENT, {}, {}, columns.error()});
    }

    VirtualTableOptions table;
    table.width_policy = policy;

    auto open = [controls] {
      return controls.popup.open().result().and_then([controls] {
        return controls.compact.focus();
      });
    };
    auto choose = [controls](std::string_view mode) {
      controls.status.text() = text("Panel mode: {} | synthetic names only", mode);

      return controls.popup.dismiss().result();
    };

    // clang-format off
    return ui::overlayHost(
      ui::vbox(
        ui::hbox(ui::button(controls.menu, "Left", open), ui::button("Files", open),
          ui::button("Commands", open), ui::button("Options", open), ui::button("Right", open)),
        ui::hbox(
          ui::virtualTable(controls.left, *model, *columns, table).flexX().flexY().framed(controls.leftFrame, "double", "Left — 日本語 😀"),
          ui::virtualTable(controls.right, *model, *columns, table).flexX().flexY().framed(controls.rightFrame, "double", "Right — 中文 👩‍💻")
        ).flexY(),
        ui::label(controls.status, "F9 menu | Tab / Shift+Tab focus | arrows select | F10 quit"),
        ui::label(text("Width: emoji={} ambiguous={} | no filesystem access",
          policy.emoji == EmojiWidth::TWO ? 2 : 1, policy.ambiguous == AmbiguousWidth::NARROW ? 1 : 2))),
      ui::dialog(controls.popup, "Panel options 日本語 😀", ui::vbox(
        ui::button(controls.compact, "Compact 界", [choose] { return choose("compact"); }),
        ui::button(controls.full, "Full 👩‍💻", [choose] { return choose("full"); }),
        ui::button(controls.close, "Close / 閉じる", [controls] { return controls.popup.dismiss().result(); })).width(33),
        {.mode = DialogMode::NON_MODAL, .maximumPanelSize = {35, 8}})
        .anchor(controls.menu, DialogPlacement::BELOW_START));
    // clang-format on
  },
                         options);

  scene.theme(modernTheme());

  scene.configure([controls](ManagedScene &owner, Application &app) {
    return owner.onInput([controls, &app](const InputEvent &event) {
      return routeInput(app, controls, event);
    });
  });

  return scene;
}
} // namespace unicode_panels
