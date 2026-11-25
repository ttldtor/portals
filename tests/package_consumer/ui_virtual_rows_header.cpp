// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_virtual_rows.hpp>

namespace virtual_rows_header_probe {
struct Record {
  std::string_view caption;

  explicit Record(std::string_view value) : caption(value) {
  }

  Record(const Record &) = delete;
  Record(Record &&) = default;
};

struct Provider final : portals::WindowProvider<Record> {
  portals::SourceCapabilities capabilities() const noexcept override {
    return {};
  }

  std::expected<portals::SourceReadReply<Record>, portals::SourceError>
  load(const portals::SourceReadCommand &command, portals::PageBuilder<Record> &out, std::stop_token) override {
    auto result = out.emplace({1, 1}, command.dataRevision, Record{"static immutable caption"});
    if (!result) {
      return std::unexpected(result.error());
    }
    return out.ready({}, {}, portals::SourceEdge::FINAL_END);
  }
};

void qualifyVirtualRowsHeader() {
  auto source = portals::ui::model<Record>(std::make_shared<Provider>());
  auto view = portals::ui::virtualRows(source, [](auto row) {
    return portals::ui::label(row.value().caption);
  });
  (void)view;
}

} // namespace virtual_rows_header_probe
