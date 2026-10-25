// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_async.hpp>

#include <expected>
#include <format>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace {
using namespace portals;

struct Corpus {
  std::vector<std::string> rows;
};

struct Summary {
  std::size_t rows{};
  std::size_t bytes{};
};

enum class ReportError {
  EMPTY_INPUT
};

using Report = std::expected<Summary, ReportError>;

std::expected<std::shared_ptr<const Report>, AsyncError> summarize(const Corpus &corpus, std::stop_token stop) {
  if (corpus.rows.empty()) {
    return std::make_shared<const Report>(std::unexpected(ReportError::EMPTY_INPUT));
  }

  Summary summary;

  for (const auto &row : corpus.rows) {
    if (stop.stop_requested()) {
      return std::unexpected(AsyncError::CANCELLED);
    }

    ++summary.rows;
    summary.bytes += row.size();
  }

  return std::make_shared<const Report>(summary);
}

UiTask showSummary(ui::AsyncContext context, std::shared_ptr<const Corpus> corpus, ui::Ref<Label> status) {
  status.text() = "Working...";

  auto completion = co_await context.background<Report>(corpus, summarize);

  if (!completion) {
    co_return std::unexpected(completion.error());
  }

  const auto &report = **completion;

  status.text() = report ? std::format("{} rows, {} bytes", report->rows, report->bytes) : "No rows to summarize";

  co_return {};
}
} // namespace

int main() {
  return ui::runApp([] {
    constexpr std::size_t SAMPLE_ROWS = 100'000;

    auto corpus = std::make_shared<const Corpus>(
      Corpus{std::vector<std::string>(SAMPLE_ROWS, "An immutable row of application data")});
    ui::Ref<Button> run;
    ui::Ref<Label> status;

    // clang-format off
    return ui::scene(ui::vbox(
      ui::label("Background summary | keep editing while it runs"),
      ui::lineEdit("This editor stays responsive"),
      ui::label(status, "Ready"),
      ui::button(run, "Summarize"),
      ui::button("Quit", [] { ui::quit(); })))
      | ui::onClick(run, ui::async([corpus, status](ui::AsyncContext context) -> UiTask {
          status.text() = "Working...";

          auto completion = co_await context.background<Report>(corpus, summarize);

          if (!completion) {
            co_return std::unexpected(completion.error());
          }

          const auto &report = **completion;

          status.text() = report
            ? std::format("{} rows, {} bytes", report->rows, report->bytes)
            : "No rows to summarize";

          co_return {};
        }))
      | ui::onKey("F2", ui::async(UiTaskRestart::CANCEL_PREVIOUS, showSummary, corpus, status))
      | ui::onKey("F10 | Ctrl+C", ui::quit);
    // clang-format on
  });
}
