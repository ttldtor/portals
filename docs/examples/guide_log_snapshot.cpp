// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_log_source_model.hpp>
#include <portals/ui_style.hpp>

#include <array>
#include <memory>
#include <memory_resource>
#include <string_view>

using namespace portals;

enum class Severity {
  INFO,
  WARNING
};

struct LogRecord {
  Severity severity;
  std::string_view message;
};

// Finite immutable domain records are supplied without filesystem access.
// These records borrow string literals, which outlive the snapshot and worker reads.
class LogSnapshot final : public ModelSnapshot<LogRecord> {
public:
  explicit LogSnapshot(std::array<ModelRow<LogRecord>, 3> rows) : ModelSnapshot(1, rows.size()), rows_(rows) {
  }

  ModelRows<LogRecord> read(RowRange range) const override {
    return {RangeState::READY, std::span(rows_).subspan(range.first, range.count), {}};
  }

private:
  const std::array<ModelRow<LogRecord>, 3> rows_;
};

int main() {
  ui::Ref<LogView> log;
  ui::LogSource application;
  ui::LogSource warnings;

  // The codec runs on a worker and only writes to its supplied workspace string.
  auto codec = [](const LogRecord &record, std::pmr::string &out) {
    out.append(record.severity == Severity::WARNING ? "[warning] " : "[info] ");
    out.append(record.message);
  };

  // clang-format off
  return ui::scene([&](ManagedScene &) {
    application = ui::logSource<LogRecord>(std::make_shared<LogSnapshot>(std::array{
      ModelRow<LogRecord>{{1, 1}, {Severity::INFO, "Application started"}},
      ModelRow<LogRecord>{{2, 1}, {Severity::WARNING, "Connection is slow"}},
      ModelRow<LogRecord>{{3, 1}, {Severity::INFO, "Request completed"}}}), codec);
    warnings = ui::logSource<LogRecord>(std::make_shared<LogSnapshot>(std::array{
      ModelRow<LogRecord>{{1, 1}, {Severity::WARNING, "Connection is slow"}},
      ModelRow<LogRecord>{{2, 1}, {Severity::WARNING, "Retry scheduled"}},
      ModelRow<LogRecord>{{3, 1}, {Severity::WARNING, "Disk space is low"}}}), codec);

    return ui::vbox(
      ui::label("F2: application | F3: warnings | F10: quit"),
      ui::logView(log, application).framed("rounded", "Log records").height(8));
  })
    | ui::theme(nordTheme())
    | ui::onKey(key::F2, [&] { log.model(application); })
    | ui::onKey(key::F3, [&] { log.model(warnings); })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
