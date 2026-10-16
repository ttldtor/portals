// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <array>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace range_test {
struct BorrowedRecipes {
  portals::ui::Description<portals::Label> *first;
  portals::ui::Description<portals::Label> *last;

  auto begin() const {
    return first;
  }

  auto end() const {
    return last;
  }
};
} // namespace range_test

namespace std::ranges {
template <> inline constexpr bool enable_borrowed_range<range_test::BorrowedRecipes> = true;
} // namespace std::ranges

namespace {
using namespace portals;
using Recipes = std::vector<ui::Description<Label>>;

template <class R>
concept CanBox = requires(R &&range) { ui::vbox(std::forward<R>(range)); };
static_assert(!CanBox<Recipes &>);
static_assert(!CanBox<const Recipes &>);
static_assert(!CanBox<const Recipes>);
static_assert(CanBox<Recipes>);
static_assert(!CanBox<std::ranges::ref_view<Recipes>>);
static_assert(!CanBox<std::span<ui::Description<Label>>>);
static_assert(CanBox<decltype(std::views::as_rvalue(std::declval<Recipes &>()))>);
static_assert(!CanBox<std::vector<int>>);
static_assert(!CanBox<range_test::BorrowedRecipes>);
static_assert(CanBox<decltype(std::views::as_rvalue(std::declval<range_test::BorrowedRecipes>()))>);
static_assert(requires {
  ui::hbox();
  ui::vbox(ui::label("legacy"));
});

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <class T, class E> T take(std::expected<T, E> result) {
  check(result.has_value(), "unexpected API failure");

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
};

struct Payload {
  int &live;
  int value;

  Payload(int &count, int number) : live(count), value(number) {
    ++live;
  }

  ~Payload() {
    --live;
  }
};

struct Owned final : Widget {
  std::unique_ptr<Payload> payload;

  explicit Owned(std::unique_ptr<Payload> value) : payload(std::move(value)) {
  }
};

void temporarySinglePassRange() {
  int live{};
  Fixture fixture;
  ui::Description<Column> stored;
  std::array<ui::NodeRef<Owned>, 3> refs;
  std::weak_ptr<int> transformCapture;
  int visits{};

  {
    std::istringstream input("4 8 15");
    auto capture = std::make_shared<int>(0);

    transformCapture = capture;
    stored = ui::vbox(std::ranges::istream_view<int>(input) |
                      std::views::transform([capture = std::move(capture), &live, &visits, &refs](int value) {
                        ++*capture;

                        return ui::custom<Owned>(std::make_unique<Payload>(live, value)).reference(refs[visits++]);
                      }));
  }

  check(transformCapture.expired() && visits == 3 && live == 3, "range consumed now; transform not retained");
  check(refs[0].node() == Node<Owned>{}, "construction publishes no ref");

  const auto root = take(ui::mount(*fixture.scene, std::move(stored)));
  const auto children = take(fixture.app->tree().inspect(root)).children;
  const std::array expected{4, 8, 15};

  check(children.size() == refs.size(), "single-pass source retains all recipes");

  for (std::size_t i = 0; i < refs.size(); ++i) {
    check(children[i] == refs[i].node(), "range order is stable");
    take(fixture.app->tree().read(refs[i].node(), [&](const Owned &widget) {
      check(widget.payload->value == expected[i], "move-only constructor payload survives the source");
    }));
  }

  take(fixture.scene->remove(root));
  check(live == 0, "removed subtree releases all owned payloads");
}

void explicitTransferAndEmptyRanges() {
  Fixture fixture;
  Recipes children;
  ui::NodeRef<Label> first;

  children.push_back(ui::label("first").reference(first));
  children.push_back(ui::label("second"));

  auto source = children | std::views::as_rvalue;
  LinearOptions options;

  options.gap = 2;

  auto recipe = ui::hbox(options, source);

  check(!ui::mount(*fixture.scene, std::move(children.front())), "explicit source transfer consumes each recipe");

  const auto row = take(ui::mount(*fixture.scene, std::move(recipe)));

  check(take(fixture.app->tree().inspect(row)).children.front() == first.node(), "hbox accepts explicit transfer");
  take(fixture.app->tree().read(row, [](const Row &widget) {
    check(widget.options().gap == 2, "range overload preserves layout options");
  }));

  Recipes empty;
  const auto column = take(ui::mount(*fixture.scene, ui::vbox(std::move(empty))));

  check(take(fixture.app->tree().inspect(column)).children.empty(), "empty range mounts empty layout");
}

void partialMountRollback() {
  int live{};
  Fixture fixture;
  const auto previous = take(ui::mount(*fixture.scene, ui::label("previous")));
  std::array<ui::Ref<Owned>, 3> refs;
  ui::Ref<Column> parent;
  std::vector<ui::Description<Owned>> children;
  int mounts{};

  for (int i = 0; i < 3; ++i) {
    children.push_back(ui::custom<Owned>(std::make_unique<Payload>(live, i))
                         .reference(refs[static_cast<std::size_t>(i)])
                         .setup([i, &mounts](ManagedScene &, Node<Owned> node) -> std::expected<void, ManagedError> {
                           ++mounts;

                           if (i == 1) {
                             return std::unexpected(
                               ManagedError{ManagedPhase::BIND, node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
                           }

                           return {};
                         }));
  }

  const auto failed = ui::mount(*fixture.scene, ui::vbox(std::move(children)).reference(parent));

  check(!failed && failed.error().phase == ManagedPhase::BIND &&
          std::get<TreeError>(failed.error().cause) == TreeError::INVALID_ARGUMENT && !failed.error().cleanup,
        "item N preserves original typed error");
  check(mounts == 2 && live == 0, "failed subtree and unmounted remaining recipes are released");
  check(parent.node() == Node<Column>{}, "failed parent reference not published");

  for (const auto &ref : refs) {
    check(ref.node() == Node<Owned>{}, "failed range child reference not published");
  }

  check(take(fixture.app->tree().inspect(fixture.scene->root())).children == std::vector<Node<Widget>>{previous},
        "rollback preserves previous scene nodes");
  take(ui::mount(*fixture.scene, ui::custom<Owned>(std::make_unique<Payload>(live, 42)).reference(refs[0])));
  check(refs[0].node() != Node<Owned>{}, "rollback releases reference reservations for reuse");
  take(fixture.scene->close());
  check(live == 0, "scene close releases replacement payload");
}

void childCreationFailure() {
  Fixture fixture;
  std::array<ui::NodeRef<Label>, 3> refs;
  Recipes children;

  children.push_back(ui::label("first").reference(refs[0]));
  children.push_back(ui::label(std::string_view("\xff", 1)).reference(refs[1]));
  children.push_back(ui::label("unreached").reference(refs[2]));

  const auto failed = ui::mount(*fixture.scene, ui::vbox(std::move(children)));

  check(!failed && failed.error().phase == ManagedPhase::MOUNT && failed.error().text.has_value() &&
          std::get<TreeError>(failed.error().cause) == TreeError::INVALID_ARGUMENT,
        "item N creation preserves typed UTF-8 diagnostic");
  check(take(fixture.app->tree().inspect(fixture.scene->root())).children.empty(),
        "creation failure removes the successful prefix and parent");

  for (const auto &ref : refs) {
    check(ref.node() == Node<Label>{}, "creation failure publishes no partial refs");
  }
}

void iterationFailureCleanup() {
  int live{};
  bool thrown{};

  try {
    auto recipe = ui::vbox(std::views::iota(0, 3) | std::views::transform([&live](int i) {
                             if (i == 1) {
                               throw std::runtime_error("range factory failure");
                             }

                             return ui::custom<Owned>(std::make_unique<Payload>(live, i));
                           }));
  } catch (const std::runtime_error &) {
    thrown = true;
  }

  check(thrown && live == 0, "construction failure releases already consumed recipes");
}
} // namespace

int main() {
  try {
    temporarySinglePassRange();
    explicitTransferAndEmptyRanges();
    partialMountRollback();
    childCreationFailure();
    iterationFailureCleanup();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }

  std::cout << "owning range contracts passed\n";

  return 0;
}
