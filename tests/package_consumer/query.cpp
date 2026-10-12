// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/model_query.hpp>

#include <array>

namespace {
struct Quote {
  int price{};
};

struct BookQuery {
  int minimumPrice{};
};

class BookSnapshot final : public portals::ModelSnapshot<Quote> {
public:
  BookSnapshot() : ModelSnapshot(19, 3) {
  }

  portals::ModelRows<Quote> read(portals::RowRange range) const override {
    return {
      portals::RangeState::READY, std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

private:
  std::array<portals::ModelRow<Quote>, 3> rows_{{{{41, 2}, {100}}, {{42, 3}, {110}}, {{43, 1}, {120}}}};
};
} // namespace

int main() {
  auto scheduler = portals::Scheduler::create();

  if (!scheduler) {
    return 1;
  }

  auto tree = portals::WidgetTree::create(**scheduler);

  if (!tree) {
    return 2;
  }

  auto root = (*tree)->root();

  if (!root) {
    return 3;
  }

  auto source = std::make_shared<const BookSnapshot>();
  auto filter = [](const Quote &quote, const BookQuery &query) {
    return quote.price >= query.minimumPrice;
  };
  auto compare = [](const Quote &left, const Quote &right, const BookQuery &) {
    return left.price > right.price;
  };
  using Provider = portals::MemoryQueryProvider<Quote, BookQuery, decltype(filter), decltype(compare)>;

  auto controller = portals::QueryController<Quote, BookQuery>::create(**tree, *root, source,
                                                                       std::make_unique<Provider>(filter, compare));

  if (!controller) {
    return 4;
  }

  auto request = (*controller)->submit({source, {105}, {0, 2}});

  if (!request) {
    return 5;
  }

  // Queue acceptance precedes application on the existing UI scheduler.
  for (int pass = 0; pass < 3; ++pass) {
    if (!(*scheduler)->dispatch()) {
      return 6;
    }
  }

  auto frame = (*controller)->pin();
  auto snapshot = (*controller)->snapshot();

  if (!frame || !snapshot || frame->rowCount() != 2 || frame->dataRevision() != 2 ||
      (*snapshot)->sourceRevision() != 19 || (*snapshot)->queryGeneration() != 1) {
    return 7;
  }

  auto rows = frame->read({0, 2});

  if (!rows || rows->rows()[0].id != portals::RowId{43, 1} || rows->rows()[1].id != portals::RowId{42, 3}) {
    return 8;
  }

  return (*controller)->close() ? 0 : 9;
}
