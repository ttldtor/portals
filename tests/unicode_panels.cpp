// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../examples/unicode_panels/scene.hpp"
#include "support/examples/replay/terminal.hpp"
#include <portals/render.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
using namespace portals;

void check(bool value, std::source_location location = std::source_location::current()) {
  if (!value) {
    throw std::runtime_error("unicode panels check line " + std::to_string(location.line()));
  }
}

template <class Error> void requireValue(bool valid, const Error *error, std::source_location location) {
  if (valid) {
    return;
  }

  std::string detail;

  if constexpr (std::same_as<Error, ManagedError>) {
    detail = " managed phase=" + std::to_string(static_cast<int>(error->phase));

    if (const auto *tree = std::get_if<TreeError>(&error->cause)) {
      detail += " tree=" + std::to_string(static_cast<int>(*tree));
    }
  } else if constexpr (std::is_enum_v<Error>) {
    detail = " error=" + std::to_string(static_cast<int>(*error));
  } else if constexpr (requires { error->kind; }) {
    detail = " kind=" + std::to_string(static_cast<int>(error->kind));
  }

  throw std::runtime_error("unicode panels value line " + std::to_string(location.line()) + detail);
}

template <class T, class Error>
T take(std::expected<T, Error> value, std::source_location location = std::source_location::current()) {
  requireValue(value.has_value(), value ? nullptr : &value.error(), location);

  return std::move(*value);
}

template <class Error>
void take(std::expected<void, Error> value, std::source_location location = std::source_location::current()) {
  requireValue(value.has_value(), value ? nullptr : &value.error(), location);
}

/** @brief Observe only after a real rendered byte transaction reaches the replay transport. */
class ObservedTerminal : public example::ReplayTerminal {
public:
  using ReplayTerminal::ReplayTerminal;
  std::function<void()> observe;

  TerminalWrite flush(TerminalDeadline deadline, std::stop_token stop) override {
    const auto result = ReplayTerminal::flush(deadline, stop);

    if (result.completed > 0 && observe) {
      observe();
    }

    return result;
  }
};

/** @brief Copy local table paint commands only for the synchronous tree frame. */
struct Commands : PaintContext {
  std::vector<RenderCommand> values;
  std::optional<NodeGeometry> geometry;

  std::expected<void, TreeError> submit(std::span<const RenderCommand> input) override {
    for (const auto &command : input) {
      if (!geometry) {
        values.push_back(command);

        continue;
      }

      std::visit(
        [&](const auto &value) {
          const auto clip = take(value.clip.translated(geometry->surface.origin())).intersection(geometry->clip);

          if constexpr (std::same_as<std::decay_t<decltype(value)>, TextCommand>) {
            values.emplace_back(TextCommand{take(translated(value.origin, geometry->surface.origin())), value.text,
                                            clip, value.style, value.z});
          } else {
            values.emplace_back(
              FillCommand{take(value.area.translated(geometry->surface.origin())), clip, value.style, value.z});
          }
        },
        command);
    }

    return {};
  }
};

CellBuffer nodeFrame(Application &app, Node<Widget> node) {
  const auto geometry = take(app.tree().inspect(node)).geometry;
  std::optional<CellBuffer> result;

  take(app.tree().frame([&] {
    Commands commands;
    take(app.tree().paint(node, commands));
    result = take(renderFullFrame(geometry.surface.size(), commands.values, app.widthPolicy()));
  }));

  return std::move(*result);
}

void paintSubtree(Application &app, Node<Widget> node, Commands &commands) {
  const auto snapshot = take(app.tree().inspect(node));

  if (!snapshot.visible || !snapshot.geometry.valid) {
    return;
  }

  commands.geometry = snapshot.geometry;
  take(app.tree().paint(node, commands));

  for (auto child : snapshot.children) {
    paintSubtree(app, child, commands);
  }
}

CellBuffer sceneFrame(Application &app) {
  const auto geometry = take(app.tree().inspect(app.root())).geometry;
  std::optional<CellBuffer> result;

  take(app.tree().frame([&] {
    Commands commands;
    paintSubtree(app, app.root(), commands);
    result = take(renderFullFrame(geometry.surface.size(), commands.values, app.widthPolicy()));
  }));

  return std::move(*result);
}

CellBuffer tableFrame(Application &app, ui::Ref<VirtualTable<unicode_panels::File>> reference) {
  return nodeFrame(app, reference.node());
}

std::string cells(const CellBuffer &frame) {
  std::string result;

  for (int row = 0; row < frame.size().height; ++row) {
    for (int column = 0; column < frame.size().width; ++column) {
      const auto cell = take(frame.at({column, row}));

      if (cell.cell.kind == CellKind::CONTINUATION) {
        check(column > 0);
        const auto lead = take(frame.at({column - 1, row}));
        check(lead.cell.columns == 2 && lead.glyph == cell.glyph && lead.style == cell.style);
      } else {
        check(cell.cell.columns == 1 || cell.cell.columns == 2);
        check(column + cell.cell.columns <= frame.size().width);
        result += cell.glyph;
      }
    }

    result += '\n';
  }

  return result;
}

void checkBorder(Application &app, ui::Ref<Border> reference) {
  const auto frame = nodeFrame(app, reference.node());
  cells(frame);

  if (frame.size().width >= 2 && frame.size().height >= 2) {
    check(take(frame.at({0, 0})).glyph == "╔");
    check(take(frame.at({frame.size().width - 1, 0})).glyph == "╗");
    check(take(frame.at({0, frame.size().height - 1})).glyph == "╚");
    check(take(frame.at({frame.size().width - 1, frame.size().height - 1})).glyph == "╝");
  }
}

void captureCells(std::ostream &output, const CellBuffer &frame, CellPoint origin) {
  output << "{\"x\":" << origin.x << ",\"y\":" << origin.y << ",\"width\":" << frame.size().width
         << ",\"height\":" << frame.size().height << ",\"cells\":[";
  bool comma{};

  for (int row = 0; row < frame.size().height; ++row) {
    for (int column = 0; column < frame.size().width; ++column) {
      const auto value = take(frame.at({column, row}));
      const auto color = [&](const std::optional<RgbColor> &rgb) {
        if (rgb) {
          output << ((static_cast<unsigned>(rgb->red) << 16) | (static_cast<unsigned>(rgb->green) << 8) | rgb->blue);
        } else {
          output << "null";
        }
      };

      output << (comma ? "," : "") << "[" << std::quoted(std::string(value.glyph)) << ","
             << static_cast<int>(value.cell.columns) << ",";
      color(value.style.foreground);
      output << ",";
      color(value.style.background);
      output << "," << value.style.bold << "," << value.style.italic << "," << value.style.underline << "]";
      comma = true;
    }
  }

  output << "]}";
}

void captureNode(std::ostream &output, Application &app, Node<Widget> node) {
  const auto geometry = take(app.tree().inspect(node)).geometry;
  captureCells(output, nodeFrame(app, node), geometry.surface.origin());
}

void captureFrame(const std::filesystem::path &directory, Application &app, const unicode_panels::Controls &controls,
                  const ObservedTerminal &terminal) {
  if (directory.empty()) {
    return;
  }

  std::filesystem::create_directories(directory);
  const auto prefix = directory / std::to_string(app.now());
  std::ofstream vt(prefix.string() + ".vt", std::ios::binary);
  vt.write(terminal.output.data(), static_cast<std::streamsize>(terminal.output.size()));
  check(vt.good());
  std::ofstream expected(prefix.string() + ".json", std::ios::binary);
  const auto size = take(terminal.size());
  expected << "{\"time\":" << app.now() << ",\"width\":" << size.width << ",\"height\":" << size.height
           << ",\"popup\":" << (take(controls.popup.opened()) ? "true" : "false") << ",\"tables\":[";
  captureNode(expected, app, controls.left.node());
  expected << ",";
  captureNode(expected, app, controls.right.node());
  expected << "],\"borders\":[";
  captureNode(expected, app, controls.leftFrame.node());
  expected << ",";
  captureNode(expected, app, controls.rightFrame.node());
  expected << "]";

  if (take(controls.popup.opened())) {
    const auto popup = take(app.tree().inspect(controls.popup.node()));
    check(!popup.children.empty());
    expected << ",\"popupFrame\":";
    captureNode(expected, app, popup.children.front());
  }

  expected << ",\"full\":";
  captureCells(expected, sceneFrame(app), {});
  expected << "}";
  check(expected.good());
}

void replay(EmojiWidth emoji, const std::filesystem::path &capture) {
  // The batches exercise selection, reverse traversal, popup restore, outside click and real focus departure.
  ObservedTerminal terminal({{40, "\x1b[B\t\x1b[Z\x1b[B", {}},
                             {80, "\x1b[<0;3;1M\x1b[<0;3;1m", {}},
                             {120, "\x1b", {}},
                             {160, "\x1b[20~", {}},
                             {200, "\x1b[Z", {}},
                             {240, "\x1b[20~", {}},
                             {280, "\x1b[<0;80;8M", {}},
                             {320, "", CellSize{65, 16}},
                             {360, "", CellSize{3, 2}},
                             {400, "", CellSize{120, 28}},
                             {440, "\x1b[20~", {}},
                             {480, "\r", {}},
                             {520, "\x1b[21~", {}}},
                            600, {120, 28});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  options.application.widthPolicy.emoji = emoji;
  unicode_panels::Controls controls;
  Application *app{};
  auto scene = unicode_panels::form(options, controls);
  scene.configure([&](ManagedScene &, Application &application) {
    app = &application;

    return controls.left.focus();
  });
  std::string initial, restored;
  std::optional<SchedulerTime> capturedAt;
  bool sawPopup{}, sawEscape{}, sawFocusLeave{}, sawOutside{}, sawTiny{}, sawRestored{}, checkedColumns{}, sawChoice{};
  bool sawWideOverlap{}, sawWideRestored{};

  terminal.observe = [&] {
    const auto now = app->now();
    const auto opened = take(controls.popup.opened());
    auto frame = tableFrame(*app, controls.left);
    const auto content = cells(frame);
    cells(tableFrame(*app, controls.right));
    checkBorder(*app, controls.leftFrame);
    checkBorder(*app, controls.rightFrame);
    const auto complete = sceneFrame(*app);
    cells(complete);

    for (int y = 0; y < complete.size().height; ++y) {
      for (int x = 0; x < complete.size().width; ++x) {
        check(!take(complete.at({x, y})).style.underline);
      }
    }

    if (capturedAt != now) {
      captureFrame(capture, *app, controls, terminal);
      capturedAt = now;
    }

    if (now < 40 && initial.empty()) {
      initial = content;
    }

    if (now >= 40 && now < 80 && frame.size().width >= 54) {
      check(take(app->router().focused()) == Node<Widget>(controls.left.node()));
      take(app->tree().read(controls.left.node(), [](const auto &table) {
        check(table.selection().has_value());
      }));
      // Every visible numeric field begins at the fixed cell column, regardless of name UTF-8 length.
      for (int row = 1; row < std::min(frame.size().height, 18); ++row) {
        const auto value = take(frame.at({36, row}));
        check(value.cell.kind != CellKind::CONTINUATION);
      }

      checkedColumns = true;
      restored = content;
    }

    if (now >= 80 && now < 120) {
      sawPopup |= opened;
      constexpr int PREFIX_CELLS = 34;
      constexpr int BODY_ROW = 3; // Header plus the third synthetic name.
      const auto tableGeometry = take(app->tree().inspect(controls.left.node())).geometry;
      const CellPoint lead{tableGeometry.surface.origin().x + PREFIX_CELLS,
                           tableGeometry.surface.origin().y + BODY_ROW};
      const auto popup = take(app->tree().inspect(controls.popup.node()));
      const auto panel = take(app->tree().inspect(popup.children.front())).geometry.surface;
      check(panel.right() - 1 == lead.x && panel.contains(lead) && !panel.contains({lead.x + 1, lead.y}));
      check(take(complete.at(lead)).glyph == "│");
      const auto uncovered = take(complete.at({lead.x + 1, lead.y}));
      const auto neighbour = take(complete.at({lead.x + 2, lead.y}));
      check(uncovered.glyph == " " && uncovered.cell.kind != CellKind::CONTINUATION);
      check(uncovered.style.background && uncovered.style.background == neighbour.style.background);
      sawWideOverlap = true;
    }

    if (now >= 120 && now < 160) {
      sawEscape |= !opened;
      check(content == restored);
      const auto tableGeometry = take(app->tree().inspect(controls.left.node())).geometry;
      const CellPoint lead{tableGeometry.surface.origin().x + 34, tableGeometry.surface.origin().y + 3};
      const auto value = take(complete.at(lead));
      check(value.glyph == "界" && value.cell.columns == 2);
      check(take(complete.at({lead.x + 1, lead.y})).cell.kind == CellKind::CONTINUATION);
      sawWideRestored = true;
    }

    if (now >= 200 && now < 240) {
      sawFocusLeave |= !opened && take(app->router().focused()) == Node<Widget>(controls.right.node());
    }

    if (now >= 280 && now < 320) {
      sawOutside |= !opened && take(app->router().focused()) == Node<Widget>(controls.right.node());
    }

    if (now >= 360 && now < 400) {
      sawTiny = true;
    }

    if (now >= 400) {
      sawRestored |= frame.size().width == 58 && content == restored;
    }

    if (now >= 480) {
      sawChoice |= !opened && take(controls.status.text().get()).starts_with("Panel mode: compact");
    }
  };

  take(scene.run(terminal));
  check(terminal.closed);
  check(!initial.empty());
  check(sawPopup && sawEscape);
  check(sawFocusLeave && sawOutside);
  check(sawTiny && sawRestored);
  check(checkedColumns && sawChoice);
  check(sawWideOverlap && sawWideRestored);
}
} // namespace

int main(int argc, char **argv) {
  try {
    (void)argc;
    (void)argv;
#ifdef _WIN32
    char *capture{};
    std::size_t captureSize{};
    check(_dupenv_s(&capture, &captureSize, "PORTALS_UNICODE_CAPTURE_DIR") == 0);
    const std::unique_ptr<char, decltype(&std::free)> captureOwner(capture, &std::free);
#else
    const auto *capture = std::getenv("PORTALS_UNICODE_CAPTURE_DIR");
#endif
    replay(EmojiWidth::TWO, capture ? std::filesystem::path(capture) : std::filesystem::path{});
    replay(EmojiWidth::ONE, {});
    std::cout << "Unicode panels: both width policies, clipping and popup replay passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
